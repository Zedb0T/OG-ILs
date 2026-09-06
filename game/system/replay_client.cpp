#include "replay_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#include <curl/curl.h>

#include "common/log/log.h"
#include "third-party/json.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace replay_client {
namespace {
using json = nlohmann::json;
constexpr int kCustomLimit = 8;
// Keep saved IDs 0-4 stable: Default gains two opponents; the old single
// next-faster behavior remains explicitly selectable as mode 5.
constexpr int kLastRaceMode = 5;
constexpr size_t kSnapshotBudget = 64 * 1024 * 1024;
constexpr int kLeaderboardPageSize = 8;
constexpr size_t kLeaderboardCachePages = 512;
constexpr size_t kLeaderboardCacheBytes = 4 * 1024 * 1024;
constexpr int kLeaderboardWarmPages = 12; // 96 rows, within the public API's 100-row limit
constexpr int kLeaderboardWarmAge = 15 * 60;
constexpr const char* kLeaderboardGroups[] = {"all", "main", "orb", "side"};
constexpr const char* kLeaderboardTitles[] = {
    "All Missions", "Main Missions", "Orb Searches", "Other Side Missions", "Individual Missions"};
constexpr const char* kLeaderboardDescriptions[] = {
    "Combined points across every mission", "Combined points from the main adventure",
    "Combined points from every orb search", "Combined points from side challenges",
    "Browse every mission and its individual standings"};

// The browser owns its navigation, not the save-game inventory. History is at
// most home -> catalog -> mission, and retains the catalog page and cursor.
struct LeaderboardLocation {
  int screen = 0; // 0 home, 1 group points, 2 mission catalog, 3 mission standings
  int group = 0, page = 0, cursor = 0, total = 0;
  std::string mission, label;
  std::string board_key() const {
    return std::to_string(screen) + ":" + kLeaderboardGroups[group] + ":" + mission;
  }
  std::string key() const { return board_key() + ":" + std::to_string(page); }
};

struct LeaderboardRow {
  int place = 0, points = 0, count = 0, records = 0;
  bool own = false;
  std::string name, time, gap, mission, group;
};
struct LeaderboardPage {
  std::vector<LeaderboardRow> rows;
  std::string wr = "--", status;
  int total = 0, ranked_missions = 0;
  bool valid = false;
  bool from_disk = false, offline = false;
  int64_t fetched_at = 0;
  LeaderboardLocation location;
  json data; // validated, public fields only; also used to revalidate disk loads
  uint64_t used = 0;
  std::chrono::steady_clock::time_point retry_after{};
};

int64_t leaderboard_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string leaderboard_age(int64_t stamp) {
  const auto age = std::max<int64_t>(0, leaderboard_now() - stamp);
  if (age < 60) return std::to_string(age) + "s";
  if (age < 3600) return std::to_string(age / 60) + "m";
  if (age < 86400) return std::to_string(age / 3600) + "h";
  return std::to_string(age / 86400) + "d";
}

bool category_ok(const std::string& value) {
  return std::regex_match(value, std::regex("[A-Za-z0-9_-]{1,96}"));
}
bool id_ok(const std::string& value) {
  return std::regex_match(value, std::regex("[a-f0-9]{32}"));
}
std::string leaderboard_label(std::string value, size_t limit) {
  for (auto& ch : value) if (ch < 32 || ch > 126 || ch == '~') ch = '_';
  if (value.empty()) value = "Unknown";
  if (value.size() > limit) value = value.substr(0, limit - 3) + "...";
  return value;
}
int leaderboard_int(const json& object, const char* key, int max) {
  const auto& value = object.at(key);
  if (!value.is_number_integer() || value < 0 || value > max)
    throw std::runtime_error("Invalid leaderboard integer");
  return value.get<int>();
}
std::vector<std::string> selected_ids(const json& custom, const std::string& category) {
  std::vector<std::string> result;
  if (!custom.is_object()) return result;
  const auto found = custom.find(category);
  if (found == custom.end() || !found->is_array()) return result;
  for (const auto& entry : *found) {
    if (!entry.is_string()) continue;
    const auto& id = entry.get_ref<const std::string&>();
    if (id_ok(id) && std::find(result.begin(), result.end(), id) == result.end())
      result.push_back(id);
    if (result.size() == kCustomLimit) break;
  }
  return result;
}
json normalize_custom(const json& custom) {
  json result = json::object();
  if (custom.is_object()) {
    for (auto it = custom.begin(); it != custom.end(); ++it)
      result[it.key()] = selected_ids(custom, it.key());
  }
  return result;
}
std::string time_label(float seconds) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3) << seconds << 's';
  return output.str();
}
std::string random_hex(size_t bytes) {
  std::random_device rng;
  std::string out;
  for (size_t i = 0; i < bytes; ++i) {
    const auto value = rng() & 255;
    out += "0123456789abcdef"[value >> 4];
    out += "0123456789abcdef"[value & 15];
  }
  return out;
}

void save_json(const fs::path& path, const json& data) {
  fs::create_directories(path.parent_path());
  const auto temporary = fs::path(path.string() + ".tmp");
  {
    std::ofstream file(temporary.string(), std::ios::binary | std::ios::trunc);
    file << data.dump(2);
    file.flush();
    if (!file) throw std::runtime_error("Cannot save ghost settings");
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace ghost settings");
#else
  fs::rename(temporary, path);
#endif
}

struct ResponseBuffer {
  std::string contents;
  size_t limit;
};
size_t receive(char* bytes, size_t size, size_t count, void* user) {
  auto& buffer = *static_cast<ResponseBuffer*>(user);
  auto& output = buffer.contents;
  if (size && count > buffer.limit / size) return 0;
  const auto length = size * count;
  if (length > buffer.limit - output.size()) return 0;
  output.append(bytes, length);
  return length;
}

// Remote servers require HTTPS. Never follow redirects with player credentials;
// bounded transfers run off the game thread and retain normal TLS verification.
std::string request(const std::string& base, const std::string& path,
                    const std::string& body = "", const std::string& player = "",
                    const std::string& token = "", const std::atomic<bool>* interrupt = nullptr) {
  auto* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("HTTP initialization failed");
  ResponseBuffer response{{}, path.rfind("/api/v1/", 0) == 0 ? 256 * 1024 : replay::kMaxFileBytes};
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (!player.empty()) headers = curl_slist_append(headers, ("X-Player-ID: " + player).c_str());
  if (!token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
  const auto url = base + path;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_PROXY, "");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, path.rfind("/api/v1/", 0) == 0 ? 10000L : 30000L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (interrupt) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, interrupt);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
        +[](void* state, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
          return static_cast<const std::atomic<bool>*>(state)->load() ? 1 : 0;
        });
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
  }
  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) throw std::runtime_error("Ghost server unavailable");
  if (status < 200 || status >= 300) throw std::runtime_error("Ghost server rejected request (" + std::to_string(status) + ")");
  return response.contents;
}

LeaderboardPage parse_leaderboard(const json& data, const LeaderboardLocation& location,
                                  const std::string& player) {
  LeaderboardPage result;
  if (data.at("api_version") != 1 || data.at("source") != "ghosts" ||
      data.at("game") != "jak3" || data.at("group") != kLeaderboardGroups[location.group] ||
      data.at("offset") != location.page * kLeaderboardPageSize ||
      !data.at("items").is_array() || data.at("items").size() > kLeaderboardPageSize)
    throw std::runtime_error("Unexpected leaderboard response");
  result.total = leaderboard_int(data, "total", location.screen == 2 ? 512 : 1000000);
  result.ranked_missions = leaderboard_int(data, "ranked_missions", 512);
  if (data.at("items").size() != static_cast<size_t>(std::clamp(
          result.total - location.page * kLeaderboardPageSize, 0, kLeaderboardPageSize)))
    throw std::runtime_error("Incomplete leaderboard page");
  const auto read_record = [](const json& value) {
    const auto seconds = value.is_null() ? 0.0f : value.get<float>();
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 601)
      throw std::runtime_error("Invalid leaderboard record");
    return seconds;
  };
  float wr = 0;
  if (location.screen == 3) {
    const auto& mission = data.at("mission");
    if (mission.at("mission_id") != location.mission ||
        (result.total > 0 && mission.at("wr_seconds").is_null()))
      throw std::runtime_error("Unexpected leaderboard mission");
    wr = read_record(mission.at("wr_seconds"));
    if (!mission.at("wr_seconds").is_null()) result.wr = time_label(wr);
  }
  for (const auto& item : data.at("items")) {
    LeaderboardRow row;
    if (location.screen == 2) {
      row.mission = item.at("mission_id").get<std::string>();
      row.group = item.at("group").get<std::string>();
      if (!category_ok(row.mission) || (row.group != "main" && row.group != "orb" && row.group != "side") ||
          std::any_of(result.rows.begin(), result.rows.end(), [&](const auto& r) { return r.mission == row.mission; }))
        throw std::runtime_error("Invalid mission catalog row");
      row.name = leaderboard_label(item.at("label").get<std::string>(), 64);
      row.count = leaderboard_int(item, "player_count", 1000000);
      const auto seconds = read_record(item.at("wr_seconds"));
      if (row.count > 0 && item.at("wr_seconds").is_null())
        throw std::runtime_error("Missing mission record");
      row.time = item.at("wr_seconds").is_null() ? "--" : time_label(seconds);
      result.rows.push_back(std::move(row));
      continue;
    }
    row.place = leaderboard_int(item, location.screen == 1 ? "rank" : "place", result.total);
    row.points = leaderboard_int(item, "points", location.screen == 1 ? 51200 : 100);
    const auto pid = item.at("player_id").get<std::string>();
    if (!id_ok(pid) || row.place < 1)
      throw std::runtime_error("Invalid leaderboard row");
    row.own = pid == player;
    row.name = leaderboard_label(item.at("display_name").get<std::string>(), 23);
    if (location.screen == 1) {
      row.count = leaderboard_int(item, "mission_count", 512);
      row.records = leaderboard_int(item, "tied_wr_count", row.count) +
                    leaderboard_int(item, "untied_wr_count", row.count);
      if (row.records > row.count || row.points > row.count * 100)
        throw std::runtime_error("Invalid aggregate standings");
    } else {
      const auto duration = item.at("duration_seconds").get<float>();
      if (!std::isfinite(duration) || duration < wr || duration > 601)
        throw std::runtime_error("Invalid leaderboard time");
      row.time = time_label(duration);
      row.gap = duration == wr ? "WR" : "+" + time_label(duration - wr);
    }
    result.rows.push_back(std::move(row));
  }

  // Persist only fields the native browser understands, never unknown response
  // fields, credentials, replay payloads, or a cached identity-dependent YOU flag.
  result.location = location;
  result.data = {{"api_version", 1}, {"source", "ghosts"}, {"game", "jak3"},
                 {"group", kLeaderboardGroups[location.group]},
                 {"offset", location.page * kLeaderboardPageSize},
                 {"total", result.total}, {"ranked_missions", result.ranked_missions},
                 {"items", json::array()}};
  if (location.screen == 3)
    result.data["mission"] = {{"mission_id", location.mission},
                             {"wr_seconds", data.at("mission").at("wr_seconds")}};
  for (size_t i = 0; i < result.rows.size(); ++i) {
    const auto& row = result.rows[i];
    const auto& item = data.at("items").at(i);
    json stored;
    if (location.screen == 2) {
      stored = {{"mission_id", row.mission}, {"group", row.group}, {"label", row.name},
                {"player_count", row.count}, {"wr_seconds", item.at("wr_seconds")}};
    } else {
      stored = {{"player_id", item.at("player_id")}, {"display_name", row.name}, {"points", row.points}};
      if (location.screen == 1) {
        stored["rank"] = row.place;
        stored["mission_count"] = row.count;
        stored["tied_wr_count"] = item.at("tied_wr_count");
        stored["untied_wr_count"] = item.at("untied_wr_count");
      } else {
        stored["place"] = row.place;
        stored["duration_seconds"] = item.at("duration_seconds");
      }
    }
    result.data["items"].push_back(std::move(stored));
  }
  result.valid = true;
  return result;
}

class Client {
 public:
  Client() : root(file_util::get_user_features_dir(GameVersion::Jak3)), config_path(root / "ghost-client.json") {
    if (fs::exists(config_path)) {
      // Never silently replace a damaged identity and create a different player.
      config = json::parse(file_util::read_text_file(config_path));
    } else {
      config = {{"player_id", random_hex(16)}, {"player_token", random_hex(32)},
                {"server", kSparkedHostServer}, {"mode", 0},
                {"submit_completed", true}, {"custom", json::object()}};
      save_json(config_path, config);
    }
    player = config.at("player_id").get<std::string>();
    token = config.at("player_token").get<std::string>();
    base = config.at("server").get<std::string>();
    if (!id_ok(player) || !std::regex_match(token, std::regex("[a-f0-9]{64}")) ||
        !std::regex_match(base, std::regex("(http://127\\.0\\.0\\.1:[0-9]{1,5}|https://[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]{1,5})?)")))
      throw std::runtime_error("Invalid ghost-client.json identity/server");
    mode = std::clamp(config.value("mode", 0), 0, kLastRaceMode);
    // Older menu reads inserted null entries into this map. Repair only the
    // selections; retain player identity, endpoints and all replay files.
    const auto custom = normalize_custom(config.value("custom", json::object()));
    if (!config.contains("custom") || config.at("custom") != custom) {
      config["custom"] = custom;
      save_json(config_path, config);
    }
    // Restore before any boot ping or network job, without disk I/O on GOAL.
    leaderboard_restore();
    worker = std::thread([this] { work(); });
  }
  ~Client() {
    { std::lock_guard lock(mutex); stopping = true; jobs.clear(); leaderboard_warm_interrupt = true; }
    wake.notify_one();
    if (worker.joinable()) worker.join();
  }

  fs::path local(const std::string& category, const char* filename) const {
    return root / "replays" / category / filename;
  }
  fs::path cache_directory(const std::string& server) const {
    // Collision-free, filesystem-safe server namespace. Split long custom URLs
    // so no individual path component exceeds filesystem filename limits.
    std::string encoded;
    for (unsigned char ch : server) {
      encoded += "0123456789abcdef"[ch >> 4];
      encoded += "0123456789abcdef"[ch & 15];
    }
    auto path = root / "ghost-cache" / "servers";
    for (size_t i = 0; i < encoded.size(); i += 120) path /= encoded.substr(i, 120);
    return path;
  }
  std::shared_ptr<const replay::File> load_local(const std::string& category, bool last) {
    auto path = local(category, last ? "last-attempt.ogr.json" : "best-completed.ogr.json");
    if (!last && !fs::exists(path)) path = local(category, "last-completed.ogr.json");
    if (!fs::exists(path)) return {};
    auto file = std::make_shared<replay::File>(replay::load(path));
    if (file->game != "jak3" || file->category != category || (!last && (!file->completed || file->truncated)))
      throw std::runtime_error("Invalid local ghost");
    return file;
  }
  void enqueue(std::function<void()> job) {  // caller owns mutex
    if (jobs.size() >= 8) throw std::runtime_error("Ghost work queue full; retry later");
    jobs.push_back(std::move(job));
    leaderboard_warm_interrupt = true; // foreground work preempts an idle download
    wake.notify_one();
  }
  void work() {
    for (;;) {
      std::function<void()> job;
      bool warming = false;
      {
        std::unique_lock lock(mutex);
        for (;;) {
          if (stopping) return;
          if (!jobs.empty()) { job = std::move(jobs.front()); jobs.pop_front(); break; }
          if (leaderboard_warm_armed && !leaderboard_warm_done && leaderboard_cache_revision == server_revision) {
            if (std::chrono::steady_clock::now() >= leaderboard_warm_after) {
              leaderboard_warm_interrupt = false;
              job = leaderboard_warm_job();
              if (job) { warming = leaderboard_warm_active = true; break; }
              leaderboard_warm_done = true;
            } else {
              wake.wait_until(lock, leaderboard_warm_after);
              continue;
            }
          }
          wake.wait(lock);
        }
      }
      try { job(); }
      catch (const std::exception& error) {
        std::lock_guard lock(mutex);
        if (warming) leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        else status = error.what();
      }
      if (warming) { std::lock_guard lock(mutex); leaderboard_warm_active = false; }
    }
  }
  void register_player(const std::string& server) {
    request(server, "/players", json({{"player_id", player}, {"token", token}}).dump());
  }
  bool ping() { // caller owns mutex; no GOAL pointers or HTTP on the game thread
    const auto now = std::chrono::steady_clock::now();
    if (ping_pending || now < next_ping) return false;
    const auto server = base;
    const auto generation = server_revision;
    enqueue([this, server, generation] {
      { std::lock_guard lock(mutex); if (generation != server_revision) return; }
      std::string name, result_status;
      bool identified = false, received_identity = false;
      try {
        const auto reply = json::parse(request(server, "/players/ping",
            json({{"player_id", player}, {"token", token}}).dump()));
        if (reply.at("player_id").get<std::string>() != player)
          throw std::runtime_error("Server returned a different player");
        name = reply.at("display_name").get<std::string>();
        identified = reply.at("identified").get<bool>();
        // Server-controlled names must never become GOAL text directives.
        for (auto& ch : name) if (ch < 32 || ch > 126 || ch == '~') ch = '_';
        if (name.size() > 40) name.resize(40);
        received_identity = true;
        result_status = identified ? "Ping sent" : "Ping sent - waiting for an admin to assign your name";
      } catch (const std::exception& error) {
        result_status = std::string("Ping failed: ") + error.what();
      }
      std::lock_guard lock(mutex);
      if (generation != server_revision) return;
      ping_pending = false;
      // A transient network error must not erase a name already verified on
      // this server. A successful unknown response still clears a removed name.
      if (received_identity) {
        player_identified = identified;
        player_name = std::move(name);
      }
      ping_status = std::move(result_status);
    });
    identity_requested = true;
    ping_pending = true;
    next_ping = now + std::chrono::seconds(3);
    ping_status = "Pinging server...";
    return true;
  }
  void upload(std::string contents, std::string category) { // caller owns mutex
    // Jobs keep the destination selected when they were submitted. Switching
    // servers must never redirect an already queued upload to another host.
    const auto server = base;
    const auto generation = server_revision;
    enqueue([this, server, generation, contents = std::move(contents)] {
      std::string result_status;
      bool submitted = false;
      try {
        register_player(server);
        request(server, "/replays", contents, player, token);
        result_status = "Submitted replay";
        submitted = true;
      } catch (const std::exception& error) {
        result_status = error.what();
      }
      std::lock_guard lock(mutex);
      if (generation != server_revision) return;
      status = std::move(result_status);
      if (submitted) prepared_category.clear(); // next start refreshes rankings
    });
    status = "Submitting replay...";
  }
  void refresh(const std::string& category, bool force = false) { // caller owns mutex
    if (!category_ok(category)) return;
    if (!force && prepared_category == category) return;
    const auto generation = ++revision;
    const auto server = base;
    const auto selected_mode = mode;
    // Never convert an unchecked JSON null/non-array on the GOAL caller's stack.
    const auto selected = selected_ids(config.at("custom"), category);
    const auto offset = page * 100;
    prepared_category = category;
    prepared.clear();
    ready = false;
    status = "Loading ghosts...";
    enqueue([this, server, category, generation, selected_mode, selected, offset] {
      { std::lock_guard lock(mutex); if (generation != revision) return; }
      std::vector<Ghost> result;
      json rows = json::array();
      std::string result_status = "Ready";
      bool more = false;
      try {
        auto best = (selected_mode == 0 || selected_mode == 1 || selected_mode == 5) ? load_local(category, false) : nullptr;
        if (selected_mode == 1 || selected_mode == 3) {
          auto file = selected_mode == 3 ? load_local(category, true) : best;
          if (file) result.push_back({file, selected_mode == 3 ? "Last Attempt" : "Personal Best"});
        } else {
          const auto listing = json::parse(request(server, "/replays?game=jak3&category=" + category + "&offset=" + std::to_string(offset)));
          rows = listing.at("replays");
          more = !listing.at("next_offset").is_null();
          json choices = json::array();
          if (selected_mode == 4) {
            // IDs survive renames and page changes; metadata refreshes names.
            for (const auto& id : selected) {
              if (!id_ok(id)) continue;
              auto match = std::find_if(rows.begin(), rows.end(), [&](const json& row) { return row.at("id") == id; });
              choices.push_back(match != rows.end() ? *match : json::parse(request(server, "/replays/" + id + "/metadata")));
            }
          } else {
            const auto policy = selected_mode == 2 ? "wr" : selected_mode == 0 ? "two-faster" : "default";
            auto url = "/selection?category=" + category + "&player_id=" + player + "&mode=" + policy;
            if (best) url += "&best_seconds=" + std::to_string(best->duration_seconds);
            choices = json::parse(request(server, url)).at("replays");
          }
          size_t memory = 0;
          const size_t max_choices = selected_mode == 4 ? kCustomLimit : selected_mode == 0 ? 2 : 1;
          if (!choices.is_array() || choices.size() > max_choices)
            throw std::runtime_error("Too many ghosts returned for race mode");
          for (const auto& row : choices) {
            const auto id = row.at("id").get<std::string>();
            if (!id_ok(id)) throw std::runtime_error("Invalid replay ID from server");
            const auto cache = cache_directory(server) / (id + ".ogr.json");
            std::shared_ptr<replay::File> file;
            if (fs::exists(cache)) file = std::make_shared<replay::File>(replay::load(cache));
            else {
              file = std::make_shared<replay::File>(replay::parse(request(server, "/replays/" + id)));
              replay::atomic_save(cache, *file);
            }
            if (file->category != category || file->game != "jak3" ||
                (selected_mode != 4 && (!file->completed || file->truncated)))
              throw std::runtime_error("Server ghost does not match mission/mode");
            memory += file->samples.size() * sizeof(replay::Sample);
            if (memory > kSnapshotBudget) throw std::runtime_error("Custom ghosts exceed 64 MiB limit");
            auto label = row.at("display_name").get<std::string>();
            // Native game text contains formatting directives; don't interpret
            // untrusted names as GOAL font control codes or non-ASCII bytes.
            for (auto& c : label) if (c < 32 || c > 126 || c == '~') c = '_';
            if (label.size() > 40) label.resize(40);
            label += " " + time_label(file->duration_seconds);
            result.push_back({file, label});
          }
          if ((selected_mode == 0 || selected_mode == 5) && result.empty() && best) result.push_back({best, "Personal Best"});
        }
        if (result.empty()) result_status = "No replay available for this mode";
      } catch (const std::exception& error) {
        result.clear();
        result_status = error.what();
        if (selected_mode == 0 || selected_mode == 5) {
          try { auto best = load_local(category, false); if (best) result.push_back({best, "Personal Best (offline)"}); }
          catch (...) {}
        }
      }
      std::lock_guard lock(mutex);
      if (generation != revision) return; // stale category/mode responses never win
      prepared = std::move(result);
      catalog = std::move(rows);
      has_more = more;
      ready = true;
      status = std::move(result_status);
    });
  }

  // All cache file I/O is worker-owned. The envelope binds its public metadata
  // to the exact server and protocol, even if a file is copied to another host's
  // directory. Invalid snapshots are ignored as a unit, not partially loaded.
  void leaderboard_restore() { // caller owns mutex (or is the constructor)
    if (leaderboard_cache_revision == server_revision || leaderboard_cache_loading || jobs.size() >= 8) return;
    const auto server = base;
    const auto generation = server_revision;
    enqueue([this, server, generation] {
      std::map<std::string, LeaderboardPage> restored;
      uint64_t used = 0;
      try {
        const auto path = cache_directory(server) / "leaderboards-v1.json";
        std::ifstream file(path, std::ios::binary);
        if (file) {
          // Bounded read, including if the file grows between opening and read.
          std::string raw(kLeaderboardCacheBytes + 1, '\0');
          file.read(raw.data(), static_cast<std::streamsize>(raw.size()));
          if (file.bad() || file.gcount() > static_cast<std::streamsize>(kLeaderboardCacheBytes))
            throw std::runtime_error("Leaderboard cache too large or unreadable");
          raw.resize(static_cast<size_t>(file.gcount()));
          const auto envelope = json::parse(raw);
          if (envelope.at("version") != 1 || envelope.at("server") != server ||
              envelope.at("source") != "ghosts" || envelope.at("game") != "jak3" ||
              envelope.at("page_size") != kLeaderboardPageSize || !envelope.at("pages").is_array() ||
              envelope.at("pages").size() > kLeaderboardCachePages)
            throw std::runtime_error("Incompatible leaderboard cache");
          for (const auto& entry : envelope.at("pages")) {
            const auto& stored = entry.at("location");
            LeaderboardLocation location;
            location.screen = leaderboard_int(stored, "screen", 3);
            location.group = leaderboard_int(stored, "group", 3);
            location.page = leaderboard_int(stored, "page", 124999);
            location.mission = stored.at("mission").get<std::string>();
            if (location.screen < 1 ||
                (location.screen != 1 && location.group != 0) ||
                (location.screen == 3 ? !category_ok(location.mission) : !location.mission.empty()) ||
                (location.screen == 2 && location.page >= 512 / kLeaderboardPageSize))
              throw std::runtime_error("Invalid cached leaderboard location");
            const auto& fetched = entry.at("fetched_at");
            if (!fetched.is_number_integer() || fetched <= 0 || fetched > leaderboard_now() + 300)
              throw std::runtime_error("Invalid cached leaderboard timestamp");
            auto page = parse_leaderboard(entry.at("data"), location, player);
            page.fetched_at = fetched.get<int64_t>();
            page.from_disk = true;
            page.used = ++used;
            // Always refresh on the first visit this session, regardless of
            // wall-clock age. Last-good data remains usable however old it is.
            if (!restored.emplace(location.key(), std::move(page)).second)
              throw std::runtime_error("Duplicate cached leaderboard page");
          }
        }
      } catch (const std::exception&) {
        restored.clear();
        lg::warn("Ignoring invalid or unreadable leaderboard disk cache");
      }
      std::lock_guard lock(mutex);
      if (generation != server_revision) return;
      leaderboard_pages = std::move(restored);
      leaderboard_cache_clock = used;
      leaderboard_cache_revision = generation;
      leaderboard_cache_loading = false;
      leaderboard_visit(); // publish cached rows before queueing a fresh GET
    });
    leaderboard_cache_loading = true;
  }

  json leaderboard_disk_snapshot(const std::string& server) const { // caller owns mutex
    std::vector<const LeaderboardPage*> ordered;
    for (const auto& [key, page] : leaderboard_pages) if (page.valid) ordered.push_back(&page);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->used < b->used; });
    json entries = json::array();
    for (const auto* page : ordered) {
      const auto& location = page->location;
      entries.push_back({{"location", {{"screen", location.screen}, {"group", location.group},
                                     {"page", location.page}, {"mission", location.mission}}},
                         {"fetched_at", page->fetched_at}, {"data", page->data}});
    }
    return {{"version", 1}, {"server", server}, {"source", "ghosts"}, {"game", "jak3"},
            {"page_size", kLeaderboardPageSize}, {"pages", std::move(entries)}};
  }

  void leaderboard_save_snapshot(const std::string& server, const json& snapshot) {
    try {
      if (snapshot.dump(2).size() > kLeaderboardCacheBytes)
        throw std::runtime_error("Leaderboard disk snapshot too large");
      save_json(cache_directory(server) / "leaderboards-v1.json", snapshot);
    } catch (const std::exception&) {
      lg::warn("Cannot save leaderboard disk cache; live standings remain available");
    }
  }

  void leaderboard_start_warming() { // caller owns mutex; boot HUD or explicit warm request
    if (leaderboard_warm_armed || !config.value("prefetch_leaderboards", true)) return;
    leaderboard_warm_armed = true;
    leaderboard_warm_epoch = leaderboard_now() - kLeaderboardWarmAge;
    leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    wake.notify_one();
  }

  // Selected only while the normal work queue is empty. Batch requests fill
  // twelve native pages at a time; a pass is bounded by the cache capacity.
  // Cached recent pages are skipped on restart. No GOAL pointers or credentials.
  std::function<void()> leaderboard_warm_job() { // caller owns mutex, on worker
    if (leaderboard_warm_seen.size() >= kLeaderboardCachePages || leaderboard_pages.size() >= kLeaderboardCachePages)
      return {};
    std::optional<LeaderboardLocation> candidate;
    const auto consider = [&](LeaderboardLocation location) {
      if (candidate || leaderboard_warm_seen.count(location.key())) return;
      const auto first = leaderboard_pages.find(location.key());
      bool fresh = first != leaderboard_pages.end() && first->second.valid;
      const auto count = fresh ? std::min(kLeaderboardWarmPages,
          std::max(1, (first->second.total + kLeaderboardPageSize - 1) / kLeaderboardPageSize - location.page)) : 1;
      for (int i = 0; fresh && i < count; ++i) {
        auto page = location;
        page.page += i;
        const auto found = leaderboard_pages.find(page.key());
        fresh = found != leaderboard_pages.end() && found->second.valid && found->second.fetched_at >= leaderboard_warm_epoch;
      }
      if (fresh) leaderboard_warm_seen.insert(location.key());
      else candidate = location;
    };
    const auto more_pages = [&](LeaderboardLocation location) {
      const auto first = leaderboard_pages.find(location.key());
      if (first == leaderboard_pages.end() || !first->second.valid) return;
      const auto pages = std::min<int>(kLeaderboardCachePages,
          (first->second.total + kLeaderboardPageSize - 1) / kLeaderboardPageSize);
      for (int page = kLeaderboardWarmPages; !candidate && page < pages; page += kLeaderboardWarmPages) {
        location.page = page;
        consider(location);
      }
    };
    for (int group = 0; group < 4; ++group) {
      LeaderboardLocation location;
      location.screen = 1; location.group = group;
      consider(location);
    }
    LeaderboardLocation catalog_location;
    catalog_location.screen = 2;
    consider(catalog_location);
    if (!candidate) more_pages(catalog_location);

    // Catalog metadata is sufficient to represent an empty board. Preserve its
    // timestamp: an old catalog must never manufacture a fresh empty result.
    std::vector<LeaderboardPage> empty_pages;
    std::vector<LeaderboardLocation> missions;
    if (!candidate) {
      const auto first = leaderboard_pages.find(catalog_location.key());
        const int count = first == leaderboard_pages.end() ? 0 :
            (first->second.total + kLeaderboardPageSize - 1) / kLeaderboardPageSize;
      for (int page = 0; page < count; ++page) {
        catalog_location.page = page;
        const auto found = leaderboard_pages.find(catalog_location.key());
        if (found == leaderboard_pages.end() || !found->second.valid) continue;
        const auto& catalog_page = found->second;
        for (const auto& row : catalog_page.rows) {
          LeaderboardLocation location;
          location.screen = 3; location.mission = row.mission; location.label = row.name;
          if (row.count) { missions.push_back(location); continue; }
          const auto existing = leaderboard_pages.find(location.key());
          if (existing != leaderboard_pages.end() && existing->second.valid &&
              existing->second.fetched_at >= catalog_page.fetched_at) continue;
          auto empty = parse_leaderboard({{"api_version", 1}, {"game", "jak3"}, {"source", "ghosts"},
              {"group", "all"}, {"offset", 0}, {"total", 0}, {"ranked_missions", catalog_page.ranked_missions},
              {"mission", {{"mission_id", row.mission}, {"wr_seconds", nullptr}}}, {"items", json::array()}}, location, player);
          empty.fetched_at = catalog_page.fetched_at;
          empty.from_disk = catalog_page.from_disk;
          empty.status = "CATALOG / No recorded runs";
          empty.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(60);
          empty_pages.push_back(std::move(empty));
        }
      }
      for (const auto& location : missions) consider(location);
      for (int group = 0; !candidate && group < 4; ++group) {
        LeaderboardLocation location;
        location.screen = 1; location.group = group;
        more_pages(location);
      }
      for (const auto& location : missions) if (!candidate) more_pages(location);
    }
    if (!candidate && empty_pages.empty()) return {};
    const auto server = base;
    const auto generation = server_revision;
    return [this, server, generation, candidate, empty_pages = std::move(empty_pages)]() mutable {
      bool failed = false;
      std::vector<LeaderboardPage> pages;
      try {
        if (candidate) {
          const auto& location = *candidate;
          const auto path = location.screen == 1 ? "/api/v1/leaderboards/points" :
              location.screen == 2 ? std::string("/api/v1/missions") :
              "/api/v1/missions/" + location.mission + "/leaderboard";
          const auto raw = request(server, path + "?source=ghosts&game=jak3&group=" + kLeaderboardGroups[location.group] +
              "&limit=" + std::to_string(kLeaderboardWarmPages * kLeaderboardPageSize) +
              "&offset=" + std::to_string(location.page * kLeaderboardPageSize), "", "", "", &leaderboard_warm_interrupt);
          const auto data = json::parse(raw);
          const auto total = leaderboard_int(data, "total", location.screen == 2 ? 512 : 1000000);
          if (data.at("offset") != location.page * kLeaderboardPageSize || !data.at("items").is_array() ||
              data.at("items").size() != static_cast<size_t>(std::clamp(total - location.page * kLeaderboardPageSize,
                  0, kLeaderboardWarmPages * kLeaderboardPageSize)))
            throw std::runtime_error("Invalid prefetch batch");
          const auto& items = data.at("items");
          for (int offset = 0; offset < std::max<int>(1, items.size()); offset += kLeaderboardPageSize) {
            auto page_location = location;
            page_location.page += offset / kLeaderboardPageSize;
            auto chunk = data;
            chunk["offset"] = page_location.page * kLeaderboardPageSize;
            chunk["items"] = json::array();
            for (int i = offset; i < std::min<int>(items.size(), offset + kLeaderboardPageSize); ++i)
              chunk["items"].push_back(items.at(i));
            auto page = parse_leaderboard(chunk, page_location, player);
            page.fetched_at = leaderboard_now();
            page.status = "LIVE STANDINGS  /  60s cache";
            page.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            pages.push_back(std::move(page));
          }
        }
      } catch (const std::exception&) { failed = true; }
      json snapshot;
      {
        std::lock_guard lock(mutex);
        if (generation != server_revision) return;
        if (leaderboard_warm_interrupt.load()) {
          leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
          return;
        }
        if (failed) {
          const auto seconds = std::min(300, 30 << std::min(leaderboard_warm_failures++, 4));
          leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
          return; // no partial batch or failed result replaces last-good data
        }
        if (candidate) leaderboard_warm_seen.insert(candidate->key());
        leaderboard_warm_failures = 0;
        // Put real standings first. Warming never evicts an already cached page.
        for (auto& empty : empty_pages) pages.push_back(std::move(empty));
        for (auto& page : pages) {
          const auto key = page.location.key();
          const auto existing = leaderboard_pages.find(key);
          if (existing == leaderboard_pages.end() && leaderboard_pages.size() >= kLeaderboardCachePages) continue;
          if (existing != leaderboard_pages.end() && existing->second.valid && existing->second.fetched_at > page.fetched_at) continue;
          page.used = ++leaderboard_cache_clock;
          if (leaderboard.board_key() == page.location.board_key()) leaderboard.total = page.total;
          leaderboard_pages[key] = std::move(page);
        }
        snapshot = leaderboard_disk_snapshot(server);
        leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      }
      leaderboard_save_snapshot(server, snapshot);
    };
  }

  // Inventory's read-only leaderboard uses the same bounded worker, never the
  // GOAL thread. Cache keys include board/group/mission/page; switching views
  // during a request cannot publish that response into the new view. 512 LRU
  // pages, 60s TTL, 15s failure backoff, 5s manual refresh cooldown.
  void leaderboard_refresh(bool force = false) { // caller owns mutex
    if (leaderboard_cache_revision != server_revision) { leaderboard_restore(); return; }
    if (leaderboard.screen == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const auto found = leaderboard_pages.find(leaderboard.key());
    if (leaderboard_pending || jobs.size() >= 8 ||
        (force ? now < leaderboard_manual_after :
         found != leaderboard_pages.end() && now < found->second.retry_after)) return;
    const auto location = leaderboard;
    const auto server = base;
    const auto generation = server_revision;
    enqueue([this, location, server, generation] {
      LeaderboardPage result;
      try {
        const auto path = location.screen == 1 ? "/api/v1/leaderboards/points" :
            location.screen == 2 ? std::string("/api/v1/missions") :
            "/api/v1/missions/" + location.mission + "/leaderboard";
        const auto raw = request(server, path + "?source=ghosts&game=jak3&group=" +
            kLeaderboardGroups[location.group] + "&limit=" + std::to_string(kLeaderboardPageSize) +
            "&offset=" + std::to_string(location.page * kLeaderboardPageSize));
        if (raw.size() > 256 * 1024) throw std::runtime_error("Leaderboard response too large");
        result = parse_leaderboard(json::parse(raw), location, player);
        result.fetched_at = leaderboard_now();
        result.valid = true;
        result.status = "LIVE STANDINGS  /  60s cache";
        result.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      } catch (const std::exception&) {
        result = LeaderboardPage{}; // never publish a partially validated page
        result.status = "Offline - press Square to retry";
        result.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      }
      json snapshot;
      {
        std::lock_guard lock(mutex);
        if (generation != server_revision) return;
        const auto save = result.valid;
        auto previous = leaderboard_pages.find(location.key());
        if (!result.valid && previous != leaderboard_pages.end() && previous->second.valid) {
          previous->second.offline = true;
          previous->second.retry_after = result.retry_after;
        } else {
          if (result.valid && leaderboard.board_key() == location.board_key()) {
            leaderboard.total = result.total;
            leaderboard.page = std::min(leaderboard.page, leaderboard_page_count() - 1);
            if (leaderboard.page == location.page)
              leaderboard.cursor = std::min(leaderboard.cursor, std::max(0, static_cast<int>(result.rows.size()) - 1));
          }
          if (leaderboard_pages.size() >= kLeaderboardCachePages && previous == leaderboard_pages.end())
            leaderboard_pages.erase(std::min_element(leaderboard_pages.begin(), leaderboard_pages.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; }));
          result.used = ++leaderboard_cache_clock;
          leaderboard_pages[location.key()] = std::move(result);
        }
        if (save) snapshot = leaderboard_disk_snapshot(server);
      }
      if (!snapshot.is_null()) leaderboard_save_snapshot(server, snapshot);
      std::lock_guard lock(mutex);
      if (generation == server_revision) leaderboard_pending = false;
    });
    leaderboard_pending = true;
    leaderboard_manual_after = now + std::chrono::seconds(5);
  }

  const LeaderboardPage* leaderboard_view() const {
    const auto found = leaderboard_pages.find(leaderboard.key());
    return found == leaderboard_pages.end() ? nullptr : &found->second;
  }

  int leaderboard_page_count() const {
    return std::max(1, (leaderboard.total + kLeaderboardPageSize - 1) / kLeaderboardPageSize);
  }

  void leaderboard_visit() {
    const auto found = leaderboard_pages.find(leaderboard.key());
    if (found != leaderboard_pages.end()) {
      found->second.used = ++leaderboard_cache_clock;
      if (found->second.valid) leaderboard.total = found->second.total;
    }
    leaderboard_refresh();
  }

  int leaderboard_enter() {
    LeaderboardLocation next;
    if (leaderboard.screen == 0) {
      next.screen = leaderboard.cursor == 4 ? 2 : 1;
      next.group = leaderboard.cursor == 4 ? 0 : leaderboard.cursor;
    } else if (leaderboard.screen == 2) {
      const auto* p = leaderboard_view();
      if (!p || !p->valid || leaderboard.cursor >= static_cast<int>(p->rows.size())) return 0;
      const auto& row = p->rows[leaderboard.cursor];
      next.screen = 3;
      next.mission = row.mission;
      next.label = row.name;
    } else return 0;
    leaderboard_history.push_back(leaderboard);
    leaderboard = next;
    leaderboard_visit();
    return 1;
  }

  int leaderboard_back() {
    if (leaderboard.screen == 0) return 0;
    leaderboard = leaderboard_history.empty() ? LeaderboardLocation{} : leaderboard_history.back();
    if (!leaderboard_history.empty()) leaderboard_history.pop_back();
    leaderboard_visit();
    return 1;
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> jobs;
  bool stopping = false, ready = false, has_more = false;
  fs::path root, config_path;
  json config, catalog = json::array();
  std::string player, token, base, prepared_category, status = "Ready";
  std::string player_name, ping_status;
  bool player_identified = false, ping_pending = false, identity_requested = false;
  std::chrono::steady_clock::time_point next_ping{};
  std::vector<Ghost> prepared;
  int mode = 0, page = 0, revision = 0, server_revision = 0;
  std::thread worker;
  std::map<std::string, LeaderboardPage> leaderboard_pages;
  LeaderboardLocation leaderboard;
  std::vector<LeaderboardLocation> leaderboard_history;
  uint64_t leaderboard_cache_clock = 0;
  bool leaderboard_pending = false;
  bool leaderboard_cache_loading = false;
  int leaderboard_cache_revision = -1;
  std::chrono::steady_clock::time_point leaderboard_manual_after{};
  bool leaderboard_warm_armed = false, leaderboard_warm_done = false, leaderboard_warm_active = false;
  int leaderboard_warm_failures = 0;
  int64_t leaderboard_warm_epoch = 0;
  std::atomic<bool> leaderboard_warm_interrupt{false};
  std::set<std::string> leaderboard_warm_seen;
  std::chrono::steady_clock::time_point leaderboard_warm_after{};
};

Client& client() { static Client instance; return instance; }
}  // namespace

ServerStatus server_status() {
  try {
    auto& c = client();
    std::lock_guard lock(c.mutex);
    return {c.base, c.status};
  } catch (const std::exception& error) {
    return {"", error.what()};
  }
}

bool set_server(Server server) {
  if (server != Server::SparkedHost && server != Server::Localhost) return false;
  try {
    auto& c = client();
    std::lock_guard lock(c.mutex);
    const std::string next_server = server == Server::Localhost ? kLocalhostServer : kSparkedHostServer;
    if (c.base == next_server) return true;
    auto next_config = c.config;
    // Preserve legacy custom selections as the current server's selections,
    // and restore them when returning. Replay IDs belong to one server only.
    next_config["custom_by_server"][c.base] = next_config.value("custom", json::object());
    next_config["custom"] = normalize_custom(
        next_config["custom_by_server"].value(next_server, json::object()));
    next_config["server"] = next_server;
    // Save before changing live state; a disk error leaves the old selection intact.
    try {
      save_json(c.config_path, next_config);
    } catch (const std::exception& error) {
      c.status = error.what();
      return false;
    }
    const auto category = c.prepared_category;
    c.config = std::move(next_config);
    c.base = next_server;
    ++c.revision;
    ++c.server_revision;
    c.leaderboard_warm_interrupt = true;
    c.leaderboard_warm_done = false;
    c.leaderboard_warm_seen.clear();
    c.leaderboard_warm_failures = 0;
    c.leaderboard_warm_epoch = leaderboard_now() - kLeaderboardWarmAge;
    c.leaderboard_warm_after = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    c.leaderboard_pages.clear();
    c.leaderboard_pending = false;
    c.leaderboard_cache_loading = false;
    c.leaderboard_restore();
    c.leaderboard = {};
    c.leaderboard_history.clear();
    c.leaderboard_manual_after = {};
    c.player_name.clear();
    c.player_identified = false;
    c.identity_requested = false;
    c.ping_pending = false;
    c.ping_status.clear();
    c.next_ping = {};
    c.prepared_category.clear();
    c.prepared.clear();
    c.catalog = json::array();
    c.ready = false;
    c.has_more = false;
    c.page = 0;
    c.status = "Server changed - retry mission to apply";
    // A full queue is temporary: leave the category invalid so prepare retries.
    if (c.jobs.size() < 8) c.refresh(category);
    return true;
  } catch (const std::exception& error) {
    lg::warn("replay client server selection: {}", error.what());
    return false;
  }
}

int command(int operation, int value, const std::string& category) {
  try {
    auto& c = client();
    std::lock_guard lock(c.mutex);
    switch (operation) {
      case 0: return c.mode;
      case 1:
        c.mode = std::clamp(value, 0, kLastRaceMode); c.config["mode"] = c.mode;
        c.prepared_category.clear(); c.page = 0;
        save_json(c.config_path, c.config); c.refresh(category); return c.mode;
      case 2: c.page = 0; c.refresh(category, true); return 1;
      case 3: return static_cast<int>(c.catalog.size());
      case 5: {
        if (category != c.prepared_category || value < 0 || value >= static_cast<int>(c.catalog.size())) return 0;
        const auto id = c.catalog.at(value).at("id").get<std::string>();
        auto& selected = c.config["custom"][category];
        if (!selected.is_array()) selected = json::array();
        auto found = std::find(selected.begin(), selected.end(), id);
        if (found != selected.end()) selected.erase(found);
        else if (selected.size() < kCustomLimit) selected.push_back(id);
        else { c.status = "Select at most 8 ghosts"; return 0; }
        save_json(c.config_path, c.config); c.refresh(category, true); return 1;
      }
      case 6: case 7: {
        if (!category_ok(category)) return 0;
        auto file = c.load_local(category, operation == 7);
        if (!file) { c.status = "No local replay to submit"; return 0; }
        c.upload(replay::serialize(*file), category); return 1;
      }
      case 8: if (c.has_more) ++c.page; c.refresh(category, true); return 1;
      case 9: c.page = std::max(0, c.page - 1); c.refresh(category, true); return 1;
      case 10: return c.config.value("submit_completed", true);
      case 11: c.config["submit_completed"] = value != 0; save_json(c.config_path, c.config); return 1;
      case 15:
        if (!category_ok(category)) return 0;
        c.config["custom"][category] = json::array();
        save_json(c.config_path, c.config); c.refresh(category, true); return 1;
      case 16: return c.ping() ? 1 : 0;
      // Called every HUD frame: contact the selected server once per session
      // or server change, not every mission retry. Manual ping can retry errors.
      case 17:
        c.leaderboard_start_warming();
        return !c.identity_requested && c.ping() ? 1 : 0;
      case 20:
        if (value >= 0 && c.leaderboard.screen != 0) {
          const auto page = std::clamp(value, 0, c.leaderboard_page_count() - 1);
          if (page != c.leaderboard.page) {
            c.leaderboard.page = page;
            c.leaderboard.cursor = 0;
            c.leaderboard_visit();
          }
        }
        c.leaderboard_refresh(); return c.leaderboard.page;
      case 21: c.leaderboard_refresh(true); return 1;
      case 22: return c.leaderboard_page_count();
      case 23: { const auto* p = c.leaderboard_view(); return p ? static_cast<int>(p->rows.size()) : 0; }
      case 24: case 25: {
        const auto* p = c.leaderboard_view();
        if (!p || value < 0 || value >= static_cast<int>(p->rows.size())) return 0;
        return operation == 24 ? p->rows[value].own : p->rows[value].place;
      }
      case 26: { const auto* p = c.leaderboard_view(); return p && p->valid; }
      case 27: return c.leaderboard_pending || c.leaderboard_cache_loading;
      case 28: // Open at the five choices, keeping the read-only cache warm.
        c.leaderboard = {}; c.leaderboard_history.clear(); return 1;
      case 29: return c.leaderboard.screen;
      case 30: { // Up/down; catalog navigation continues across page boundaries.
        if (c.leaderboard.screen == 0) {
          c.leaderboard.cursor = std::clamp(c.leaderboard.cursor + std::clamp(value, -1, 1), 0, 4);
        } else if (c.leaderboard.screen == 2) {
          const auto* p = c.leaderboard_view();
          if (!p || !p->valid || p->rows.empty()) return 0;
          const auto index = std::clamp(c.leaderboard.page * kLeaderboardPageSize + c.leaderboard.cursor +
              std::clamp(value, -1, 1), 0, c.leaderboard.total - 1);
          c.leaderboard.page = index / kLeaderboardPageSize;
          c.leaderboard.cursor = index % kLeaderboardPageSize;
          c.leaderboard_visit();
        }
        return c.leaderboard.cursor;
      }
      case 31: return c.leaderboard_enter();
      case 32: return c.leaderboard_back();
      case 33: return c.leaderboard.cursor;
      case 34:
        if (c.leaderboard.screen == 0) return 0;
        c.leaderboard.page = std::clamp(c.leaderboard.page + std::clamp(value, -1, 1), 0, c.leaderboard_page_count() - 1);
        c.leaderboard.cursor = 0; c.leaderboard_visit(); return c.leaderboard.page;
      case 35: c.leaderboard_start_warming(); return 1;
      case 36: return !c.leaderboard_warm_armed || c.leaderboard_warm_done ? 0 : c.leaderboard_warm_active ? 2 : 1;
      default: return 0;
    }
  } catch (const std::exception& e) { lg::warn("replay client: {}", e.what()); return 0; }
}

std::string text(int operation, int index) {
  try {
    auto& c = client(); std::lock_guard lock(c.mutex);
    if (operation == 0) return c.status;
    if (operation == 3) return "Unknown / ID " + c.player;
    if (operation == 4) return c.player_identified ? "Welcome back " + c.player_name :
                              c.ping_pending ? "Detecting player..." :
                              "Undetected player - Press L3 + D-pad Down to ping server";
    if (operation == 5) return c.ping_status;
    if (operation >= 10) {
      const auto* p = c.leaderboard_view();
      switch (operation) {
        case 10: return c.base == kSparkedHostServer ? "SPARKEDHOST / UPLOADED GHOSTS" :
                        c.base == kLocalhostServer ? "LOCALHOST / UPLOADED GHOSTS" : "CUSTOM SERVER / UPLOADED GHOSTS";
        case 11:
          if (c.leaderboard.screen == 0) return "Choose a board to explore";
          if (p && p->valid && (c.leaderboard_pending || p->from_disk || p->offline))
            return std::string(c.leaderboard_pending ? "UPDATING" : p->offline ? "OFFLINE" : "SAVED") +
                   " / Cached " + leaderboard_age(p->fetched_at) + " ago";
          return c.leaderboard_pending ? "Updating standings..." : p ? p->status :
                 c.leaderboard_cache_loading ? "Loading saved standings..." : "Connecting to leaderboard...";
        case 12: return c.leaderboard.screen == 1 ? (p ? std::to_string(p->ranked_missions) : "--") :
                       c.leaderboard.screen == 2 ? std::to_string(c.leaderboard.total) : p ? p->wr : "--";
        case 13: return std::to_string(c.leaderboard.total);
        case 14: return "PAGE " + std::to_string(c.leaderboard.page + 1) + " / " + std::to_string(c.leaderboard_page_count());
        case 15: return c.leaderboard.screen == 0 ? "BROWSE THE RANKINGS" :
                       c.leaderboard.screen == 1 ? kLeaderboardTitles[c.leaderboard.group] :
                       c.leaderboard.screen == 2 ? "Individual Missions" : leaderboard_label(c.leaderboard.label, 48);
        case 16: return c.leaderboard.screen == 0 ? "JAK 3  /  COMMUNITY LEADERBOARDS" :
                       c.leaderboard.screen == 1 ? "COMBINED POINTS  /  YOUR BEST RUN PER MISSION" :
                       c.leaderboard.screen == 2 ? "EVERY MISSION  /  INCLUDING UNRANKED COURSES" : "MISSION STANDINGS  /  BEST COMPLETED RUNS";
        case 17: { // Selected catalog row's preview, without another HTTP request.
          if (!p || c.leaderboard.cursor >= static_cast<int>(p->rows.size())) return "Select a mission below";
          const auto& row = p->rows[c.leaderboard.cursor];
          return std::to_string(row.count) + " RACERS  /  WR " + row.time;
        }
        case 30: return index >= 0 && index < 5 ? kLeaderboardTitles[index] : "";
        case 31: return index >= 0 && index < 5 ? kLeaderboardDescriptions[index] : "";
        default: break;
      }
      if (!p || index < 0 || index >= static_cast<int>(p->rows.size())) return "";
      const auto& row = p->rows[index];
      switch (operation) {
        case 20: return std::to_string(row.place);
        case 21: return c.leaderboard.screen == 2 ? leaderboard_label(row.name, 43) : row.name;
        case 22: return c.leaderboard.screen == 1 ? std::to_string(row.points) : row.time;
        case 23: return c.leaderboard.screen == 1 ? std::to_string(row.count) : row.gap;
        case 24: return std::to_string(c.leaderboard.screen == 1 ? row.records : row.points);
        case 32: return row.group == "main" ? "MAIN" : row.group == "orb" ? "ORB" : "SIDE";
        case 33: return row.count == 0 ? "--" : std::to_string(row.count);
        default: return "";
      }
    }
    if (operation == 1 && index >= 0 && index < static_cast<int>(c.catalog.size())) {
      const auto& row = c.catalog.at(index);
      // A display read must not create a null selection for a new mission.
      const auto selected = selected_ids(c.config.at("custom"), c.prepared_category);
      const auto chosen = row.at("id").is_string() &&
          std::find(selected.begin(), selected.end(), row.at("id").get_ref<const std::string&>()) != selected.end();
      std::string name = row.at("display_name").get<std::string>();
      if (name.size() > 28) name.resize(28);
      for (auto& ch : name) if (ch < 32 || ch > 126 || ch == '~') ch = '_';
      return std::string(chosen ? "[X] " : "[ ] ") + name + " " + time_label(row.at("duration_seconds").get<float>()) + (row.at("completed").get<bool>() ? "" : " (DNF)");
    }
    return "No server replays - refresh";
  } catch (...) { return "Ghost settings error"; }
}

void prepare(const std::string& category) {
  try { auto& c = client(); std::lock_guard lock(c.mutex); c.refresh(category); }
  catch (const std::exception& e) { lg::warn("replay client: {}", e.what()); }
}
std::vector<Ghost> snapshot(const std::string& category) {
  try {
    auto& c = client(); std::lock_guard lock(c.mutex);
    if (c.ready && c.prepared_category == category) return c.prepared;
  } catch (...) {}
  return {};
}
void completed(const replay::File& file) {
  try {
    auto& c = client(); std::lock_guard lock(c.mutex);
    c.prepared_category.clear();
    if (file.completed && !file.truncated && c.config.value("submit_completed", true))
      c.upload(replay::serialize(file), file.category);
  } catch (const std::exception& e) { lg::warn("replay client: {}", e.what()); }
}
}  // namespace replay_client

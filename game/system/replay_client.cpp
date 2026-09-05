#include "replay_client.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <random>
#include <regex>
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
constexpr size_t kSnapshotBudget = 64 * 1024 * 1024;

bool category_ok(const std::string& value) {
  return std::regex_match(value, std::regex("[A-Za-z0-9_-]{1,96}"));
}
bool id_ok(const std::string& value) {
  return std::regex_match(value, std::regex("[a-f0-9]{32}"));
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

size_t receive(char* bytes, size_t size, size_t count, void* user) {
  auto& output = *static_cast<std::string*>(user);
  if (size && count > replay::kMaxFileBytes / size) return 0;
  const auto length = size * count;
  if (length > replay::kMaxFileBytes - output.size()) return 0;
  output.append(bytes, length);
  return length;
}

// Remote servers require HTTPS. Never follow redirects with player credentials;
// bounded transfers run off the game thread and retain normal TLS verification.
std::string request(const std::string& base, const std::string& path,
                    const std::string& body = "", const std::string& player = "",
                    const std::string& token = "") {
  auto* curl = curl_easy_init();
  if (!curl) throw std::runtime_error("HTTP initialization failed");
  std::string response;
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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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
  return response;
}

class Client {
 public:
  Client() : root(file_util::get_user_features_dir(GameVersion::Jak3)), config_path(root / "ghost-client.json") {
    if (fs::exists(config_path)) {
      // Never silently replace a damaged identity and create a different player.
      config = json::parse(file_util::read_text_file(config_path));
    } else {
      config = {{"player_id", random_hex(16)}, {"player_token", random_hex(32)},
                {"server", "https://opengoal-ghosts.sparked.network"}, {"mode", 0},
                {"submit_completed", true}, {"custom", json::object()}};
      save_json(config_path, config);
    }
    player = config.at("player_id").get<std::string>();
    token = config.at("player_token").get<std::string>();
    base = config.at("server").get<std::string>();
    if (!id_ok(player) || !std::regex_match(token, std::regex("[a-f0-9]{64}")) ||
        !std::regex_match(base, std::regex("(http://127\\.0\\.0\\.1:[0-9]{1,5}|https://[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?(:[0-9]{1,5})?)")))
      throw std::runtime_error("Invalid ghost-client.json identity/server");
    mode = std::clamp(config.value("mode", 0), 0, 4);
    worker = std::thread([this] { work(); });
  }
  ~Client() {
    { std::lock_guard lock(mutex); stopping = true; jobs.clear(); }
    wake.notify_one();
    if (worker.joinable()) worker.join();
  }

  fs::path local(const std::string& category, const char* filename) const {
    return root / "replays" / category / filename;
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
    wake.notify_one();
  }
  void work() {
    for (;;) {
      std::function<void()> job;
      { std::unique_lock lock(mutex); wake.wait(lock, [this] { return stopping || !jobs.empty(); });
        if (stopping) return;
        job = std::move(jobs.front()); jobs.pop_front(); }
      try { job(); }
      catch (const std::exception& error) { std::lock_guard lock(mutex); status = error.what(); }
    }
  }
  void register_player() {
    request(base, "/players", json({{"player_id", player}, {"token", token}}).dump());
  }
  void upload(std::string contents, std::string category) { // caller owns mutex
    enqueue([this, contents = std::move(contents), category = std::move(category)] {
      register_player();
      request(base, "/replays", contents, player, token);
      std::lock_guard lock(mutex);
      status = "Submitted replay";
      prepared_category.clear(); // next start refreshes rankings
    });
    status = "Submitting replay...";
  }
  void refresh(const std::string& category, bool force = false) { // caller owns mutex
    if (!category_ok(category)) return;
    if (!force && prepared_category == category) return;
    const auto generation = ++revision;
    const auto selected_mode = mode;
    std::vector<std::string> selected;
    if (config["custom"].contains(category)) selected = config["custom"][category].get<std::vector<std::string>>();
    if (selected.size() > kCustomLimit) selected.resize(kCustomLimit);
    const auto offset = page * 100;
    prepared_category = category;
    prepared.clear();
    ready = false;
    status = "Loading ghosts...";
    enqueue([this, category, generation, selected_mode, selected, offset] {
      std::vector<Ghost> result;
      json rows = json::array();
      std::string result_status = "Ready";
      bool more = false;
      try {
        auto best = (selected_mode == 0 || selected_mode == 1) ? load_local(category, false) : nullptr;
        if (selected_mode == 1 || selected_mode == 3) {
          auto file = selected_mode == 3 ? load_local(category, true) : best;
          if (file) result.push_back({file, selected_mode == 3 ? "Last Attempt" : "Personal Best"});
        } else {
          const auto listing = json::parse(request(base, "/replays?game=jak3&category=" + category + "&offset=" + std::to_string(offset)));
          rows = listing.at("replays");
          more = !listing.at("next_offset").is_null();
          json choices = json::array();
          if (selected_mode == 4) {
            // IDs survive renames and page changes; metadata refreshes names.
            for (const auto& id : selected) {
              if (!id_ok(id)) continue;
              auto match = std::find_if(rows.begin(), rows.end(), [&](const json& row) { return row.at("id") == id; });
              choices.push_back(match != rows.end() ? *match : json::parse(request(base, "/replays/" + id + "/metadata")));
            }
          } else {
            auto url = "/selection?category=" + category + "&player_id=" + player + "&mode=" + (selected_mode == 2 ? "wr" : "default");
            if (best) url += "&best_seconds=" + std::to_string(best->duration_seconds);
            choices = json::parse(request(base, url)).at("replays");
          }
          size_t memory = 0;
          for (const auto& row : choices) {
            const auto id = row.at("id").get<std::string>();
            if (!id_ok(id)) throw std::runtime_error("Invalid replay ID from server");
            const auto cache = root / "ghost-cache" / (id + ".ogr.json");
            std::shared_ptr<replay::File> file;
            if (fs::exists(cache)) file = std::make_shared<replay::File>(replay::load(cache));
            else {
              file = std::make_shared<replay::File>(replay::parse(request(base, "/replays/" + id)));
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
          if (selected_mode == 0 && result.empty() && best) result.push_back({best, "Personal Best"});
        }
        if (result.empty()) result_status = "No replay available for this mode";
      } catch (const std::exception& error) {
        result.clear();
        result_status = error.what();
        if (selected_mode == 0) {
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

  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> jobs;
  bool stopping = false, ready = false, has_more = false;
  fs::path root, config_path;
  json config, catalog = json::array();
  std::string player, token, base, prepared_category, status = "Ready";
  std::vector<Ghost> prepared;
  int mode = 0, page = 0, revision = 0;
  std::thread worker;
};

Client& client() { static Client instance; return instance; }
}  // namespace

int command(int operation, int value, const std::string& category) {
  try {
    auto& c = client();
    std::lock_guard lock(c.mutex);
    switch (operation) {
      case 0: return c.mode;
      case 1:
        c.mode = std::clamp(value, 0, 4); c.config["mode"] = c.mode;
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
      default: return 0;
    }
  } catch (const std::exception& e) { lg::warn("replay client: {}", e.what()); return 0; }
}

std::string text(int operation, int index) {
  try {
    auto& c = client(); std::lock_guard lock(c.mutex);
    if (operation == 0) return c.status;
    if (operation == 3) return "Unknown / ID " + c.player;
    if (operation == 1 && index >= 0 && index < static_cast<int>(c.catalog.size())) {
      const auto& row = c.catalog.at(index);
      const auto& selected = c.config["custom"][c.prepared_category];
      const auto chosen = selected.is_array() && std::find(selected.begin(), selected.end(), row.at("id")) != selected.end();
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

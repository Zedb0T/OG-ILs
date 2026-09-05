#include "kmachine_extras.h"

#include <bit>
#include <bitset>
#include <cstring>
#include <regex>
#include <sstream>

#include "kscheme.h"

#include "common/replay/replay_format.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/font/font_utils.h"

#include "game/external/discord_jak3.h"
#include "game/kernel/common/Symbol4.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/overlord/jak3/iso_cd.h"
#include "game/runtime.h"
#include "game/system/replay_client.h"

namespace jak3 {
namespace kmachine_extras {
AutoSplitterBlock g_auto_splitter_block_jak3;

namespace {
constexpr float kReplayGameUnitsPerMeter = 4096.f;
replay::Recorder g_replay_recorder;
std::shared_ptr<const replay::File> g_replay_playback;
std::vector<replay_client::Ghost> g_replay_ghosts;
bool g_replay_logged_extra = false;

void select_replay_sample(s64& index) {
  const auto slot = static_cast<u64>(index) >> 32;
  index = static_cast<s64>(static_cast<u64>(index) & 0xffffffff);
  g_replay_playback = g_replay_ghosts.empty() ? nullptr :
      g_replay_ghosts.at(slot % g_replay_ghosts.size()).file;
}

fs::path replay_directory_for(std::string_view category) {
  return file_util::get_user_features_dir(g_game_version) / "replays" / std::string(category);
}

bool save_recording(const replay::File& recording) {
  const auto directory = replay_directory_for(recording.category);
  replay::atomic_save(directory / "last-attempt.ogr.json", recording);
  replay::atomic_save(
      directory / (recording.completed ? "last-completed.ogr.json" : "last-unfinished.ogr.json"),
      recording);

  if (!recording.completed) {
    return false;
  }

  const auto best_path = directory / "best-completed.ogr.json";
  bool replace_best = true;
  if (fs::exists(best_path)) {
    try {
      const auto best = replay::load(best_path);
      replace_best = !best.completed || best.category != recording.category ||
                     recording.duration_seconds < best.duration_seconds;
    } catch (const std::exception& error) {
      // A valid completed recording repairs a corrupt local PB instead of
      // making playback permanently unavailable.
      lg::warn("replay: replacing invalid local best: {}", error.what());
    }
  }
  if (replace_best) {
    replay::atomic_save(best_path, recording);
  }
  return replace_best;
}

template <std::size_t Size>
bool copy_replay_string(u32 destination_ptr, const std::array<char, Size>& source) {
  if (!destination_ptr) {
    return false;
  }
  auto* destination = Ptr<String>(destination_ptr).c();
  if (destination->len == 0) {
    return false;
  }
  const auto source_end = std::find(source.begin(), source.end(), '\0');
  const auto source_size = static_cast<std::size_t>(source_end - source.begin());
  const auto copy_size = std::min(source_size, static_cast<std::size_t>(destination->len - 1));
  std::memcpy(destination->data(), source.data(), copy_size);
  destination->data()[copy_size] = '\0';
  return true;
}
}  // namespace

u64 pc_replay_recording_start(s64 start_ticks, u32 category_ptr) {
  if (!category_ptr) {
    return bool_to_symbol(false);
  }
  try {
    auto* category_string = Ptr<String>(category_ptr).c();
    const auto category = std::string_view(category_string->data(), category_string->len);
    const auto started = g_replay_recorder.start("jak3", category, start_ticks);
    if (started) {
      g_replay_logged_extra = false;
      lg::info("replay: started local recording for {}", category);
    } else {
      lg::warn("replay: refused invalid category '{}'", category);
    }
    return bool_to_symbol(started);
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: could not start recording: {}", error.what());
    return bool_to_symbol(false);
  }
}

u64 pc_replay_recording_sample(s64 now_ticks,
                               u32 position_ptr,
                               u32 rotation_ptr,
                               u32 velocity_ptr) {
  if (!position_ptr || !rotation_ptr || !velocity_ptr) {
    return bool_to_symbol(false);
  }
  try {
    const auto* position = Ptr<Vector>(position_ptr).c();
    const auto* rotation = Ptr<Vector>(rotation_ptr).c();
    const auto* velocity = Ptr<Vector>(velocity_ptr).c();
    const auto recorded = g_replay_recorder.add_sample(now_ticks, position->data, rotation->data,
                                                       velocity->data, "none", "", 0.f, 0);
    if (recorded && g_replay_recorder.sample_count() == 1) {
      lg::info("replay: captured first local sample");
    }
    return bool_to_symbol(recorded);
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: stopped recording after sample error: {}", error.what());
    return bool_to_symbol(false);
  }
}

u64 pc_replay_recording_sample_metadata(u32 state_ptr,
                                        u32 animation_ptr,
                                        u32 animation_frame_bits,
                                        u32 status) {
  if (!state_ptr || !animation_ptr) {
    return bool_to_symbol(false);
  }
  try {
    auto* state_string = Ptr<String>(state_ptr).c();
    auto* animation_string = Ptr<String>(animation_ptr).c();
    const auto state = std::string_view(state_string->data(), state_string->len);
    const auto animation = std::string_view(animation_string->data(), animation_string->len);
    const auto animation_frame = std::bit_cast<float>(animation_frame_bits);
    return bool_to_symbol(
        g_replay_recorder.update_last_sample_metadata(state, animation, animation_frame, status));
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: stopped recording after metadata error: {}", error.what());
    return bool_to_symbol(false);
  }
}

u64 pc_replay_recording_finish(u32 completed_symbol) {
  if (!g_replay_recorder.active()) {
    return bool_to_symbol(false);
  }
  try {
    const auto recording = g_replay_recorder.finish(completed_symbol != s7.offset);
    const auto new_best = save_recording(recording);
    replay_client::completed(recording);
    lg::info("replay: saved {} {} attempt with {} samples{}", recording.category,
             recording.completed ? "completed" : "unfinished", recording.samples.size(),
             recording.truncated ? " (duration limit reached)" : "");
    if (new_best) {
      lg::info("replay: updated local best for {} ({:.3f}s)", recording.category,
               recording.duration_seconds);
    }
    return bool_to_symbol(true);
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: could not save recording: {}", error.what());
    return bool_to_symbol(false);
  }
}

u64 pc_replay_recording_active() {
  return bool_to_symbol(g_replay_recorder.active());
}

s64 pc_replay_recording_count() {
  return static_cast<s64>(g_replay_recorder.sample_count());
}

u64 pc_replay_recording_sample_extra(u32 art_group_ptr,
                                     u32 position_ptr,
                                     u32 rotation_ptr,
                                     u32 scale_ptr) {
  // Keep native replay entry points within the Windows trampoline's four register arguments.
  if (!art_group_ptr || !position_ptr || !rotation_ptr || !scale_ptr) {
    return bool_to_symbol(false);
  }
  try {
    auto* art_group_string = Ptr<String>(art_group_ptr).c();
    const auto art_group = std::string_view(art_group_string->data(), art_group_string->len);
    const auto* position = Ptr<Vector>(position_ptr).c();
    const auto* rotation = Ptr<Vector>(rotation_ptr).c();
    const auto* scale = Ptr<Vector>(scale_ptr).c();
    const auto recorded = g_replay_recorder.update_last_sample_extra(
        art_group, position->data, rotation->data, scale->data, "", 0.f);
    if (recorded && !g_replay_logged_extra) {
      g_replay_logged_extra = true;
      lg::info("replay: captured companion drawable {}", art_group);
    }
    return bool_to_symbol(recorded);
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: stopped recording after extra metadata error: {}", error.what());
    return bool_to_symbol(false);
  }
}

u64 pc_replay_recording_sample_extra_animation(u32 animation_ptr, u32 animation_frame_bits) {
  if (!animation_ptr) {
    return bool_to_symbol(false);
  }
  try {
    auto* animation = Ptr<String>(animation_ptr).c();
    return bool_to_symbol(g_replay_recorder.update_last_sample_extra_animation(
        std::string_view(animation->data(), animation->len),
        std::bit_cast<float>(animation_frame_bits)));
  } catch (const std::exception& error) {
    g_replay_recorder.cancel();
    lg::error("replay: stopped recording after extra animation error: {}", error.what());
    return bool_to_symbol(false);
  }
}

s64 pc_replay_playback_start(u32 category_ptr) {
  g_replay_playback.reset();
  g_replay_ghosts.clear();
  if (!category_ptr) {
    return 0;
  }

  try {
    auto* category_string = Ptr<String>(category_ptr).c();
    const std::string category(category_string->data());
    replay_client::prepare(category);
    g_replay_ghosts = replay_client::snapshot(category);
    if (g_replay_ghosts.empty()) return 0;
    g_replay_playback = g_replay_ghosts.front().file;
    return static_cast<s64>(g_replay_playback->samples.size());
  } catch (const std::exception& error) {
    g_replay_playback.reset();
    lg::warn("replay: could not load local best for playback: {}", error.what());
    return 0;
  }
}

u64 pc_replay_playback_sample_transform(s64 sample_index, u32 position_ptr, u32 rotation_ptr) {
  select_replay_sample(sample_index);
  if (!g_replay_playback || !position_ptr || !rotation_ptr || sample_index < 0 ||
      sample_index >= static_cast<s64>(g_replay_playback->samples.size())) {
    return bool_to_symbol(false);
  }

  const auto& sample = g_replay_playback->samples.at(static_cast<std::size_t>(sample_index));
  auto* position = Ptr<Vector>(position_ptr).c();
  auto* rotation = Ptr<Vector>(rotation_ptr).c();
  for (std::size_t i = 0; i < 3; ++i) {
    position->data[i] = sample.position_meters[i] * kReplayGameUnitsPerMeter;
  }
  position->data[3] = 1.f;
  for (std::size_t i = 0; i < sample.rotation.size(); ++i) {
    rotation->data[i] = sample.rotation[i];
  }
  return bool_to_symbol(true);
}

u64 pc_replay_playback_sample_metadata(s64 sample_index,
                                       u32 state_ptr,
                                       u32 animation_ptr,
                                       u32 animation_info_ptr) {
  select_replay_sample(sample_index);
  if (!g_replay_playback || !state_ptr || !animation_ptr || !animation_info_ptr ||
      sample_index < 0 || sample_index >= static_cast<s64>(g_replay_playback->samples.size())) {
    return bool_to_symbol(false);
  }

  const auto& sample = g_replay_playback->samples.at(static_cast<std::size_t>(sample_index));
  if (!copy_replay_string(state_ptr, sample.state) ||
      !copy_replay_string(animation_ptr, sample.animation)) {
    return bool_to_symbol(false);
  }
  auto* animation_info = Ptr<Vector>(animation_info_ptr).c();
  animation_info->data[0] = sample.animation_frame;
  animation_info->data[1] = std::bit_cast<float>(sample.status);
  animation_info->data[2] = 0.f;
  animation_info->data[3] = 0.f;
  return bool_to_symbol(true);
}

u64 pc_replay_playback_sample_extra_transform(s64 sample_index,
                                              u32 art_group_ptr,
                                              u32 position_ptr,
                                              u32 rotation_ptr) {
  select_replay_sample(sample_index);
  if (!g_replay_playback || !art_group_ptr || !position_ptr || !rotation_ptr ||
      sample_index < 0 ||
      sample_index >= static_cast<s64>(g_replay_playback->samples.size())) {
    return bool_to_symbol(false);
  }

  const auto& sample = g_replay_playback->samples.at(static_cast<std::size_t>(sample_index));
  if (sample.extra_art_group[0] == '\0') {
    return bool_to_symbol(false);
  }
  if (!copy_replay_string(art_group_ptr, sample.extra_art_group)) {
    return bool_to_symbol(false);
  }
  auto* position = Ptr<Vector>(position_ptr).c();
  auto* rotation = Ptr<Vector>(rotation_ptr).c();
  for (std::size_t i = 0; i < 3; ++i) {
    position->data[i] = sample.extra_position_meters[i] * kReplayGameUnitsPerMeter;
  }
  position->data[3] = 1.f;
  std::copy(sample.extra_rotation.begin(), sample.extra_rotation.end(), rotation->data);
  return bool_to_symbol(true);
}

u64 pc_replay_playback_sample_extra_metadata(s64 sample_index,
                                             u32 animation_ptr,
                                             u32 animation_info_ptr,
                                             u32 scale_ptr) {
  select_replay_sample(sample_index);
  if (!g_replay_playback || !animation_ptr || !animation_info_ptr || !scale_ptr || sample_index < 0 ||
      sample_index >= static_cast<s64>(g_replay_playback->samples.size())) {
    return bool_to_symbol(false);
  }
  const auto& sample = g_replay_playback->samples.at(static_cast<std::size_t>(sample_index));
  if (sample.extra_art_group[0] == '\0' ||
      !copy_replay_string(animation_ptr, sample.extra_animation)) {
    return bool_to_symbol(false);
  }
  auto* animation_info = Ptr<Vector>(animation_info_ptr).c();
  animation_info->data[0] = sample.extra_animation_frame;
  auto* scale = Ptr<Vector>(scale_ptr).c();
  std::copy(sample.extra_scale.begin(), sample.extra_scale.end(), scale->data);
  animation_info->data[1] = 0.f;
  animation_info->data[2] = 0.f;
  animation_info->data[3] = 0.f;
  return bool_to_symbol(true);
}

u64 pc_replay_playback_stop() {
  const auto was_active = !g_replay_ghosts.empty();
  g_replay_playback.reset();
  g_replay_ghosts.clear();
  return bool_to_symbol(was_active);
}

s64 pc_replay_client_command(s64 operation, s64 value, u32 category_ptr) {
  if (operation == 12) return static_cast<s64>(g_replay_ghosts.size());
  if (operation == 13) return g_replay_ghosts.empty() || value < 0 ? 0 :
      static_cast<s64>(g_replay_ghosts.at(value % g_replay_ghosts.size()).file->samples.size());
  return replay_client::command(static_cast<int>(operation), static_cast<int>(value),
      category_ptr ? std::string(Ptr<String>(category_ptr)->data()) : "");
}

u64 pc_replay_client_text(s64 operation, s64 index, u32 destination_ptr) {
  std::string value;
  if (operation == 2) {
    if (!g_replay_ghosts.empty() && index >= 0) value = g_replay_ghosts.at(index % g_replay_ghosts.size()).label;
  } else value = replay_client::text(static_cast<int>(operation), static_cast<int>(index));
  std::array<char, 256> bytes = {};
  std::memcpy(bytes.data(), value.data(), std::min(value.size(), bytes.size() - 1));
  return bool_to_symbol(copy_replay_string(destination_ptr, bytes));
}

void update_discord_rpc(u32 discord_info) {
  if (gDiscordRpcEnabled) {
    DiscordRichPresence rpc;
    char state[128] = {};
    char large_image_key[128] = {};
    char large_image_text[128] = {};
    char small_image_key[128] = {};
    char small_image_text[128] = {};
    auto info = discord_info ? Ptr<DiscordInfo>(discord_info).c() : NULL;
    if (info) {
      // Get the data from GOAL
      int orbs = (int)info->orb_count;
      int gems = (int)info->gem_count;
      // convert encodings
      std::string status = get_font_bank(GameTextVersion::JAK3)
                               ->convert_game_to_utf8(Ptr<String>(info->status).c()->data());

      // get rid of special encodings like <COLOR_WHITE>
      std::regex r("<.*?>");
      while (std::regex_search(status, r)) {
        status = std::regex_replace(status, r, "");
      }

      char* level = Ptr<String>(info->level).c()->data();
      auto cutscene = Ptr<Symbol4<u32>>(info->cutscene)->value();
      float time = info->time_of_day;
      float percent_completed = info->percent_completed;
      std::bitset<64> focus_status = info->focus_status;
      auto vehicle = static_cast<VehicleType>(info->current_vehicle);
      auto gun = static_cast<PickupType>(info->active_gun);
      char* task = Ptr<String>(info->task).c()->data();

      // Construct the DiscordRPC Object
      const char* full_level_name =
          get_full_level_name(level_names, level_name_remap, Ptr<String>(info->level).c()->data());
      memset(&rpc, 0, sizeof(rpc));
      // if we have an active task, set the mission specific image for it
      if (strcmp(task, "unknown") != 0) {
        strcpy(large_image_key, task);
      } else {
        auto base_level_name = get_base_level_name(level_name_remap, level);
        // if we are in an outdoors level, use the picture for the corresponding time of day
        if (!indoors(indoor_levels, base_level_name.c_str())) {
          char level_with_tod[128] = {};
          strcpy(level_with_tod, base_level_name.c_str());
          strcat(level_with_tod, "-");
          strcat(level_with_tod, time_of_day_str(time));
          strcpy(large_image_key, level_with_tod);
        } else {
          strcpy(large_image_key, base_level_name.c_str());
        }
      }
      strcpy(large_image_text, full_level_name);
      if (!strcmp(full_level_name, "unknown")) {
        strcpy(large_image_key, full_level_name);
        strcpy(large_image_text, level);
      }
      rpc.largeImageKey = large_image_key;
      if (cutscene != offset_of_s7()) {
        strcpy(state, "Watching a cutscene");
        // temporarily move these counters to the large image tooltip during a cutscene
        strcat(large_image_text,
               fmt::format(" | {:.0f}% | Orbs: {} | Gems: {} | {}", percent_completed,
                           std::to_string(orbs), std::to_string(gems), get_time_of_day(time))
                   .c_str());
      } else {
        strcpy(state, fmt::format("{:.0f}% | Orbs: {} | Gems: {} | {}", percent_completed,
                                  std::to_string(orbs), std::to_string(gems), get_time_of_day(time))
                          .c_str());
      }
      rpc.largeImageText = large_image_text;
      rpc.state = state;
      // check for any special conditions to display for the small image
      if (FOCUS_TEST(focus_status, FocusStatus::Board)) {
        strcpy(small_image_key, "focus-status-board");
        strcpy(small_image_text, "On the JET-Board");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Mech)) {
        strcpy(small_image_key, "focus-status-mech");
        strcpy(small_image_text, "Controlling a Dark Maker bot");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Pilot)) {
        strcpy(small_image_key, "focus-status-pilot");
        auto vehicle_name = VehicleTypeToString(vehicle);
        if (!strcmp(task, "comb-travel") || !strcmp(task, "comb-wild-ride")) {
          strcpy(small_image_key, vehicle_type_to_string.at(VehicleType::h_sled).c_str());
          strcpy(small_image_text, "Driving the Catacombs Rail Rider");
        } else if (!strcmp(task, "desert-glide")) {
          strcpy(small_image_key, vehicle_type_to_string.at(VehicleType::h_glider).c_str());
          strcpy(small_image_text, "Flying the Monk Glider");
        } else if (!strcmp(task, "factory-sky-battle")) {
          strcpy(small_image_key, vehicle_type_to_string.at(VehicleType::h_hellcat).c_str());
          strcpy(small_image_text, "Flying the Hellcat");
        } else {
          if (vehicle_name != "Unknown") {
            strcpy(small_image_key, vehicle_type_to_string.at(vehicle).c_str());
            strcpy(small_image_text, fmt::format("Driving the {}", vehicle_name).c_str());
          } else {
            strcpy(small_image_text, "Driving a Zoomer");
          }
        }
      } else if (FOCUS_TEST(focus_status, FocusStatus::Indax)) {
        strcpy(small_image_key, "focus-status-indax");
        strcpy(small_image_text, "Playing as Daxter");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Dark)) {
        strcpy(small_image_key, "focus-status-dark");
        strcpy(small_image_text, "Dark Jak");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Light)) {
        strcpy(small_image_key, "focus-status-light");
        strcpy(small_image_text, "Light Jak");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Turret)) {
        strcpy(small_image_key, "focus-status-turret");
        strcpy(small_image_text, "In a Turret");
      } else if (FOCUS_TEST(focus_status, FocusStatus::Gun)) {
        auto gun_name = PickupTypeToString(gun);
        if (gun_name != "Unknown") {
          strcpy(small_image_key, pickup_type_to_string.at(gun).c_str());
          strcpy(small_image_text, fmt::format("Using the {}", gun_name).c_str());
        } else {
          strcpy(small_image_key, "gun-none");
          strcpy(small_image_text, "Using a Gun");
        }
      } else {
        strcpy(small_image_key, "");
        strcpy(small_image_text, "");
      }
      rpc.smallImageKey = small_image_key;
      rpc.smallImageText = small_image_text;
      rpc.startTimestamp = gStartTime;
      rpc.details = status.c_str();
      rpc.partySize = 0;
      rpc.partyMax = 0;
      Discord_UpdatePresence(&rpc);
    }
  } else {
    Discord_ClearPresence();
  }
}

void pc_set_levels(u32 lev_list) {
  if (!Gfx::GetCurrentRenderer()) {
    return;
  }
  std::vector<std::string> levels;
  for (int i = 0; i < LEVEL_MAX; i++) {
    u32 lev = *Ptr<u32>(lev_list + i * 4);
    std::string ls = Ptr<String>(lev).c()->data();
    if (ls != "none" && ls != "#f" && ls != "") {
      levels.push_back(ls);
    }
  }

  Gfx::GetCurrentRenderer()->set_levels(levels);
}

void pc_set_active_levels(u32 lev_list) {
  if (!Gfx::GetCurrentRenderer()) {
    return;
  }
  std::vector<std::string> levels;
  for (int i = 0; i < LEVEL_MAX; i++) {
    u32 lev = *Ptr<u32>(lev_list + i * 4);
    std::string ls = Ptr<String>(lev).c()->data();
    if (ls != "none" && ls != "#f" && ls != "") {
      levels.push_back(ls);
    }
  }

  Gfx::GetCurrentRenderer()->set_active_levels(levels);
}

static std::string unpack_vag_name_jak3(u64 compressed) {
  const char* char_map = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";
  u32 chars = compressed & 0x1fffff;
  std::array<char, 9> buf{};
  buf.fill(0);
  for (int i = 0; i < 8; i++) {
    if (i == 4) {
      chars = (compressed >> 21) & 0x1fffff;
    }
    buf[7 - i] = char_map[chars % 38];
    chars /= 38;
  }

  return {buf.data()};
}

u32 alloc_vagdir_names(u32 heap_sym) {
  auto alloced_heap = (Ptr<u64>)alloc_heap_memory(heap_sym, g_VagDir.num_entries * 8 + 8);
  if (alloced_heap.offset) {
    *alloced_heap = g_VagDir.num_entries;
    // use entry -1 to get the amount
    alloced_heap = alloced_heap + 8;
    for (size_t i = 0; i < g_VagDir.num_entries; ++i) {
      char vagname_temp[9];
      u64 packed = *(u64*)g_VagDir.entries[i].words;
      auto name = unpack_vag_name_jak3(packed);
      memcpy(vagname_temp, name.data(), 8);
      for (int j = 0; j < 8; ++j) {
        vagname_temp[j] = tolower(vagname_temp[j]);
      }
      vagname_temp[8] = 0;
      u64 vagname_val;
      memcpy(&vagname_val, vagname_temp, 8);
      *(alloced_heap + i * 8) = vagname_val;
    }
    return alloced_heap.offset;
  }
  return s7.offset;
}

inline u64 bool_to_symbol(const bool val) {
  return val ? static_cast<u64>(s7.offset) + true_symbol_offset(g_game_version) : s7.offset;
}

inline bool symbol_to_bool(const u32 symptr) {
  return symptr != s7.offset;
}

void init_autosplit_struct() {
  g_auto_splitter_block_jak3.pointer_to_symbol =
      (u64)g_ee_main_mem + (u64)intern_from_c(-1, 0, "*autosplit-info-jak3*")->value();
}

// TODO - currently using a single mutex for all background task synchronization
std::mutex background_task_lock;

std::string last_rpc_error;

// TODO - add a TTL to this
std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>
    external_speedrun_time_cache = {};
std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>
    external_race_time_cache = {};
std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>
    external_highscores_cache = {};
std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>
    any_mission_times_cache = {};
std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>
    warp_mission_times_cache = {};

// clang-format off
// TODO - eventually don't depend on SRC
const std::unordered_map<std::string, std::string> external_speedrun_lookup_urls = {
    {"any", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/9d8p1qkn?embed=players&max=200"},
    {"nooob", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/5dwj0n0k?embed=players&max=200"},
    {"allmissions", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/xd1r98k8?embed=players&max=200"},
    {"100", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/zd30nndn?embed=players&max=200"},
    {"anyorbs", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/jdzw79vd?embed=players&max=200"},
    {"anyhero", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/category/9kvp50kg?embed=players&max=200"}};
const std::unordered_map<std::string, std::string> external_race_lookup_urls = {
    {"time-trial", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/level/kwjvyzwg/jdr8onk6?embed=players&max=200"},
    {"rally", "https://www.speedrun.com/api/v1/leaderboards/nj1nww1p/level/owo3kyw6/jdr8onk6?embed=players&max=200"}};
const std::unordered_map<std::string, std::string> external_highscores_lookup_urls = {
    {"was-pre-game", "https://api.jakspeedruns.workers.dev/v1/highscores/9"},
    {"air-time", "https://api.jakspeedruns.workers.dev/v1/highscores/10"},
    {"total-air-time", "https://api.jakspeedruns.workers.dev/v1/highscores/11"},
    {"jump-distance", "https://api.jakspeedruns.workers.dev/v1/highscores/12"},
    {"total-jump-distance", "https://api.jakspeedruns.workers.dev/v1/highscores/13"},
    {"roll-count", "https://api.jakspeedruns.workers.dev/v1/highscores/14"},
    {"wascity-gungame", "https://api.jakspeedruns.workers.dev/v1/highscores/15"},
    {"jetboard", "https://api.jakspeedruns.workers.dev/v1/highscores/16"},
    {"gungame-yellow-2", "https://api.jakspeedruns.workers.dev/v1/highscores/17"},
    {"gungame-red-2", "https://api.jakspeedruns.workers.dev/v1/highscores/18"},
    {"gungame-ratchet", "https://api.jakspeedruns.workers.dev/v1/highscores/19"},
    {"gungame-clank", "https://api.jakspeedruns.workers.dev/v1/highscores/20"},
    {"power-game", "https://api.jakspeedruns.workers.dev/v1/highscores/21"},
    {"destroy-interceptors", "https://api.jakspeedruns.workers.dev/v1/highscores/22"}};
const std::unordered_map<std::string, std::string> mission_level_ids = {
    {"arena-training-1", "9m5y375d"},
    {"arena-fight-1", "wkk8o52w"},
    {"wascity-chase", "9203l2od"},
    {"wascity-pre-game", "9vmjx439"},
    {"desert-turtle-training", "d40nz429"},
    {"desert-course-race", "d0klymx9"},
    {"desert-artifact-race-1", "w6qrx8rd"},
    {"wascity-leaper-race", "93q8evnw"},
    {"desert-hover", "98reqygd"},
    {"arena-fight-2", "dnor86qw"},
    {"desert-catch-lizards", "dy1j6nkd"},
    {"desert-rescue", "93q8evzw"},
    {"wascity-gungame", "98reqypd"},
    {"arena-fight-3", "dnor867w"},
    {"nest-eggs", "dy1j6nrd"},
    {"temple-climb", "drpglmlw"},
    {"desert-glide", "wlgn1qe9"},
    {"volcano-darkeco", "we2ngr7w"},
    {"temple-oracle", "9zpxjygw"},
    {"desert-oasis-defense", "9gyr5ep9"},
    {"temple-tests", "9x1kq8pd"},
    {"comb-travel", "95kyg339"},
    {"mine-explore", "dqzgrp2d"},
    {"mine-blow", "d7yl5k6d"},
    {"mine-boss", "wj73jl7w"},
    {"sewer-met-hum", "wo7ln1k9"},
    {"city-port-fight", "d1j6re5d"},
    {"city-port-attack", "wp7eymjw"},
    {"city-gun-course-1", "9m5y3qxd"},
    {"city-sniper-fight", "wkk8ozpw"},
    {"sewer-kg-met", "9203lg3d"},
    {"city-destroy-darkeco", "9vmjxel9"},
    {"forest-kill-plants", "d40nz7r9"},
    {"city-destroy-grid", "d0klyr49"},
    {"city-hijack-vehicle", "w6qrxpgd"},
    {"city-port-assault", "93q8e5zw"},
    {"city-gun-course-2", "98reqvpd"},
    {"city-blow-barricade", "dnor8j7w"},
    {"city-protect-hq", "dy1j67rd"},
    {"sewer-hum-kg", "drpgl4lw"},
    {"city-power-game", "wlgn1xe9"},
    {"desert-artifact-race-2", "we2ngk7w"},
    {"nest-hunt", "9zpxjogw"},
    {"desert-beast-battle", "9gyr50p9"},
    {"desert-jump-mission", "9x1kq5pd"},
    {"desert-chase-marauders", "95kygo39"},
    {"forest-ring-chase", "dqzgr22d"},
    {"factory-sky-battle", "d7ylr66d"},
    {"factory-assault", "wj73ro7w"},
    {"factory-boss", "wo7lxgk9"},
    {"temple-defend", "d1j61v5d"},
    {"wascity-defend", "wp7e5kjw"},
    {"forest-turn-on-machine", "9m5yzkxd"},
    {"precursor-tour", "wkk8lgpw"},
    {"city-blow-tower", "9203pv3d"},
    {"tower-destroy", "9vmjp1l9"},
    {"palace-ruins-patrol", "d40n8vr9"},
    {"palace-ruins-patrol-w-bbush", "dy1j04rd"},
    {"palace-ruins-attack", "d0klov49"},
    {"palace-ruins-attack-w-bbush", "drpg7klw"},
    {"comb-wild-ride", "w6qrykgd"},
    {"precursor-destroy-ship", "93q832zw"},
    {"terraformer", "98rel2pd"},
    {"desert-final-boss", "dnorgk7w"},
    {"wascity-bbush-get-to-18", "9203p03d"},
    {"desert-bbush-get-to-19", "9vmjpml9"},
    {"wascity-bbush-get-to-20", "d40n80r9"},
    {"wascity-bbush-get-to-21", "d0klok49"},
    {"wascity-bbush-get-to-22", "w6qryqgd"},
    {"wascity-bbush-get-to-23", "93q83qzw"},
    {"wascity-bbush-get-to-24", "98relrpd"},
    {"wascity-bbush-get-to-25", "dnorgo7w"},
    {"wascity-bbush-ring-3", "93q830zw"},
    {"wascity-bbush-ring-4", "98reljpd"},
    {"wascity-bbush-spirit-chase-2", "dnorg5vw"},
    {"wascity-bbush-timer-chase-2", "wlgne6r9"},
    {"wascity-gungame-2", "wkk8lrqw"},
    {"desert-bbush-get-to-1", "wlgneke9"},
    {"desert-bbush-get-to-2", "we2npz7w"},
    {"desert-bbush-get-to-3", "9zpx0mgw"},
    {"desert-bbush-get-to-4", "9gyr8kp9"},
    {"desert-bbush-get-to-5", "9x1k0mpd"},
    {"desert-bbush-get-to-6", "95kynv39"},
    {"desert-bbush-get-to-7", "dqzgq62d"},
    {"desert-bbush-get-to-8", "d7ylry6d"},
    {"desert-bbush-get-to-9", "wj73r77w"},
    {"desert-bbush-get-to-11", "wo7lx7k9"},
    {"desert-bbush-get-to-12", "d1j61j5d"},
    {"desert-bbush-get-to-14", "wp7e57jw"},
    {"desert-bbush-get-to-16", "9m5yz5xd"},
    {"desert-bbush-get-to-17", "wkk8lkpw"},
    {"desert-bbush-ring-1", "d0klo049"},
    {"desert-bbush-ring-2", "w6qryvgd"},
    {"desert-bbush-egg-spider-1", "93q830ew"},
    {"desert-bbush-spirit-chase-1", "98reljld"},
    {"desert-bbush-timer-chase-1", "drpg7jzw"},
    {"desert-bbush-time-trial-1", "dqzgq11d"},
    {"desert-bbush-rally", "d7ylr3vd"},
    {"desert-rescue-bbush", "wo7lx2y9"},
    {"desert-bbush-air-time", "we2np4rw"},
    {"desert-bbush-total-air-time", "9zpx0row"},
    {"desert-bbush-jump-distance", "9gyr83k9"},
    {"desert-bbush-total-jump-distance", "9x1k0l1d"},
    {"desert-bbush-roll-count", "95kyn8j9"},
    {"desert-bbush-destroy-interceptors", "wp7e5qzw"},
    {"wascity-pre-game-2", "9m5yzp1d"},
    {"city-bbush-get-to-26", "dy1j01rd"},
    {"city-bbush-get-to-27", "drpg7plw"},
    {"city-bbush-get-to-28", "wlgnege9"},
    {"city-bbush-get-to-29", "we2np27w"},
    {"city-bbush-get-to-30", "9zpx0pgw"},
    {"city-bbush-get-to-31", "9gyr8yp9"},
    {"city-bbush-get-to-32", "9x1k01pd"},
    {"city-bbush-get-to-33", "95kynk39"},
    {"city-bbush-get-to-34", "dqzgqz2d"},
    {"city-bbush-get-to-35", "d7ylr76d"},
    {"city-bbush-get-to-36", "wj73ry7w"},
    {"city-bbush-get-to-37", "wo7lxkk9"},
    {"city-bbush-get-to-38", "d1j6105d"},
    {"city-bbush-get-to-39", "wp7e5pjw"},
    {"city-bbush-get-to-40", "9m5yzexd"},
    {"city-bbush-get-to-41", "wkk8l2pw"},
    {"city-bbush-get-to-42", "9203pj3d"},
    {"city-bbush-get-to-43", "9vmjpyl9"},
    {"city-bbush-get-to-44", "d40n86r9"},
    {"city-bbush-ring-5", "dnorg57w"},
    {"city-bbush-ring-6", "dy1j0erd"},
    {"city-bbush-spirit-chase-3", "dy1j0ezd"},
    {"city-bbush-port-attack", "wj73rezw"},
    {"city-jetboard-bbush", "d1j6156d"},
    {"city-power-game-2", "9203pord"}
};
// clang-format on

void callback_fetch_external_speedrun_times(bool success,
                                            const std::string& cache_id,
                                            std::optional<std::string> result) {
  std::scoped_lock lock{background_task_lock};

  if (!success) {
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(true);
    if (result) {
      last_rpc_error = result.value();
    } else {
      last_rpc_error = "Unexpected Error Occurred";
    }
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // TODO - might be nice to have an error if we get an unexpected payload
  if (!result) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // Parse the response
  const auto data = safe_parse_json(result.value());
  if (!data || !data->contains("data") || !data->at("data").contains("players") ||
      !data->at("data").at("players").contains("data") || !data->at("data").contains("runs")) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  auto& players = data->at("data").at("players").at("data");
  auto& runs = data->at("data").at("runs");
  std::vector<std::pair<std::string, float>> times = {};
  for (const auto& run_info : runs) {
    std::pair<std::string, float> time_info;
    if (players.size() > times.size() && players.at(times.size()).contains("names") &&
        players.at(times.size()).at("names").contains("international")) {
      time_info.first = players.at(times.size()).at("names").at("international");
    } else if (players.size() > times.size() && players.at(times.size()).contains("name")) {
      time_info.first = players.at(times.size()).at("name");
    } else {
      time_info.first = "Unknown";
    }
    if (run_info.contains("run") && run_info.at("run").contains("times") &&
        run_info.at("run").at("times").contains("primary_t")) {
      time_info.second = run_info.at("run").at("times").at("primary_t");
      times.push_back(time_info);
    }
  }
  external_speedrun_time_cache[cache_id] = times;
  intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
}

// TODO - duplicate code, put it in a function
void callback_fetch_external_race_times(bool success,
                                        const std::string& cache_id,
                                        std::optional<std::string> result) {
  std::scoped_lock lock{background_task_lock};

  if (!success) {
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(true);
    if (result) {
      last_rpc_error = result.value();
    } else {
      last_rpc_error = "Unexpected Error Occurred";
    }
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // TODO - might be nice to have an error if we get an unexpected payload
  if (!result) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // Parse the response
  const auto data = safe_parse_json(result.value());
  if (!data || !data->contains("data") || !data->at("data").contains("players") ||
      !data->at("data").at("players").contains("data") || !data->at("data").contains("runs")) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  auto& players = data->at("data").at("players").at("data");
  auto& runs = data->at("data").at("runs");
  std::vector<std::pair<std::string, float>> times = {};
  for (const auto& run_info : runs) {
    std::pair<std::string, float> time_info;
    if (players.size() > times.size() && players.at(times.size()).contains("names") &&
        players.at(times.size()).at("names").contains("international")) {
      time_info.first = players.at(times.size()).at("names").at("international");
    } else if (players.size() > times.size() && players.at(times.size()).contains("name")) {
      time_info.first = players.at(times.size()).at("name");
    } else {
      time_info.first = "Unknown";
    }
    if (run_info.contains("run") && run_info.at("run").contains("times") &&
        run_info.at("run").at("times").contains("primary_t")) {
      time_info.second = run_info.at("run").at("times").at("primary_t");
      times.push_back(time_info);
    }
  }
  external_race_time_cache[cache_id] = times;
  intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
}

// TODO - duplicate code, put it in a function
void callback_fetch_external_highscores(bool success,
                                        const std::string& cache_id,
                                        std::optional<std::string> result) {
  std::scoped_lock lock{background_task_lock};

  if (!success) {
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(true);
    if (result) {
      last_rpc_error = result.value();
    } else {
      last_rpc_error = "Unexpected Error Occurred";
    }
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // TODO - might be nice to have an error if we get an unexpected payload
  if (!result) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // Parse the response
  const auto data = safe_parse_json(result.value());
  std::vector<std::pair<std::string, float>> times = {};
  for (const auto& highscore_info : data.value()) {
    if (highscore_info.contains("playerName") && highscore_info.contains("score")) {
      std::pair<std::string, float> time_info;
      time_info.first = highscore_info.at("playerName");
      time_info.second = highscore_info.at("score");
      times.push_back(time_info);
    }
  }
  external_highscores_cache[cache_id] = times;
  intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
}

void callback_fetch_external_any_mission_times(bool success,
                                               const std::string& cache_id,
                                               std::optional<std::string> result) {
  std::scoped_lock lock{background_task_lock};

  if (!success) {
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(true);
    if (result) {
      last_rpc_error = result.value();
    } else {
      last_rpc_error = "Unexpected Error Occurred";
    }
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // TODO - might be nice to have an error if we get an unexpected payload
  if (!result) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // Parse the response
  const auto data = safe_parse_json(result.value());
  if (!data || !data->contains("data") || !data->at("data").contains("players") ||
      !data->at("data").at("players").contains("data") || !data->at("data").contains("runs")) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  auto& players = data->at("data").at("players").at("data");
  auto& runs = data->at("data").at("runs");
  std::vector<std::pair<std::string, float>> times = {};
  for (const auto& run_info : runs) {
    std::pair<std::string, float> time_info;
    if (players.size() > times.size() && players.at(times.size()).contains("names") &&
        players.at(times.size()).at("names").contains("international")) {
      time_info.first = players.at(times.size()).at("names").at("international");
    } else if (players.size() > times.size() && players.at(times.size()).contains("name")) {
      time_info.first = players.at(times.size()).at("name");
    } else {
      time_info.first = "Unknown";
    }
    if (run_info.contains("run") && run_info.at("run").contains("times") &&
        run_info.at("run").at("times").contains("primary_t")) {
      time_info.second = run_info.at("run").at("times").at("primary_t");
      times.push_back(time_info);
    }
  }
  any_mission_times_cache[cache_id] = times;
  intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
}

void callback_fetch_external_warp_mission_times(bool success,
                                                const std::string& cache_id,
                                                std::optional<std::string> result) {
  std::scoped_lock lock{background_task_lock};

  if (!success) {
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(true);
    if (result) {
      last_rpc_error = result.value();
    } else {
      last_rpc_error = "Unexpected Error Occurred";
    }
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // TODO - might be nice to have an error if we get an unexpected payload
  if (!result) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  // Parse the response
  const auto data = safe_parse_json(result.value());
  if (!data || !data->contains("data") || !data->at("data").contains("players") ||
      !data->at("data").at("players").contains("data") || !data->at("data").contains("runs")) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
    return;
  }

  auto& players = data->at("data").at("players").at("data");
  auto& runs = data->at("data").at("runs");
  std::vector<std::pair<std::string, float>> times = {};
  for (const auto& run_info : runs) {
    std::pair<std::string, float> time_info;
    if (players.size() > times.size() && players.at(times.size()).contains("names") &&
        players.at(times.size()).at("names").contains("international")) {
      time_info.first = players.at(times.size()).at("names").at("international");
    } else if (players.size() > times.size() && players.at(times.size()).contains("name")) {
      time_info.first = players.at(times.size()).at("name");
    } else {
      time_info.first = "Unknown";
    }
    if (run_info.contains("run") && run_info.at("run").contains("times") &&
        run_info.at("run").at("times").contains("primary_t")) {
      time_info.second = run_info.at("run").at("times").at("primary_t");
      times.push_back(time_info);
    }
  }
  warp_mission_times_cache[cache_id] = times;
  intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(false);
}

void pc_fetch_external_speedrun_times(u32 speedrun_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto speedrun_id = std::string(Ptr<String>(speedrun_id_ptr).c()->data());
  if (external_speedrun_lookup_urls.find(speedrun_id) == external_speedrun_lookup_urls.end()) {
    lg::error("No URL for speedrun_id: '{}'", speedrun_id);
    return;
  }

  // First check to see if we've already retrieved this info
  if (external_speedrun_time_cache.find(speedrun_id) == external_speedrun_time_cache.end()) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(true);
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(false);
    // otherwise, hit the URL
    WebRequestJobPayload req;
    req.callback = callback_fetch_external_speedrun_times;
    req.url = external_speedrun_lookup_urls.at(speedrun_id);
    req.cache_id = speedrun_id;
    g_background_worker.enqueue_webrequest(req);
  }
}

void pc_fetch_external_race_times(u32 race_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto race_id = std::string(Ptr<String>(race_id_ptr).c()->data());
  if (external_race_lookup_urls.find(race_id) == external_race_lookup_urls.end()) {
    lg::error("No URL for race_id: '{}'", race_id);
    return;
  }

  // First check to see if we've already retrieved this info
  if (external_race_time_cache.find(race_id) == external_race_time_cache.end()) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(true);
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(false);
    // otherwise, hit the URL
    WebRequestJobPayload req;
    req.callback = callback_fetch_external_race_times;
    req.url = external_race_lookup_urls.at(race_id);
    req.cache_id = race_id;
    g_background_worker.enqueue_webrequest(req);
  }
}

void pc_fetch_external_mission_times(u32 mission_id_ptr, u32 p_warp) {
  std::scoped_lock lock{background_task_lock};
  auto mission_id = std::string(Ptr<String>(mission_id_ptr).c()->data());
  if (mission_level_ids.find(mission_id) == mission_level_ids.end()) {
    lg::error("No URL for mission_id: '{}'", mission_id);
    return;
  }

  // First check to see if we've already retrieved this info
  if (p_warp == 0) {
    // any%
    if (any_mission_times_cache.find(mission_id) == any_mission_times_cache.end()) {
      intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(true);
      intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(false);
      // otherwise, hit the URL
      WebRequestJobPayload req;
      req.callback = callback_fetch_external_any_mission_times;
      req.url = fmt::format(
          "https://www.speedrun.com/api/v1/leaderboards/j1l7q0zd/level/{}/"
          "rkl7n8qd?embed=players&max=200",
          mission_level_ids.at(mission_id));
      req.cache_id = mission_id;
      g_background_worker.enqueue_webrequest(req);
    }
  } else {
    // major % warp
    if (warp_mission_times_cache.find(mission_id) == warp_mission_times_cache.end()) {
      intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(true);
      intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(false);
      // otherwise, hit the URL
      WebRequestJobPayload req;
      req.callback = callback_fetch_external_warp_mission_times;
      req.url = fmt::format(
          "https://www.speedrun.com/api/v1/leaderboards/j1l7q0zd/level/{}/"
          "7dgw7742?embed=players&max=200",
          mission_level_ids.at(mission_id));
      req.cache_id = mission_id;
      g_background_worker.enqueue_webrequest(req);
    }
  }
}

void pc_fetch_external_highscores(u32 highscore_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto highscore_id = std::string(Ptr<String>(highscore_id_ptr).c()->data());
  if (external_highscores_lookup_urls.find(highscore_id) == external_highscores_lookup_urls.end()) {
    lg::error("No URL for highscore_id: '{}'", highscore_id);
    return;
  }

  // First check to see if we've already retrieved this info
  if (external_highscores_cache.find(highscore_id) == external_highscores_cache.end()) {
    intern_from_c(-1, 0, "*pc-waiting-on-rpc?*")->value() = bool_to_symbol(true);
    intern_from_c(-1, 0, "*pc-rpc-error?*")->value() = bool_to_symbol(false);
    // otherwise, hit the URL
    WebRequestJobPayload req;
    req.callback = callback_fetch_external_highscores;
    req.url = external_highscores_lookup_urls.at(highscore_id);
    req.cache_id = highscore_id;
    g_background_worker.enqueue_webrequest(req);
  }
}

void pc_get_external_speedrun_time(u32 speedrun_id_ptr,
                                   s32 index,
                                   u32 name_dest_ptr,
                                   u32 time_dest_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto speedrun_id = std::string(Ptr<String>(speedrun_id_ptr).c()->data());
  if (external_speedrun_time_cache.find(speedrun_id) != external_speedrun_time_cache.end()) {
    const auto& runs = external_speedrun_time_cache.at(speedrun_id);
    if (index < (int)runs.size()) {
      const auto& run_info = external_speedrun_time_cache.at(speedrun_id).at(index);
      std::string converted =
          get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game(run_info.first);
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = run_info.second;
    } else {
      std::string converted = get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game("");
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = -1.0;
    }
  }
}

void pc_get_external_race_time(u32 race_id_ptr, s32 index, u32 name_dest_ptr, u32 time_dest_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto race_id = std::string(Ptr<String>(race_id_ptr).c()->data());
  if (external_race_time_cache.find(race_id) != external_race_time_cache.end()) {
    const auto& runs = external_race_time_cache.at(race_id);
    if (index < (int)runs.size()) {
      const auto& run_info = external_race_time_cache.at(race_id).at(index);
      std::string converted =
          get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game(run_info.first);
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = run_info.second;
    } else {
      std::string converted = get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game("");
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = -1.0;
    }
  }
}

void pc_get_external_any_mission_time(u32 mission_id_ptr,
                                      s32 index,
                                      u32 name_dest_ptr,
                                      u32 time_dest_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto mission_id = std::string(Ptr<String>(mission_id_ptr).c()->data());
  // any%
  if (any_mission_times_cache.find(mission_id) != any_mission_times_cache.end()) {
    const auto& runs = any_mission_times_cache.at(mission_id);
    if (index < (int)runs.size()) {
      const auto& run_info = any_mission_times_cache.at(mission_id).at(index);
      // std::string converted =
      //     get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game(run_info.first);
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), run_info.first.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = run_info.second;
    } else {
      std::string converted = get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game("");
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = -1.0;
    }
  }
}

void pc_get_external_warp_mission_time(u32 mission_id_ptr,
                                       s32 index,
                                       u32 name_dest_ptr,
                                       u32 time_dest_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto mission_id = std::string(Ptr<String>(mission_id_ptr).c()->data());
  // major % warp
  if (warp_mission_times_cache.find(mission_id) != warp_mission_times_cache.end()) {
    const auto& runs = warp_mission_times_cache.at(mission_id);
    if (index < (int)runs.size()) {
      const auto& run_info = warp_mission_times_cache.at(mission_id).at(index);
      // std::string converted =
      //     get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game(run_info.first);
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), run_info.first.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = run_info.second;
    } else {
      std::string converted = get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game("");
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = -1.0;
    }
  }
}

void pc_get_external_highscore(u32 highscore_id_ptr,
                               s32 index,
                               u32 name_dest_ptr,
                               u32 time_dest_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto highscore_id = std::string(Ptr<String>(highscore_id_ptr).c()->data());
  if (external_highscores_cache.find(highscore_id) != external_highscores_cache.end()) {
    const auto& runs = external_highscores_cache.at(highscore_id);
    if (index < (int)runs.size()) {
      const auto& run_info = external_highscores_cache.at(highscore_id).at(index);
      std::string converted =
          get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game(run_info.first);
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = run_info.second;
    } else {
      std::string converted = get_font_bank(GameTextVersion::JAK3)->convert_utf8_to_game("");
      strcpy(Ptr<String>(name_dest_ptr).c()->data(), converted.c_str());
      *(Ptr<float>(time_dest_ptr).c()) = -1.0;
    }
  }
}

s32 pc_get_num_external_speedrun_times(u32 speedrun_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto speedrun_id = std::string(Ptr<String>(speedrun_id_ptr).c()->data());
  if (external_speedrun_time_cache.find(speedrun_id) != external_speedrun_time_cache.end()) {
    return external_speedrun_time_cache.at(speedrun_id).size();
  }
  return 0;
}

s32 pc_get_num_external_mission_times(u32 mission_id_ptr, u32 p_warp) {
  std::scoped_lock lock{background_task_lock};
  auto mission_id = std::string(Ptr<String>(mission_id_ptr).c()->data());
  if (p_warp == 0) {
    // any%
    if (any_mission_times_cache.find(mission_id) != any_mission_times_cache.end()) {
      return any_mission_times_cache.at(mission_id).size();
    }
  } else {
    // major % warp
    if (warp_mission_times_cache.find(mission_id) != warp_mission_times_cache.end()) {
      return warp_mission_times_cache.at(mission_id).size();
    }
  }
  return 0;
}

s32 pc_get_num_external_race_times(u32 race_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto race_id = std::string(Ptr<String>(race_id_ptr).c()->data());
  if (external_race_time_cache.find(race_id) != external_race_time_cache.end()) {
    return external_race_time_cache.at(race_id).size();
  }
  return 0;
}

s32 pc_get_num_external_highscores(u32 highscore_id_ptr) {
  std::scoped_lock lock{background_task_lock};
  auto highscore_id = std::string(Ptr<String>(highscore_id_ptr).c()->data());
  if (external_highscores_cache.find(highscore_id) != external_highscores_cache.end()) {
    return external_highscores_cache.at(highscore_id).size();
  }
  return 0;
}

void to_json(json& j, const SpeedrunPracticeEntryHistoryAttempt& obj) {
  if (obj.time) {
    j["time"] = obj.time.value();
  } else {
    j["time"] = nullptr;
  }
}

void from_json(const json& j, SpeedrunPracticeEntryHistoryAttempt& obj) {
  if (j["time"].is_null()) {
    obj.time = {};
  } else {
    obj.time = j["time"];
  }
}

void to_json(json& j, const SpeedrunPracticeEntry& obj) {
  json_serialize(name);
  json_serialize(continue_point_name);
  json_serialize(flags);
  json_serialize(completed_task);
  json_serialize(features);
  json_serialize(secrets);
  json_serialize(vehicles);
  json_serialize(starting_position);
  json_serialize(starting_rotation);
  json_serialize(starting_camera_position);
  json_serialize(starting_camera_rotation);
  json_serialize(start_zone_v1);
  json_serialize(start_zone_v2);
  json_serialize_optional(end_zone_v1);
  json_serialize_optional(end_zone_v2);
  json_serialize_optional(end_task);
  json_serialize(history);
}

void from_json(const json& j, SpeedrunPracticeEntry& obj) {
  json_deserialize_if_exists(name);
  json_deserialize_if_exists(continue_point_name);
  json_deserialize_if_exists(flags);
  json_deserialize_if_exists(completed_task);
  json_deserialize_if_exists(features);
  json_deserialize_if_exists(secrets);
  json_deserialize_if_exists(vehicles);
  json_deserialize_if_exists(starting_position);
  json_deserialize_if_exists(starting_rotation);
  json_deserialize_if_exists(starting_camera_position);
  json_deserialize_if_exists(starting_camera_rotation);
  json_deserialize_if_exists(start_zone_v1);
  json_deserialize_if_exists(start_zone_v2);
  json_deserialize_optional_if_exists(end_zone_v1);
  json_deserialize_optional_if_exists(end_zone_v2);
  json_deserialize_optional_if_exists(end_task);
  json_deserialize_if_exists(history);
}

void to_json(json& j, const SpeedrunCustomCategoryEntry& obj) {
  json_serialize(name);
  json_serialize(secrets);
  json_serialize(features);
  json_serialize(vehicles);
  json_serialize(forbidden_features);
  json_serialize(cheats);
  json_serialize(continue_point_name);
  json_serialize(completed_task);
}

void from_json(const json& j, SpeedrunCustomCategoryEntry& obj) {
  json_deserialize_if_exists(name);
  json_deserialize_if_exists(secrets);
  json_deserialize_if_exists(features);
  json_deserialize_if_exists(vehicles);
  json_deserialize_if_exists(forbidden_features);
  json_deserialize_if_exists(cheats);
  json_deserialize_if_exists(continue_point_name);
  json_deserialize_if_exists(completed_task);
}

std::vector<SpeedrunPracticeEntry> g_speedrun_practice_entries;
std::unordered_map<int, SpeedrunPracticeState> g_speedrun_practice_state;

s32 pc_sr_mode_get_practice_entries_amount() {
  // load practice entries from the file
  const auto file_path =
      file_util::get_user_features_dir(g_game_version) / "speedrun-practice.json";
  if (!file_util::file_exists(file_path.string())) {
    lg::info("speedrun-practice.json not found, no entries to return!");
    return 0;
  }
  const auto file_contents = safe_parse_json(file_util::read_text_file(file_path));
  if (!file_contents) {
    lg::error("speedrun-practice.json could not be parsed!");
    return 0;
  }

  g_speedrun_practice_entries = *file_contents;

  for (size_t i = 0; i < g_speedrun_practice_entries.size(); i++) {
    const auto& entry = g_speedrun_practice_entries.at(i);
    s32 last_session_id = -1;
    s32 total_attempts = 0;
    s32 total_successes = 0;
    s32 session_attempts = 0;
    s32 session_successes = 0;
    double total_time = 0;
    float average_time = 0;
    float fastest_time = 0;
    for (const auto& [history_session, times] : entry.history) {
      s32 session_id = stoi(history_session);
      if (session_id > last_session_id) {
        last_session_id = session_id;
      }
      for (const auto& time : times) {
        total_attempts++;
        if (time.time) {
          total_successes++;
          total_time += *time.time;
          if (fastest_time == 0 || *time.time < fastest_time) {
            fastest_time = *time.time;
          }
        }
      }
    }
    if (total_successes != 0) {
      average_time = total_time / total_successes;
    }
    g_speedrun_practice_state[i] = {last_session_id + 1, total_attempts,    total_successes,
                                    session_attempts,    session_successes, total_time,
                                    average_time,        fastest_time};
  }

  return g_speedrun_practice_entries.size();
}

void pc_sr_mode_get_practice_entry_name(s32 entry_index, u32 name_str_ptr) {
  std::string name;
  if (entry_index < (int)g_speedrun_practice_entries.size()) {
    name = g_speedrun_practice_entries.at(entry_index).name;
  }
  strcpy(Ptr<String>(name_str_ptr).c()->data(), name.c_str());
}

void pc_sr_mode_get_practice_entry_continue_point(s32 entry_index, u32 name_str_ptr) {
  std::string name;
  if (entry_index < (int)g_speedrun_practice_entries.size()) {
    name = g_speedrun_practice_entries.at(entry_index).continue_point_name;
  }
  strcpy(Ptr<String>(name_str_ptr).c()->data(), name.c_str());
}

s32 pc_sr_mode_get_practice_entry_history_success(s32 entry_index) {
  return g_speedrun_practice_state.at(entry_index).total_successes;
}

s32 pc_sr_mode_get_practice_entry_history_attempts(s32 entry_index) {
  return g_speedrun_practice_state.at(entry_index).total_attempts;
}

s32 pc_sr_mode_get_practice_entry_session_success(s32 entry_index) {
  return g_speedrun_practice_state.at(entry_index).session_successes;
}

s32 pc_sr_mode_get_practice_entry_session_attempts(s32 entry_index) {
  return g_speedrun_practice_state.at(entry_index).session_attempts;
}

void pc_sr_mode_get_practice_entry_avg_time(s32 entry_index, u32 time_str_ptr) {
  const auto time = fmt::format("{:.2f}", g_speedrun_practice_state.at(entry_index).average_time);
  strcpy(Ptr<String>(time_str_ptr).c()->data(), time.c_str());
}

void pc_sr_mode_get_practice_entry_fastest_time(s32 entry_index, u32 time_str_ptr) {
  const auto time = fmt::format("{:.2f}", g_speedrun_practice_state.at(entry_index).fastest_time);
  strcpy(Ptr<String>(time_str_ptr).c()->data(), time.c_str());
}

u64 pc_sr_mode_record_practice_entry_attempt(s32 entry_index, u32 success_bool, u32 time_ptr) {
  auto& state = g_speedrun_practice_state.at(entry_index);
  const auto was_successful = symbol_to_bool(success_bool);
  state.total_attempts++;
  state.session_attempts++;
  bool ret = false;
  SpeedrunPracticeEntryHistoryAttempt new_history_entry;
  if (was_successful) {
    auto time = Ptr<float>(time_ptr).c();
    new_history_entry.time = *time;
    state.total_successes++;
    state.session_successes++;
    state.total_time += *time;
    state.average_time = state.total_time / state.total_successes;
    if (*time < state.fastest_time) {
      state.fastest_time = *time;
      ret = true;
    }
  }
  // persist to file
  const auto file_path =
      file_util::get_user_features_dir(g_game_version) / "speedrun-practice.json";
  if (!file_util::file_exists(file_path.string())) {
    lg::info("speedrun-practice.json not found, not persisting!");
  } else {
    auto& history = g_speedrun_practice_entries.at(entry_index).history;
    if (history.find(fmt::format("{}", state.current_session_id)) == history.end()) {
      history[fmt::format("{}", state.current_session_id)] = {};
    }
    history[fmt::format("{}", state.current_session_id)].push_back(new_history_entry);
    json data = g_speedrun_practice_entries;
    file_util::write_text_file(file_path, data.dump(2));
  }
  // return
  return bool_to_symbol(ret);
}

void pc_sr_mode_init_practice_info(s32 entry_index, u32 speedrun_practice_obj_ptr) {
  if (entry_index >= (int)g_speedrun_practice_entries.size()) {
    return;
  }

  auto objective = speedrun_practice_obj_ptr
                       ? Ptr<SpeedrunPracticeObjective>(speedrun_practice_obj_ptr).c()
                       : NULL;
  if (objective) {
    const auto& json_info = g_speedrun_practice_entries.at(entry_index);

    objective->index = entry_index;
    objective->flags = json_info.flags;
    objective->completed_task = json_info.completed_task;
    objective->features = json_info.features;
    objective->vehicles = json_info.vehicles;
    objective->secrets = json_info.secrets;
    auto starting_position =
        objective->starting_position ? Ptr<Vector>(objective->starting_position).c() : NULL;
    if (starting_position) {
      for (int i = 0; i < 4; i++) {
        starting_position->data[i] = json_info.starting_position.at(i) * METER_LENGTH;
      }
    }
    auto starting_rotation =
        objective->starting_rotation ? Ptr<Vector>(objective->starting_rotation).c() : NULL;
    if (starting_rotation) {
      for (int i = 0; i < 4; i++) {
        starting_rotation->data[i] = json_info.starting_rotation.at(i);
      }
    }
    auto starting_camera_position = objective->starting_camera_position
                                        ? Ptr<Vector>(objective->starting_camera_position).c()
                                        : NULL;
    if (starting_camera_position) {
      for (int i = 0; i < 4; i++) {
        starting_camera_position->data[i] = json_info.starting_camera_position.at(i) * 4096.0;
      }
    }
    auto starting_camera_rotation = objective->starting_camera_rotation
                                        ? Ptr<Vector>(objective->starting_camera_rotation).c()
                                        : NULL;
    if (starting_camera_rotation) {
      for (int i = 0; i < 16; i++) {
        starting_camera_rotation->data[i] = json_info.starting_camera_rotation.at(i);
      }
    }

    if (json_info.end_task) {
      objective->end_task = *json_info.end_task;
    } else {
      objective->end_task = 0;
    }

    auto starting_zone = objective->start_zone_init_params
                             ? Ptr<ObjectiveZoneInitParams>(objective->start_zone_init_params).c()
                             : NULL;
    if (starting_zone) {
      starting_zone->v1[0] = json_info.start_zone_v1.at(0) * METER_LENGTH;
      starting_zone->v1[1] = json_info.start_zone_v1.at(1) * METER_LENGTH;
      starting_zone->v1[2] = json_info.start_zone_v1.at(2) * METER_LENGTH;
      starting_zone->v1[3] = json_info.start_zone_v1.at(3) * METER_LENGTH;
      starting_zone->v2[0] = json_info.start_zone_v2.at(0) * METER_LENGTH;
      starting_zone->v2[1] = json_info.start_zone_v2.at(1) * METER_LENGTH;
      starting_zone->v2[2] = json_info.start_zone_v2.at(2) * METER_LENGTH;
      starting_zone->v2[3] = json_info.start_zone_v2.at(3) * METER_LENGTH;
    }

    if (json_info.end_zone_v1 && json_info.end_zone_v2) {
      auto ending_zone = objective->end_zone_init_params
                             ? Ptr<ObjectiveZoneInitParams>(objective->end_zone_init_params).c()
                             : NULL;
      if (ending_zone) {
        ending_zone->v1[0] = json_info.end_zone_v1->at(0) * METER_LENGTH;
        ending_zone->v1[1] = json_info.end_zone_v1->at(1) * METER_LENGTH;
        ending_zone->v1[2] = json_info.end_zone_v1->at(2) * METER_LENGTH;
        ending_zone->v1[3] = json_info.end_zone_v1->at(3) * METER_LENGTH;
        ending_zone->v2[0] = json_info.end_zone_v2->at(0) * METER_LENGTH;
        ending_zone->v2[1] = json_info.end_zone_v2->at(1) * METER_LENGTH;
        ending_zone->v2[2] = json_info.end_zone_v2->at(2) * METER_LENGTH;
        ending_zone->v2[3] = json_info.end_zone_v2->at(3) * METER_LENGTH;
      }
    }
  }
}

std::vector<SpeedrunCustomCategoryEntry> g_speedrun_custom_categories;

s32 pc_sr_mode_get_custom_category_amount() {
  // load practice entries from the file
  const auto file_path =
      file_util::get_user_features_dir(g_game_version) / "speedrun-categories.json";
  if (!file_util::file_exists(file_path.string())) {
    lg::info("speedrun-categories.json not found, no entries to return!");
    return 0;
  }
  const auto file_contents = safe_parse_json(file_util::read_text_file(file_path));
  if (!file_contents) {
    lg::error("speedrun-categories.json could not be parsed!");
    return 0;
  }

  g_speedrun_custom_categories = *file_contents;

  return g_speedrun_custom_categories.size();
}

void pc_sr_mode_get_custom_category_name(s32 entry_index, u32 name_str_ptr) {
  std::string name;
  if (entry_index < (int)g_speedrun_custom_categories.size()) {
    name = g_speedrun_custom_categories.at(entry_index).name;
  }
  strcpy(Ptr<String>(name_str_ptr).c()->data(), name.c_str());
}

void pc_sr_mode_get_custom_category_continue_point(s32 entry_index, u32 name_str_ptr) {
  std::string name;
  if (entry_index < (int)g_speedrun_custom_categories.size()) {
    name = g_speedrun_custom_categories.at(entry_index).continue_point_name;
  }
  strcpy(Ptr<String>(name_str_ptr).c()->data(), name.c_str());
}

void pc_sr_mode_init_custom_category_info(s32 entry_index, u32 speedrun_custom_category_ptr) {
  if (entry_index >= (int)g_speedrun_custom_categories.size()) {
    return;
  }

  auto category = speedrun_custom_category_ptr
                      ? Ptr<SpeedrunCustomCategory>(speedrun_custom_category_ptr).c()
                      : NULL;
  if (category) {
    const auto& json_info = g_speedrun_custom_categories.at(entry_index);
    category->index = entry_index;
    category->secrets = json_info.secrets;
    category->features = json_info.features;
    category->vehicles = json_info.vehicles;
    category->forbidden_features = json_info.forbidden_features;
    category->cheats = json_info.cheats;
    category->completed_task = json_info.completed_task;
  }
}

void pc_sr_mode_dump_new_custom_category(u32 speedrun_custom_category_ptr) {
  const auto file_path =
      file_util::get_user_features_dir(g_game_version) / "speedrun-categories.json";
  if (file_util::file_exists(file_path.string())) {
    // read current categories from file
    const auto file_contents = safe_parse_json(file_util::read_text_file(file_path));
    if (file_contents) {
      g_speedrun_custom_categories = *file_contents;
    }
  }

  auto category = speedrun_custom_category_ptr
                      ? Ptr<SpeedrunCustomCategory>(speedrun_custom_category_ptr).c()
                      : NULL;
  if (category) {
    SpeedrunCustomCategoryEntry new_category;
    new_category.name = fmt::format("custom-category-{}", g_speedrun_custom_categories.size());
    new_category.secrets = category->secrets;
    new_category.features = category->features;
    new_category.vehicles = category->vehicles;
    new_category.forbidden_features = category->forbidden_features;
    new_category.cheats = category->cheats;
    new_category.completed_task = category->completed_task;
    new_category.continue_point_name = "";
    g_speedrun_custom_categories.push_back(new_category);
    // convert to json and write file
    json data = g_speedrun_custom_categories;
    file_util::write_text_file(file_path, data.dump(2));
  }
}

}  // namespace kmachine_extras
}  // namespace jak3

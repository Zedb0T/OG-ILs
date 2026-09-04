#include "common/replay/replay_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "third-party/json.hpp"

namespace replay {
namespace {

using json = nlohmann::json;
constexpr float kGameUnitsPerMeter = 4096.f;
constexpr float kTicksPerSecond = 300.f;

std::string bounded_string(const auto& value) {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return std::string(value.begin(), end);
}

template <std::size_t Size>
void set_bounded_string(std::array<char, Size>& destination,
                        std::string_view source,
                        const char* field_name,
                        bool reject_overflow) {
  if (source.size() >= Size && reject_overflow) {
    throw FormatError(std::string(field_name) + " is too long");
  }
  destination.fill(0);
  const auto copy_size = std::min(source.size(), Size - 1);
  for (std::size_t i = 0; i < copy_size; ++i) {
    const auto byte = static_cast<unsigned char>(source[i]);
    // GOAL's strings are byte strings, not guaranteed UTF-8. Runtime state and
    // animation names are identifiers, so keep their printable ASCII bytes and
    // replace anything else before it reaches the JSON serializer.
    destination[i] =
        (!reject_overflow && (byte < 0x20 || byte > 0x7e)) ? '?' : static_cast<char>(byte);
  }
}

bool valid_identifier(std::string_view value) {
  if (value.empty() || value.size() > kMaxCategoryBytes) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
  });
}

void require(bool condition, const char* message) {
  if (!condition) {
    throw FormatError(message);
  }
}

template <std::size_t Size>
std::array<float, Size> parse_float_array(const json& value, const char* field_name) {
  if (!value.is_array() || value.size() != Size) {
    throw FormatError(std::string(field_name) + " has the wrong size");
  }
  std::array<float, Size> result = {};
  for (std::size_t i = 0; i < Size; ++i) {
    require(value.at(i).is_number(), "sample vector contains a non-number");
    result[i] = value.at(i).get<float>();
    require(std::isfinite(result[i]), "sample vector contains a non-finite number");
  }
  return result;
}

void validate_file(const File& replay) {
  require(replay.game == "jak3", "unsupported replay game");
  require(valid_identifier(replay.category), "invalid replay category");
  require(replay.sample_rate_hz == kSampleRateHz, "unsupported replay sample rate");
  require(!replay.samples.empty(), "replay has no samples");
  require(replay.samples.size() <= kMaxSamples, "replay has too many samples");
  require(std::isfinite(replay.duration_seconds) && replay.duration_seconds >= 0.f,
          "invalid replay duration");
  require(replay.duration_seconds <= static_cast<float>(kMaxDurationSeconds) + 1.f,
          "replay duration exceeds limit");

  float previous_time = -1.f;
  for (const auto& sample : replay.samples) {
    require(std::isfinite(sample.time_seconds) && sample.time_seconds >= 0.f,
            "invalid sample time");
    require(sample.time_seconds >= previous_time, "sample times are not ordered");
    require(sample.time_seconds <= static_cast<float>(kMaxDurationSeconds) + 1.f,
            "sample time exceeds limit");
    previous_time = sample.time_seconds;
    for (const auto component : sample.position_meters) {
      require(std::isfinite(component), "position contains a non-finite number");
    }
    for (const auto component : sample.rotation) {
      require(std::isfinite(component), "rotation contains a non-finite number");
    }
    for (const auto component : sample.velocity_meters) {
      require(std::isfinite(component), "velocity contains a non-finite number");
    }
    require(std::isfinite(sample.animation_frame), "animation frame is non-finite");
    for (const auto component : sample.extra_position_meters) {
      require(std::isfinite(component), "extra position contains a non-finite number");
    }
    for (const auto component : sample.extra_rotation) {
      require(std::isfinite(component), "extra rotation contains a non-finite number");
    }
    for (const auto component : sample.extra_scale) {
      require(std::isfinite(component), "extra scale contains a non-finite number");
    }
    require(std::isfinite(sample.extra_animation_frame),
            "extra animation frame is non-finite");
    require(sample.state[0] != '\0', "sample state is empty");
  }
}

json sample_to_json(const Sample& sample) {
  return json::array({sample.time_seconds, sample.position_meters, sample.rotation,
                      sample.velocity_meters, sample.status, sample.animation_frame,
                      bounded_string(sample.state), bounded_string(sample.animation),
                      bounded_string(sample.extra_art_group), sample.extra_position_meters,
                      sample.extra_rotation, sample.extra_scale, sample.extra_animation_frame,
                      bounded_string(sample.extra_animation)});
}

void replace_file(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "could not atomically replace replay file");
  }
#else
  if (std::rename(source.string().c_str(), destination.string().c_str()) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "could not atomically replace replay file");
  }
#endif
}

}  // namespace

std::string serialize(const File& replay) {
  validate_file(replay);
  json root = {{"schema", "opengoal-replay"},
               {"version", kSchemaVersion},
               {"game", replay.game},
               {"category", replay.category},
               {"completed", replay.completed},
               {"truncated", replay.truncated},
               {"duration_seconds", replay.duration_seconds},
               {"sample_rate_hz", replay.sample_rate_hz},
               {"sample_count", replay.samples.size()},
               {"units", "meters"},
               {"sample_keys",
                {"time", "position", "rotation", "velocity", "status", "animation_frame",
                 "state", "animation", "extra_art_group", "extra_position", "extra_rotation",
                 "extra_scale", "extra_animation_frame", "extra_animation"}}};
  auto& samples = root["samples"] = json::array();
  for (const auto& sample : replay.samples) {
    samples.push_back(sample_to_json(sample));
  }
  // The recorder sanitizes runtime metadata, but replacement here is a final
  // containment boundary for programmatically constructed File values.
  return root.dump(-1, ' ', false, json::error_handler_t::replace);
}

File parse(std::string_view contents) {
  require(contents.size() <= kMaxFileBytes, "replay file is too large");
  json root;
  try {
    root = json::parse(contents);
  } catch (const json::exception& error) {
    throw FormatError(std::string("malformed replay JSON: ") + error.what());
  }

  try {
    require(root.is_object(), "replay root is not an object");
    require(root.at("schema") == "opengoal-replay", "unsupported replay schema");
    const auto version = root.at("version").get<std::uint32_t>();
    // Keep phase 1/2 PBs playable; v3 adds the optional companion drawable.
    require(version >= 1 && version <= kSchemaVersion, "unsupported replay version");
    require(root.at("units") == "meters", "unsupported replay units");

    File replay;
    replay.game = root.at("game").get<std::string>();
    replay.category = root.at("category").get<std::string>();
    replay.completed = root.at("completed").get<bool>();
    replay.truncated = root.at("truncated").get<bool>();
    replay.duration_seconds = root.at("duration_seconds").get<float>();
    replay.sample_rate_hz = root.at("sample_rate_hz").get<std::uint32_t>();

    const auto declared_count = root.at("sample_count").get<std::size_t>();
    require(declared_count <= kMaxSamples, "replay declares too many samples");
    const auto& samples = root.at("samples");
    require(samples.is_array(), "replay samples is not an array");
    require(samples.size() == declared_count, "replay sample count does not match payload");
    replay.samples.reserve(declared_count);

    for (const auto& encoded : samples) {
      const auto expected_size = version == 1 ? 7u : (version == 2 ? 8u : 14u);
      require(encoded.is_array() && encoded.size() == expected_size, "malformed replay sample");
      Sample sample;
      sample.time_seconds = encoded.at(0).get<float>();
      sample.position_meters = parse_float_array<3>(encoded.at(1), "position");
      sample.rotation = parse_float_array<4>(encoded.at(2), "rotation");
      sample.velocity_meters = parse_float_array<3>(encoded.at(3), "velocity");
      sample.status = encoded.at(4).get<std::uint32_t>();
      const auto metadata_offset = version == 1 ? 0u : 1u;
      if (version >= 2) {
        sample.animation_frame = encoded.at(5).get<float>();
      }
      const auto state = encoded.at(5 + metadata_offset).get<std::string>();
      const auto animation = encoded.at(6 + metadata_offset).get<std::string>();
      set_bounded_string(sample.state, state, "state", true);
      set_bounded_string(sample.animation, animation, "animation", true);
      if (version >= 3) {
        const auto extra_art_group = encoded.at(8).get<std::string>();
        sample.extra_position_meters = parse_float_array<3>(encoded.at(9), "extra position");
        sample.extra_rotation = parse_float_array<4>(encoded.at(10), "extra rotation");
        sample.extra_scale = parse_float_array<4>(encoded.at(11), "extra scale");
        sample.extra_animation_frame = encoded.at(12).get<float>();
        const auto extra_animation = encoded.at(13).get<std::string>();
        set_bounded_string(sample.extra_art_group, extra_art_group, "extra art group", true);
        set_bounded_string(sample.extra_animation, extra_animation, "extra animation", true);
      }
      replay.samples.push_back(sample);
    }
    validate_file(replay);
    return replay;
  } catch (const FormatError&) {
    throw;
  } catch (const json::exception& error) {
    throw FormatError(std::string("invalid replay fields: ") + error.what());
  }
}

File load(const fs::path& path) {
  const auto bytes = file_util::read_binary_file(path);
  require(bytes.size() <= kMaxFileBytes, "replay file is too large");
  return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void atomic_save(const fs::path& path, const File& replay) {
  const auto data = serialize(replay);
  require(data.size() <= kMaxFileBytes, "serialized replay is too large");
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path());
  }
  auto temporary = path;
  temporary += ".tmp";
  try {
    file_util::write_binary_file(temporary, data.data(), data.size());
    replace_file(temporary, path);
  } catch (...) {
    std::error_code error;
    fs::remove(temporary, error);
    throw;
  }
}

Recorder::Recorder(std::size_t max_samples) : m_samples(max_samples) {
  if (max_samples == 0 || max_samples > kMaxSamples) {
    throw std::invalid_argument("invalid replay recorder capacity");
  }
  m_game.reserve(8);
  m_category.reserve(kMaxCategoryBytes);
}

bool Recorder::start(std::string_view game, std::string_view category, std::int64_t start_ticks) {
  if (game != "jak3" || !valid_identifier(category)) {
    cancel();
    return false;
  }
  m_game.assign(game);
  m_category.assign(category);
  m_count = 0;
  m_start_ticks = start_ticks;
  m_active = true;
  m_truncated = false;
  return true;
}

bool Recorder::add_sample(std::int64_t now_ticks,
                          const float* position_game_units,
                          const float* rotation,
                          const float* velocity_game_units,
                          std::string_view state,
                          std::string_view animation,
                          float animation_frame,
                          std::uint32_t status) {
  if (!m_active || !position_game_units || !rotation || !velocity_game_units ||
      now_ticks < m_start_ticks) {
    return false;
  }
  const auto elapsed_ticks = now_ticks - m_start_ticks;
  if (elapsed_ticks > static_cast<std::int64_t>(kMaxDurationSeconds) * 300) {
    m_truncated = true;
    return false;
  }
  if (m_count == m_samples.size()) {
    m_truncated = true;
    return false;
  }

  auto& sample = m_samples[m_count++];
  sample = {};
  sample.time_seconds = static_cast<float>(elapsed_ticks) / kTicksPerSecond;
  for (std::size_t i = 0; i < 3; ++i) {
    sample.position_meters[i] = position_game_units[i] / kGameUnitsPerMeter;
    sample.velocity_meters[i] = velocity_game_units[i] / kGameUnitsPerMeter;
  }
  std::copy_n(rotation, sample.rotation.size(), sample.rotation.begin());
  sample.status = status;
  sample.animation_frame = animation_frame;
  set_bounded_string(sample.state, state.empty() ? "none" : state, "state", false);
  set_bounded_string(sample.animation, animation, "animation", false);
  return true;
}

bool Recorder::update_last_sample_metadata(std::string_view state,
                                           std::string_view animation,
                                           float animation_frame,
                                           std::uint32_t status) {
  if (!m_active || m_count == 0) {
    return false;
  }
  auto& sample = m_samples[m_count - 1];
  sample.status = status;
  sample.animation_frame = animation_frame;
  set_bounded_string(sample.state, state.empty() ? "none" : state, "state", false);
  set_bounded_string(sample.animation, animation, "animation", false);
  return true;
}

bool Recorder::update_last_sample_extra(std::string_view art_group,
                                        const float* position_game_units,
                                        const float* rotation,
                                        const float* scale,
                                        std::string_view animation,
                                        float animation_frame) {
  if (!m_active || m_count == 0 || art_group.empty() || !position_game_units || !rotation ||
      !scale || !std::isfinite(animation_frame)) {
    return false;
  }
  auto& sample = m_samples[m_count - 1];
  set_bounded_string(sample.extra_art_group, art_group, "extra art group", false);
  set_bounded_string(sample.extra_animation, animation, "extra animation", false);
  for (std::size_t i = 0; i < 3; ++i) {
    sample.extra_position_meters[i] = position_game_units[i] / kGameUnitsPerMeter;
  }
  std::copy_n(rotation, sample.extra_rotation.size(), sample.extra_rotation.begin());
  std::copy_n(scale, sample.extra_scale.size(), sample.extra_scale.begin());
  sample.extra_animation_frame = animation_frame;
  return true;
}

bool Recorder::update_last_sample_extra_animation(std::string_view animation, float animation_frame) {
  if (!m_active || m_count == 0 || m_samples[m_count - 1].extra_art_group[0] == '\0' ||
      !std::isfinite(animation_frame)) {
    return false;
  }
  auto& sample = m_samples[m_count - 1];
  set_bounded_string(sample.extra_animation, animation, "extra animation", false);
  sample.extra_animation_frame = animation_frame;
  return true;
}

File Recorder::finish(bool completed) {
  if (!m_active || m_count == 0) {
    cancel();
    throw FormatError("cannot finish an empty or inactive recording");
  }
  File result;
  result.game = m_game;
  result.category = m_category;
  result.completed = completed;
  result.truncated = m_truncated;
  result.duration_seconds = m_samples[m_count - 1].time_seconds;
  result.samples.assign(m_samples.begin(), m_samples.begin() + m_count);
  m_active = false;
  m_count = 0;
  return result;
}

void Recorder::cancel() {
  m_active = false;
  m_truncated = false;
  m_count = 0;
}

}  // namespace replay

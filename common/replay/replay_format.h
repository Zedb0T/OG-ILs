#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "common/util/FileUtil.h"

namespace replay {

constexpr std::uint32_t kSchemaVersion = 3;
constexpr std::uint32_t kSampleRateHz = 60;
constexpr std::size_t kMaxDurationSeconds = 10 * 60;
constexpr std::size_t kMaxSamples = kSampleRateHz * kMaxDurationSeconds;
constexpr std::size_t kMaxFileBytes = 32 * 1024 * 1024;
constexpr std::size_t kMaxCategoryBytes = 96;
constexpr std::size_t kMaxStateBytes = 48;
constexpr std::size_t kMaxAnimationBytes = 64;
constexpr std::size_t kMaxArtGroupBytes = 64;

struct Sample {
  float time_seconds = 0.f;
  std::array<float, 3> position_meters = {};
  std::array<float, 4> rotation = {0.f, 0.f, 0.f, 1.f};
  std::array<float, 3> velocity_meters = {};
  std::uint32_t status = 0;
  float animation_frame = 0.f;
  std::array<char, kMaxStateBytes> state = {};
  std::array<char, kMaxAnimationBytes> animation = {};
  // Optional companion drawable (jetboard, mech, vehicle, etc.). Its art name
  // is empty when this sample has no replay extra.
  std::array<char, kMaxArtGroupBytes> extra_art_group = {};
  std::array<float, 3> extra_position_meters = {};
  std::array<float, 4> extra_rotation = {0.f, 0.f, 0.f, 1.f};
  std::array<float, 4> extra_scale = {1.f, 1.f, 1.f, 1.f};
  float extra_animation_frame = 0.f;
  std::array<char, kMaxAnimationBytes> extra_animation = {};
};

struct File {
  std::string game;
  std::string category;
  bool completed = false;
  bool truncated = false;
  float duration_seconds = 0.f;
  std::uint32_t sample_rate_hz = kSampleRateHz;
  std::vector<Sample> samples;
};

class FormatError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string serialize(const File& replay);
File parse(std::string_view contents);
File load(const fs::path& path);
void atomic_save(const fs::path& path, const File& replay);

class Recorder {
 public:
  explicit Recorder(std::size_t max_samples = kMaxSamples);

  bool start(std::string_view game, std::string_view category, std::int64_t start_ticks);
  bool add_sample(std::int64_t now_ticks,
                  const float* position_game_units,
                  const float* rotation,
                  const float* velocity_game_units,
                  std::string_view state,
                  std::string_view animation,
                  float animation_frame,
                  std::uint32_t status);
  bool update_last_sample_metadata(std::string_view state,
                                   std::string_view animation,
                                   float animation_frame,
                                   std::uint32_t status);
  bool update_last_sample_extra(std::string_view art_group,
                                const float* position_game_units,
                                const float* rotation,
                                const float* scale,
                                std::string_view animation,
                                float animation_frame);
  File finish(bool completed);
  bool update_last_sample_extra_animation(std::string_view animation, float animation_frame);
  void cancel();

  bool active() const { return m_active; }
  bool truncated() const { return m_truncated; }
  std::size_t sample_count() const { return m_count; }
  std::size_t capacity() const { return m_samples.size(); }
  const Sample* storage_address() const { return m_samples.data(); }

 private:
  std::vector<Sample> m_samples;
  std::size_t m_count = 0;
  std::int64_t m_start_ticks = 0;
  std::string m_game;
  std::string m_category;
  bool m_active = false;
  bool m_truncated = false;
};

}  // namespace replay

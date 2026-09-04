#include <limits>

#include "common/replay/replay_format.h"

#include "gtest/gtest.h"

#include "third-party/json.hpp"

namespace {

replay::File make_replay() {
  replay::Recorder recorder(4);
  const float position[] = {4096.f, 8192.f, -4096.f, 1.f};
  const float rotation[] = {0.f, 0.5f, 0.f, 0.8660254f};
  const float velocity[] = {2048.f, 0.f, -1024.f, 0.f};
  EXPECT_TRUE(recorder.start("jak3", "wascity-bbush-get-to-18", 900));
  EXPECT_TRUE(
      recorder.add_sample(900, position, rotation, velocity, "target-idle", "jakb-idle", 1.25f, 7));
  EXPECT_TRUE(
      recorder.add_sample(905, position, rotation, velocity, "target-run", "jakb-run", 2.5f, 9));
  const float extra_position[] = {12288.f, 4096.f, -8192.f, 1.f};
  const float extra_scale[] = {1.f, 1.f, 1.f, 1.f};
  EXPECT_TRUE(recorder.update_last_sample_extra("skel-board", extra_position, rotation, extra_scale,
                                                "board-open", 4.25f));
  return recorder.finish(true);
}

TEST(ReplayFormat, RoundTripPreservesSamples) {
  const auto original = make_replay();
  const auto decoded = replay::parse(replay::serialize(original));

  EXPECT_EQ(decoded.game, "jak3");
  EXPECT_EQ(decoded.category, "wascity-bbush-get-to-18");
  EXPECT_TRUE(decoded.completed);
  EXPECT_FALSE(decoded.truncated);
  ASSERT_EQ(decoded.samples.size(), 2);
  EXPECT_FLOAT_EQ(decoded.samples[0].position_meters[0], 1.f);
  EXPECT_FLOAT_EQ(decoded.samples[0].position_meters[1], 2.f);
  EXPECT_FLOAT_EQ(decoded.samples[0].velocity_meters[0], 0.5f);
  EXPECT_FLOAT_EQ(decoded.samples[1].time_seconds, 5.f / 300.f);
  EXPECT_FLOAT_EQ(decoded.samples[1].animation_frame, 2.5f);
  EXPECT_STREQ(decoded.samples[1].state.data(), "target-run");
  EXPECT_STREQ(decoded.samples[1].animation.data(), "jakb-run");
  EXPECT_STREQ(decoded.samples[1].extra_art_group.data(), "skel-board");
  EXPECT_FLOAT_EQ(decoded.samples[1].extra_position_meters[0], 3.f);
  EXPECT_FLOAT_EQ(decoded.samples[1].extra_animation_frame, 4.25f);
  EXPECT_STREQ(decoded.samples[1].extra_animation.data(), "board-open");
}

TEST(ReplayFormat, RecorderStorageIsBoundedAndReused) {
  replay::Recorder recorder(2);
  const auto* storage = recorder.storage_address();
  const float vector[] = {0.f, 0.f, 0.f, 1.f};

  ASSERT_TRUE(recorder.start("jak3", "test-mission", 0));
  EXPECT_TRUE(recorder.add_sample(0, vector, vector, vector, "idle", "", 0.f, 0));
  EXPECT_TRUE(recorder.add_sample(5, vector, vector, vector, "idle", "", 0.f, 0));
  EXPECT_FALSE(recorder.add_sample(10, vector, vector, vector, "idle", "", 0.f, 0));
  EXPECT_TRUE(recorder.truncated());
  const auto first = recorder.finish(false);
  EXPECT_TRUE(first.truncated);
  EXPECT_EQ(first.samples.size(), 2);

  ASSERT_TRUE(recorder.start("jak3", "test-mission", 100));
  EXPECT_EQ(recorder.storage_address(), storage);
  EXPECT_EQ(recorder.capacity(), 2);
  EXPECT_TRUE(recorder.add_sample(100, vector, vector, vector, "idle", "", 0.f, 0));
  EXPECT_EQ(recorder.finish(true).samples.size(), 1);
}

TEST(ReplayFormat, RecorderEnforcesWallClockDurationLimit) {
  replay::Recorder recorder(2);
  const float vector[] = {0.f, 0.f, 0.f, 1.f};

  ASSERT_TRUE(recorder.start("jak3", "test-mission", 100));
  ASSERT_TRUE(recorder.add_sample(100, vector, vector, vector, "idle", "", 0.f, 0));
  EXPECT_FALSE(recorder.add_sample(100 + replay::kMaxDurationSeconds * 300 + 1, vector, vector,
                                   vector, "idle", "", 0.f, 0));
  EXPECT_TRUE(recorder.truncated());
  const auto result = recorder.finish(false);
  EXPECT_TRUE(result.truncated);
  ASSERT_EQ(result.samples.size(), 1);
  EXPECT_FLOAT_EQ(result.duration_seconds, 0.f);
}

TEST(ReplayFormat, SanitizesNonUtf8RuntimeMetadataBeforeSaving) {
  replay::Recorder recorder(1);
  const float vector[] = {0.f, 0.f, 0.f, 1.f};
  const std::string invalid_state{"target-\xffidle", 12};
  const std::string invalid_animation{"jak\x01run", 7};

  ASSERT_TRUE(recorder.start("jak3", "test-mission", 0));
  ASSERT_TRUE(
      recorder.add_sample(0, vector, vector, vector, invalid_state, invalid_animation, 0.f, 0));

  const auto decoded = replay::parse(replay::serialize(recorder.finish(false)));
  ASSERT_EQ(decoded.samples.size(), 1);
  EXPECT_STREQ(decoded.samples[0].state.data(), "target-?idle");
  EXPECT_STREQ(decoded.samples[0].animation.data(), "jak?run");
}

TEST(ReplayFormat, SplitMetadataUpdateTargetsTheLatestSample) {
  replay::Recorder recorder(2);
  const float vector[] = {0.f, 0.f, 0.f, 1.f};

  ASSERT_TRUE(recorder.start("jak3", "test-mission", 0));
  ASSERT_TRUE(recorder.add_sample(0, vector, vector, vector, "none", "", 0.f, 0));
  ASSERT_TRUE(recorder.update_last_sample_metadata("target-run", "jakb-run", 3.75f, 9));
  const auto result = recorder.finish(true);

  ASSERT_EQ(result.samples.size(), 1);
  EXPECT_STREQ(result.samples[0].state.data(), "target-run");
  EXPECT_STREQ(result.samples[0].animation.data(), "jakb-run");
  EXPECT_FLOAT_EQ(result.samples[0].animation_frame, 3.75f);
  EXPECT_EQ(result.samples[0].status, 9);
}

TEST(ReplayFormat, SplitExtraAnimationUpdateDoesNotLeakIntoOnFootSamples) {
  replay::Recorder recorder(3);
  const float position[] = {4096.f, 8192.f, 12288.f, 1.f};
  const float rotation[] = {0.f, 0.f, 0.f, 1.f};
  const float scale[] = {1.f, 2.f, 3.f, 1.f};
  EXPECT_FALSE(recorder.update_last_sample_extra_animation("board-open", 5.f));
  ASSERT_TRUE(recorder.start("jak3", "test-mission", 0));
  ASSERT_TRUE(recorder.add_sample(0, position, rotation, rotation, "none", "", 0.f, 0));
  EXPECT_FALSE(recorder.update_last_sample_extra_animation("board-open", 5.f));
  ASSERT_TRUE(recorder.update_last_sample_extra("board", position, rotation, scale, "", 0.f));
  ASSERT_TRUE(recorder.update_last_sample_extra_animation("board-open", 5.5f));
  EXPECT_FALSE(recorder.update_last_sample_extra_animation(
      "board-close", std::numeric_limits<float>::infinity()));
  ASSERT_TRUE(recorder.add_sample(5, position, rotation, rotation, "none", "", 0.f, 0));
  EXPECT_FALSE(recorder.update_last_sample_extra_animation("board-close", 7.f));

  const auto decoded = replay::parse(replay::serialize(recorder.finish(true)));
  ASSERT_EQ(decoded.samples.size(), 2);
  EXPECT_STREQ(decoded.samples[0].extra_art_group.data(), "board");
  EXPECT_STREQ(decoded.samples[0].extra_animation.data(), "board-open");
  EXPECT_FLOAT_EQ(decoded.samples[0].extra_animation_frame, 5.5f);
  EXPECT_FLOAT_EQ(decoded.samples[0].extra_position_meters[1], 2.f);
  EXPECT_FLOAT_EQ(decoded.samples[0].extra_scale[1], 2.f);
  EXPECT_STREQ(decoded.samples[1].extra_art_group.data(), "");
  EXPECT_STREQ(decoded.samples[1].extra_animation.data(), "");
  EXPECT_FALSE(recorder.update_last_sample_extra_animation("board-open", 5.f));
}

TEST(ReplayFormat, LoadsPhaseOneFilesWithoutAnimationFrames) {
  auto encoded = nlohmann::json::parse(replay::serialize(make_replay()));
  encoded["version"] = 1;
  encoded["sample_keys"] =
      {"time", "position", "rotation", "velocity", "status", "state", "animation"};
  for (auto& sample : encoded["samples"]) {
    while (sample.size() > 8) {
      sample.erase(sample.end() - 1);
    }
    sample.erase(sample.begin() + 5);
  }

  const auto decoded = replay::parse(encoded.dump());
  ASSERT_EQ(decoded.samples.size(), 2);
  EXPECT_FLOAT_EQ(decoded.samples[1].animation_frame, 0.f);
  EXPECT_STREQ(decoded.samples[1].state.data(), "target-run");
  EXPECT_STREQ(decoded.samples[1].animation.data(), "jakb-run");
}

TEST(ReplayFormat, LoadsPhaseTwoFilesWithoutDrawableExtras) {
  auto encoded = nlohmann::json::parse(replay::serialize(make_replay()));
  encoded["version"] = 2;
  encoded["sample_keys"] =
      {"time", "position", "rotation", "velocity", "status", "animation_frame", "state",
       "animation"};
  for (auto& sample : encoded["samples"]) {
    while (sample.size() > 8) {
      sample.erase(sample.end() - 1);
    }
  }

  const auto decoded = replay::parse(encoded.dump());
  ASSERT_EQ(decoded.samples.size(), 2);
  EXPECT_STREQ(decoded.samples[1].extra_art_group.data(), "");
  EXPECT_STREQ(decoded.samples[1].extra_animation.data(), "");
  EXPECT_FLOAT_EQ(decoded.samples[1].extra_scale[0], 1.f);
}

TEST(ReplayFormat, RejectsMalformedOversizedAndIncompatibleFiles) {
  EXPECT_THROW(replay::parse("not json"), replay::FormatError);
  EXPECT_THROW(replay::parse(std::string(replay::kMaxFileBytes + 1, 'x')), replay::FormatError);

  auto encoded = nlohmann::json::parse(replay::serialize(make_replay()));
  encoded["version"] = replay::kSchemaVersion + 1;
  EXPECT_THROW(replay::parse(encoded.dump()), replay::FormatError);

  encoded = nlohmann::json::parse(replay::serialize(make_replay()));
  encoded["sample_count"] = replay::kMaxSamples + 1;
  EXPECT_THROW(replay::parse(encoded.dump()), replay::FormatError);

  encoded = nlohmann::json::parse(replay::serialize(make_replay()));
  encoded["samples"][0][1][0] = std::numeric_limits<double>::infinity();
  EXPECT_THROW(replay::parse(encoded.dump()), replay::FormatError);
}

TEST(ReplayFormat, AtomicSaveLeavesAValidCompleteFile) {
  const auto directory = fs::path("out") / "test" / "replay-format";
  const auto path = directory / "last-attempt.ogr.json";
  fs::create_directories(directory);

  auto first = make_replay();
  replay::atomic_save(path, first);
  auto loaded = replay::load(path);
  EXPECT_TRUE(loaded.completed);
  EXPECT_EQ(loaded.samples.size(), 2);

  first.completed = false;
  replay::atomic_save(path, first);
  loaded = replay::load(path);
  EXPECT_FALSE(loaded.completed);
  EXPECT_FALSE(fs::exists(path.string() + ".tmp"));
}

}  // namespace

#include <chrono>
#include <cstdlib>
#include <thread>

#include "game/system/replay_client.h"
#include "gtest/gtest.h"
#include "third-party/json.hpp"

TEST(ReplayClient, BootIdentityFromSelectedServer) {
  // Only the Python loopback harness may enable this test. It supplies a fresh
  // profile and fake credentials; never ping the user's servers from a test.
  const auto* profile = std::getenv("OG_REPLAY_TEST_PROFILE");
  if (!profile) GTEST_SKIP() << "Run tools/replay-server/test_client_identity.py";
  file_util::override_user_config_dir(fs::path(profile), true);
  const auto saved = nlohmann::json::parse(file_util::read_text_file(
      file_util::get_user_features_dir(GameVersion::Jak3) / "ghost-client.json"));
  ASSERT_EQ(replay_client::server_status().url, saved.at("server").get<std::string>());
  auto wait_for_ping = [] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (replay_client::text(5, 0) == "Pinging server..." &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_NE(replay_client::text(5, 0), "Pinging server...");
  };
  ASSERT_EQ(replay_client::command(17, 0, ""), 1);
  wait_for_ping();
  EXPECT_EQ(replay_client::text(4, 0), "Welcome back Zed");
  // Per-frame calls must not spam requests.
  for (int i = 0; i < 1000; ++i) EXPECT_EQ(replay_client::command(17, 0, ""), 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  ASSERT_EQ(replay_client::command(16, 0, ""), 1);
  wait_for_ping();
  EXPECT_EQ(replay_client::text(4, 0), "Welcome back Zed");
  EXPECT_EQ(replay_client::text(5, 0), "Ping failed: Ghost server rejected request (503)");
  EXPECT_EQ(replay_client::command(17, 0, ""), 0); // no automatic retry loop

  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  ASSERT_EQ(replay_client::command(16, 0, ""), 1);
  wait_for_ping();
  EXPECT_EQ(replay_client::text(4, 0), "Welcome back New_Name"); // sanitize GOAL directives

  // Switching clears the old server's name. Do not tick the new endpoint: this
  // harness deliberately never sends a fake identity to the public service.
  ASSERT_TRUE(replay_client::set_server(replay_client::Server::SparkedHost));
  EXPECT_EQ(replay_client::text(4, 0), "Undetected player - Press L3 + D-pad Down to ping server");
  EXPECT_TRUE(replay_client::text(5, 0).empty());
}

TEST(ReplayClient, NullCustomSelectionsAreSafeAndMenuReadsDoNotMutateSettings) {
  const auto* profile = std::getenv("OG_REPLAY_TEST_PROFILE");
  if (!profile) GTEST_SKIP() << "Run tools/replay-server/test_client_identity.py";
  file_util::override_user_config_dir(fs::path(profile), true);
  const auto path = file_util::get_user_features_dir(GameVersion::Jak3) / "ghost-client.json";
  auto read_config = [&] { return nlohmann::json::parse(file_util::read_text_file(path)); };
  const auto original = read_config();
  EXPECT_EQ(replay_client::command(0, 0, ""), 4);
  const auto repaired = read_config();
  EXPECT_EQ(repaired.at("player_id"), original.at("player_id"));
  EXPECT_EQ(repaired.at("player_token"), original.at("player_token"));
  EXPECT_EQ(repaired.at("custom").at("wascity-bbush-get-to-18"), nlohmann::json::array());
  EXPECT_EQ(repaired.at("custom").at("bad-entry"), nlohmann::json::array());
  EXPECT_EQ(repaired.at("custom").at("valid-entry"), nlohmann::json::array({std::string(32, 'd')}));

  for (const auto* category : {"wascity-bbush-get-to-18", "previously-unseen-mission"}) {
    replay_client::prepare(category);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (replay_client::text(0, 0) == "Loading ghosts..." &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(replay_client::text(0, 0), "No replay available for this mode");
    ASSERT_EQ(replay_client::command(3, 0, ""), 1);
    EXPECT_TRUE(replay_client::snapshot(category).empty());
    for (int i = 0; i < 10; ++i) EXPECT_EQ(replay_client::text(1, 0), "[ ] Test Player 1.000s");
    EXPECT_EQ(replay_client::command(11, 0, ""), 1); // persist after reading the menu
    EXPECT_EQ(read_config(), repaired); // no inserted null/missing mission key
  }
  // Repair legacy server-specific selections too, without making HTTP requests.
  EXPECT_EQ(replay_client::command(1, 4, ""), 4); // invalidate category before switching
  ASSERT_TRUE(replay_client::set_server(replay_client::Server::Localhost));
  EXPECT_EQ(read_config().at("custom").at("legacy-mission"), nlohmann::json::array());
}

TEST(ReplayClient, InventoryLeaderboardIsPagedCachedAndReadOnly) {
  const auto* profile = std::getenv("OG_LEADERBOARD_TEST_PROFILE");
  if (!profile) GTEST_SKIP() << "Run tools/replay-server/test_client_leaderboard.py";
  file_util::override_user_config_dir(fs::path(profile), true);
  auto wait = [] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (replay_client::command(27, 0, "") && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(replay_client::command(27, 0, ""), 0);
  };
  const auto config_path = file_util::get_user_features_dir(GameVersion::Jak3) / "ghost-client.json";
  const auto original = file_util::read_text_file(config_path);
  EXPECT_EQ(replay_client::command(20, -10, ""), 0);
  wait();
  ASSERT_EQ(replay_client::command(26, 0, ""), 1);
  EXPECT_EQ(replay_client::command(23, 0, ""), 8);
  EXPECT_EQ(replay_client::command(22, 0, ""), 2);
  EXPECT_EQ(replay_client::text(12, 0), "8.900s");
  EXPECT_EQ(replay_client::text(20, 0), "1");
  EXPECT_EQ(replay_client::text(23, 0), "WR");
  EXPECT_EQ(replay_client::text(21, 6), "Long_Name_Player_Nam...");
  EXPECT_EQ(replay_client::text(21, -1), "");
  EXPECT_EQ(replay_client::text(22, 999), "");
  for (int i = 0; i < 1000; ++i) replay_client::command(20, 0, "");
  EXPECT_EQ(replay_client::command(27, 0, ""), 0);
  EXPECT_EQ(replay_client::command(20, 9999, ""), 1);
  wait();
  EXPECT_EQ(replay_client::command(23, 0, ""), 2);
  EXPECT_EQ(replay_client::command(24, 1, ""), 1);
  EXPECT_EQ(replay_client::text(14, 0), "PAGE 2 / 2");
  EXPECT_EQ(replay_client::text(21, 1), "You");
  replay_client::command(20, 0, "");
  EXPECT_EQ(replay_client::command(27, 0, ""), 0); // reuse first cached page
  EXPECT_EQ(replay_client::command(23, 0, ""), 8);
  // Third request fails, fourth returns malformed data: keep last good rows.
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5100));
    replay_client::command(21, 0, "");
    wait();
    EXPECT_EQ(replay_client::text(11, 0), "OFFLINE  /  Showing cached standings");
    EXPECT_EQ(replay_client::command(23, 0, ""), 8);
    EXPECT_EQ(replay_client::text(21, 0), "Record Holder");
    for (int i = 0; i < 100; ++i) replay_client::command(20, 0, "");
    EXPECT_EQ(replay_client::command(27, 0, ""), 0); // failure backoff
  }
  EXPECT_EQ(file_util::read_text_file(config_path), original); // no ping/upload/settings write
  ASSERT_TRUE(replay_client::set_server(replay_client::Server::SparkedHost));
  EXPECT_EQ(replay_client::command(23, 0, ""), 0);
  EXPECT_EQ(replay_client::command(26, 0, ""), 0);
  EXPECT_EQ(replay_client::text(12, 0), "--");
}

TEST(ReplayClient, ServerSelectionPersistsWithoutChangingIdentityOrRaceMode) {
  // No HTTP jobs: an empty category is intentionally invalid. Never touch the
  // real user profile, even when this test is launched without a project cwd.
  const auto profile = fs::temp_directory_path() /
                       ("opengoal-ghost-menu-test-" + std::to_string(
                           std::chrono::steady_clock::now().time_since_epoch().count()));
  file_util::override_user_config_dir(profile, true);
  const auto config_path = file_util::get_user_features_dir(GameVersion::Jak3) / "ghost-client.json";
  auto read_config = [&] { return nlohmann::json::parse(file_util::read_text_file(config_path)); };

  EXPECT_EQ(replay_client::server_status().url, replay_client::kSparkedHostServer);
  const auto initial = read_config();
  EXPECT_EQ(initial.at("mode"), 0); // two-opponent Default for new profiles
  EXPECT_EQ(initial.at("server"), replay_client::kSparkedHostServer);
  EXPECT_EQ(replay_client::text(4, 0), "Undetected player - Press L3 + D-pad Down to ping server");
  EXPECT_TRUE(replay_client::text(5, 0).empty());
  EXPECT_EQ(replay_client::command(1, 3, ""), 3);

  ASSERT_TRUE(replay_client::set_server(replay_client::Server::Localhost));
  EXPECT_EQ(replay_client::server_status().url, replay_client::kLocalhostServer);
  EXPECT_EQ(replay_client::text(4, 0), "Undetected player - Press L3 + D-pad Down to ping server");
  auto local = read_config();
  EXPECT_EQ(local.at("server"), replay_client::kLocalhostServer);
  EXPECT_EQ(local.at("mode"), 3);
  EXPECT_EQ(local.at("player_id"), initial.at("player_id"));
  EXPECT_EQ(local.at("player_token"), initial.at("player_token"));
  EXPECT_TRUE(local.at("custom_by_server").contains(replay_client::kSparkedHostServer));
  EXPECT_TRUE(replay_client::snapshot("test-mission").empty());
  EXPECT_EQ(replay_client::command(3, 0, ""), 0);

  // Simultaneous renderer reads and game-thread settings commands use one lock.
  std::thread reader([] {
    for (int i = 0; i < 100; ++i) {
      const auto url = replay_client::server_status().url;
      EXPECT_TRUE(url == replay_client::kLocalhostServer || url == replay_client::kSparkedHostServer);
      replay_client::command(0, 0, "");
    }
  });
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(replay_client::set_server(i % 2 ? replay_client::Server::Localhost :
                                                 replay_client::Server::SparkedHost));
  }
  reader.join();
  EXPECT_TRUE(replay_client::set_server(replay_client::Server::SparkedHost));
  EXPECT_FALSE(replay_client::set_server(static_cast<replay_client::Server>(99)));
  const auto final = read_config();
  EXPECT_EQ(final.at("server"), replay_client::kSparkedHostServer);
  EXPECT_EQ(final.at("player_id"), initial.at("player_id"));
  EXPECT_EQ(final.at("player_token"), initial.at("player_token"));
  EXPECT_EQ(final.at("mode"), 3);
  EXPECT_TRUE(final.at("custom_by_server").contains(replay_client::kLocalhostServer));

  // A failed atomic save must not change the active endpoint or persisted data.
  const auto temporary = fs::path(config_path.string() + ".tmp");
  fs::create_directory(temporary); // force opening the temporary file to fail
  EXPECT_FALSE(replay_client::set_server(replay_client::Server::Localhost));
  EXPECT_EQ(replay_client::server_status().url, replay_client::kSparkedHostServer);
  EXPECT_EQ(read_config(), final);
  fs::remove(temporary);
  EXPECT_TRUE(replay_client::set_server(replay_client::Server::Localhost));
  EXPECT_EQ(read_config().at("server"), replay_client::kLocalhostServer);

  file_util::override_user_config_dir({}, true);
  // Only this test's newly created profile; no jobs were queued to access it.
  fs::remove_all(profile);
}

TEST(ReplayClient, TwoFasterDefaultAndLegacySingleSelection) {
  const auto* profile = std::getenv("OG_REPLAY_TEST_PROFILE");
  const auto* expected_two = std::getenv("OG_REPLAY_EXPECTED_TWO");
  const auto* expected_single = std::getenv("OG_REPLAY_EXPECTED_SINGLE");
  if (!profile || !expected_two || !expected_single)
    GTEST_SKIP() << "Run the isolated Python client harness";
  file_util::override_user_config_dir(fs::path(profile), true);
  const auto settings = file_util::get_user_features_dir(GameVersion::Jak3) / "ghost-client.json";
  EXPECT_EQ(replay_client::command(0, 0, ""), 5); // new mode survives a fresh process
  const std::string category = "desert-bbush-get-to-19";
  for (int mode : {0, 5, 0}) {
    ASSERT_EQ(replay_client::command(1, mode, category), mode);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (replay_client::text(0, 0) == "Loading ghosts..." &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_NE(replay_client::text(0, 0), "Loading ghosts...");
    const auto ghosts = replay_client::snapshot(category);
    const auto expected = nlohmann::json::parse(mode == 0 ? expected_two : expected_single);
    ASSERT_EQ(ghosts.size(), expected.size()) << replay_client::text(0, 0);
    for (size_t i = 0; i < ghosts.size(); ++i) {
      EXPECT_FLOAT_EQ(ghosts[i].file->duration_seconds, expected.at(i).get<float>());
      EXPECT_EQ(ghosts[i].file->category, category);
      EXPECT_TRUE(ghosts[i].file->completed);
      EXPECT_FALSE(ghosts[i].label.empty());
    }
    EXPECT_EQ(nlohmann::json::parse(file_util::read_text_file(settings)).at("mode"), mode);
  }
}

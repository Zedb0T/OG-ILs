#include <chrono>
#include <thread>

#include "game/system/replay_client.h"
#include "gtest/gtest.h"
#include "third-party/json.hpp"

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

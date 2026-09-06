#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/replay/replay_format.h"

namespace replay_client {
struct Ghost {
  std::shared_ptr<const replay::File> file;
  std::string label;
};

enum class Server { SparkedHost, Localhost };
inline constexpr const char* kSparkedHostServer = "https://opengoal-ghosts.sparked.network";
inline constexpr const char* kLocalhostServer = "http://127.0.0.1:8765";

struct ServerStatus {
  std::string url;
  std::string status;
};

// Thread-safe native settings for the ImGui renderer. Selection is persisted;
// an active replay stays immutable and the new selection applies next attempt.
ServerStatus server_status();
bool set_server(Server server);

// GOAL entry points run on the game thread and share the settings mutex with
// ImGui. HTTP, parsing, and cache I/O run on a bounded worker with no GOAL pointers.
// Inventory leaderboard commands: 20 select/tick page, 21 refresh, 22 page count,
// 23 row count, 24 own-row flag, 25 rank, 26 valid page, 27 pending. Text operations
// 10-14: server/status/WR/racer count/pagination; 20-24: rank/name/time/gap/points.
int command(int operation, int value, const std::string& category);
std::string text(int operation, int index);
void prepare(const std::string& category);
std::vector<Ghost> snapshot(const std::string& category);
void completed(const replay::File& file);
}  // namespace replay_client

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
// Inventory leaderboard commands: 20 select page (negative = tick), 21 refresh, 22 page count,
// 23 row count, 24 own-row flag, 25 rank, 26 valid page, 27 pending. Text operations
// Commands 28 home, 29 view (0 home/1 points/2 catalog/3 mission), 30 move cursor,
// 31 enter, 32 back (0 = let native menu exit), 33 cursor, 34 previous/next page.
// 35 arm background warming (also armed by boot command 17), 36 warming incomplete.
// Text 10-17: server/status/summary/count/page/title/subtitle/selected preview;
// 20-24: rank/name/time/gap/points (points boards: rank/name/points/missions/WRs);
// 30-31 home choice/title description; 32-33 catalog group/racer count.
int command(int operation, int value, const std::string& category);
std::string text(int operation, int index);
void prepare(const std::string& category);
std::vector<Ghost> snapshot(const std::string& category);
void completed(const replay::File& file);
}  // namespace replay_client

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

// All entry points are called on the game thread. HTTP, parsing, and cache I/O
// run on a bounded worker with no GOAL pointers. Snapshots remain immutable.
int command(int operation, int value, const std::string& category);
std::string text(int operation, int index);
void prepare(const std::string& category);
std::vector<Ghost> snapshot(const std::string& category);
void completed(const replay::File& file);
}  // namespace replay_client

#pragma once

#include <string>
#include <vector>

namespace plume {
  // Runs the editor until its last window closes; `files` open as tabs.
  int runApplication(const std::vector<std::string>& files);
} // namespace plume

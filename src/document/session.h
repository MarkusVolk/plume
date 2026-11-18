#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace plume {
  struct SessionTab {
    // Empty for a text that was never saved.
    std::string path;
    // File in the drafts folder that holds the unsaved text, empty when the file on disk is current.
    std::string draft;
    std::size_t cursor = 0;
    std::size_t anchor = 0;
    std::size_t topLine = 0;
    bool operator==(const SessionTab&) const = default;
  };

  struct Session {
    std::vector<SessionTab> tabs;
    std::size_t active = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const Session&) const = default;
  };

  // $XDG_STATE_HOME/plume/session.toml: the tabs to bring back on the next start.
  Session loadSession();
  void saveSession(const Session& session);

  // $XDG_DATA_HOME/plume/drafts: unsaved texts, one file per tab.
  std::filesystem::path draftsDirectory();
  std::string newDraftName();
  std::optional<std::string> readDraft(const std::string& name);
  bool writeDraft(const std::string& name, const std::string& text);
  void removeDraft(const std::string& name);
  // Deletes drafts no tab of `session` refers to.
  void pruneDrafts(const Session& session);
} // namespace plume

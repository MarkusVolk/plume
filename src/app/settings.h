#pragma once

#include "ui/text_view.h"

#include <filesystem>
#include <string>
#include <vector>

namespace plume {
  struct Settings {
    TextViewOptions editor;
    bool syntaxHighlighting = true;
    bool operator==(const Settings&) const = default;
  };

  // Reads $XDG_CONFIG_HOME/plume/config.toml, then settings.toml over it. config.toml is the
  // user's own file and never written; what the window changes goes to settings.toml.
  class SettingsStore {
  public:
    SettingsStore();
    const Settings& settings() const noexcept { return m_settings; }
    // Problems found while reading, one line each.
    const std::vector<std::string>& issues() const noexcept { return m_issues; }
    void save(const Settings& settings);

  private:
    void read(const std::filesystem::path& path);
    std::filesystem::path m_directory;
    Settings m_settings;
    std::vector<std::string> m_issues;
  };
} // namespace plume

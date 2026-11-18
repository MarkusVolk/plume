#include "app/settings.h"

#include "fs/xdg_dirs.h"

#include <format>
#include <fstream>
#include <glib.h>
#include <sstream>
#include <toml++/toml.hpp>

plume::SettingsStore::SettingsStore() {
  try {
    m_directory = configHome() / "plume";
  } catch (const std::exception& error) {
    m_issues.emplace_back(error.what());
    return;
  }
  read(m_directory / "config.toml");
  read(m_directory / "settings.toml");
}

void plume::SettingsStore::read(const std::filesystem::path& path) {
  std::error_code missing;
  if (!std::filesystem::exists(path, missing)) {
    return;
  }
  toml::table table;
  try {
    table = toml::parse_file(path.string());
  } catch (const toml::parse_error& error) {
    m_issues.push_back(std::format("{}:{}: {}", path.string(), error.source().begin.line, std::string_view(error.description())));
    return;
  }
  auto& editor = m_settings.editor;
  const auto section = table["editor"];
  if (const auto value = section["font_family"].value<std::string>(); value && !value->empty()) {
    editor.fontFamily = *value;
  }
  if (const auto value = section["font_size"].value<double>()) {
    editor.fontSize = static_cast<float>(std::clamp(*value, 6.0, 72.0));
  }
  if (const auto value = section["tab_width"].value<std::int64_t>()) {
    editor.tabWidth = static_cast<int>(std::clamp<std::int64_t>(*value, 1, 32));
  }
  const auto flag = [&section](const char* key, bool& target) {
    if (const auto value = section[key].value<bool>()) {
      target = *value;
    }
  };
  flag("line_numbers", editor.lineNumbers);
  flag("wrap", editor.wrap);
  flag("highlight_current_line", editor.highlightCurrentLine);
  flag("auto_indent", editor.autoIndent);
  flag("insert_spaces", editor.insertSpaces);
  flag("syntax_highlighting", m_settings.syntaxHighlighting);
}

void plume::SettingsStore::save(const Settings& settings) {
  m_settings = settings;
  if (m_directory.empty()) {
    return;
  }
  const auto& editor = settings.editor;
  toml::table table{
      {"editor",
       toml::table{
           {"font_family", editor.fontFamily},
           {"font_size", static_cast<double>(editor.fontSize)},
           {"tab_width", editor.tabWidth},
           {"line_numbers", editor.lineNumbers},
           {"wrap", editor.wrap},
           {"highlight_current_line", editor.highlightCurrentLine},
           {"auto_indent", editor.autoIndent},
           {"insert_spaces", editor.insertSpaces},
           {"syntax_highlighting", settings.syntaxHighlighting},
       }},
  };
  std::ostringstream text;
  text << "# Written by Plume. Your own defaults belong in config.toml next to this file.\n" << table << "\n";
  std::error_code error;
  std::filesystem::create_directories(m_directory, error);
  g_file_set_contents((m_directory / "settings.toml").c_str(), text.str().c_str(), -1, nullptr);
}

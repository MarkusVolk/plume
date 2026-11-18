#include "document/session.h"

#include "fs/xdg_dirs.h"

#include <algorithm>
#include <glib.h>
#include <sstream>
#include <toml++/toml.hpp>

namespace {
  std::filesystem::path sessionPath() { return plume::stateHome() / "plume" / "session.toml"; }

  // A draft name is a file name inside the drafts folder, nothing else.
  bool plainName(const std::string& name) {
    return !name.empty() && name.find('/') == std::string::npos && name.front() != '.';
  }
} // namespace

plume::Session plume::loadSession() {
  Session session;
  toml::table table;
  try {
    table = toml::parse_file(sessionPath().string());
  } catch (const std::exception&) {
    return session;
  }
  session.active = static_cast<std::size_t>(std::max<std::int64_t>(0, table["active"].value_or<std::int64_t>(0)));
  session.width = static_cast<std::uint32_t>(std::clamp<std::int64_t>(table["window"]["width"].value_or<std::int64_t>(0), 0, 16384));
  session.height = static_cast<std::uint32_t>(std::clamp<std::int64_t>(table["window"]["height"].value_or<std::int64_t>(0), 0, 16384));
  if (const auto* tabs = table["tabs"].as_array()) {
    for (const auto& node : *tabs) {
      const auto* entry = node.as_table();
      if (!entry) {
        continue;
      }
      const auto number = [entry](const char* key) {
        return static_cast<std::size_t>(std::max<std::int64_t>(0, (*entry)[key].value_or<std::int64_t>(0)));
      };
      SessionTab tab;
      tab.path = (*entry)["path"].value_or<std::string>("");
      tab.draft = (*entry)["draft"].value_or<std::string>("");
      if (!tab.draft.empty() && !plainName(tab.draft)) {
        tab.draft.clear();
      }
      tab.cursor = number("cursor");
      tab.anchor = number("anchor");
      tab.topLine = number("top_line");
      if (!tab.path.empty() || !tab.draft.empty()) {
        session.tabs.push_back(std::move(tab));
      }
    }
  }
  return session;
}

void plume::saveSession(const Session& session) {
  toml::array tabs;
  for (const SessionTab& tab : session.tabs) {
    tabs.push_back(toml::table{
        {"path", tab.path},
        {"draft", tab.draft},
        {"cursor", static_cast<std::int64_t>(tab.cursor)},
        {"anchor", static_cast<std::int64_t>(tab.anchor)},
        {"top_line", static_cast<std::int64_t>(tab.topLine)},
    });
  }
  toml::table table{
      {"active", static_cast<std::int64_t>(session.active)},
      {"window", toml::table{{"width", session.width}, {"height", session.height}}},
      {"tabs", std::move(tabs)},
  };
  std::ostringstream text;
  text << table << "\n";
  try {
    const auto path = sessionPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    g_file_set_contents(path.c_str(), text.str().c_str(), -1, nullptr);
  } catch (const std::exception&) {
  }
}

std::filesystem::path plume::draftsDirectory() { return dataHome() / "plume" / "drafts"; }

std::string plume::newDraftName() {
  gchar* id = g_uuid_string_random();
  std::string name = std::string(id) + ".txt";
  g_free(id);
  return name;
}

std::optional<std::string> plume::readDraft(const std::string& name) {
  if (!plainName(name)) {
    return std::nullopt;
  }
  gchar* contents = nullptr;
  gsize length = 0;
  try {
    if (!g_file_get_contents((draftsDirectory() / name).c_str(), &contents, &length, nullptr)) {
      return std::nullopt;
    }
  } catch (const std::exception&) {
    return std::nullopt;
  }
  std::string text(contents, length);
  g_free(contents);
  return text;
}

bool plume::writeDraft(const std::string& name, const std::string& text) {
  if (!plainName(name)) {
    return false;
  }
  try {
    const auto directory = draftsDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    // Drafts can hold anything the user typed: readable by the user only.
    return g_file_set_contents_full(
               (directory / name).c_str(), text.data(), static_cast<gssize>(text.size()),
               G_FILE_SET_CONTENTS_CONSISTENT, 0600, nullptr
           )
        != FALSE;
  } catch (const std::exception&) {
    return false;
  }
}

void plume::removeDraft(const std::string& name) {
  if (!plainName(name)) {
    return;
  }
  try {
    std::error_code error;
    std::filesystem::remove(draftsDirectory() / name, error);
  } catch (const std::exception&) {
  }
}

void plume::pruneDrafts(const Session& session) {
  try {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(draftsDirectory(), error)) {
      const std::string name = entry.path().filename().string();
      const bool used = std::ranges::any_of(session.tabs, [&name](const SessionTab& tab) { return tab.draft == name; });
      if (!used && entry.is_regular_file(error)) {
        std::filesystem::remove(entry.path(), error);
      }
    }
  } catch (const std::exception&) {
  }
}

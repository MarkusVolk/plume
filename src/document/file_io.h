#pragma once

#include "text/document.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace plume {
  // How a file is stored on disk, kept so saving writes it back the same way.
  struct FileFormat {
    std::string encoding = "UTF-8";
    bool byteOrderMark = false;
    LineEnding lineEnding = LineEnding::Lf;
    bool operator==(const FileFormat&) const = default;
  };

  struct LoadedFile {
    // UTF-8 with '\n' line ends.
    std::string text;
    FileFormat format;
  };

  // Turns file bytes into editable text: UTF-8 and UTF-16 by their marks, anything else that is
  // not valid UTF-8 as ISO-8859-15. Files whose lines all end in CR LF are edited with '\n'.
  std::expected<LoadedFile, std::string> decodeFile(std::string bytes);
  // The bytes to write for `text` in `format`.
  std::expected<std::string, std::string> encodeFile(std::string_view text, const FileFormat& format);

  std::expected<LoadedFile, std::string> loadFile(const std::filesystem::path& path);
  // Replaces the file atomically, keeping its permissions.
  std::expected<void, std::string> saveFile(const std::filesystem::path& path, std::string_view text, const FileFormat& format);
} // namespace plume

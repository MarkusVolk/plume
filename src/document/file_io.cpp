#include "document/file_io.h"

#include "fs/gio_ptr.h"

#include <gio/gio.h>
#include <glib.h>
#include <utility>

namespace {
  std::expected<std::string, std::string> convert(std::string_view bytes, const char* to, const char* from) {
    GError* error = nullptr;
    gsize written = 0;
    gchar* out = g_convert(bytes.data(), static_cast<gssize>(bytes.size()), to, from, nullptr, &written, &error);
    if (!out) {
      std::string message = error ? error->message : "conversion failed";
      g_clear_error(&error);
      return std::unexpected(std::move(message));
    }
    std::string result(out, written);
    g_free(out);
    return result;
  }
} // namespace

std::expected<plume::LoadedFile, std::string> plume::decodeFile(std::string bytes) {
  LoadedFile file;
  const auto starts = [&bytes](std::string_view mark) { return std::string_view(bytes).starts_with(mark); };
  if (starts("\xEF\xBB\xBF")) {
    file.format.byteOrderMark = true;
    bytes.erase(0, 3);
  } else if (starts("\xFF\xFE") || starts("\xFE\xFF")) {
    file.format.encoding = starts("\xFF\xFE") ? "UTF-16LE" : "UTF-16BE";
    file.format.byteOrderMark = true;
    auto text = convert(std::string_view(bytes).substr(2), "UTF-8", file.format.encoding.c_str());
    if (!text) {
      return std::unexpected(text.error());
    }
    bytes = std::move(*text);
  }
  if (!g_utf8_validate_len(bytes.data(), static_cast<gssize>(bytes.size()), nullptr)) {
    if (bytes.find('\0') != std::string::npos) {
      return std::unexpected("this is not a text file");
    }
    auto text = convert(bytes, "UTF-8", "ISO-8859-15");
    if (!text) {
      return std::unexpected(text.error());
    }
    file.format.encoding = "ISO-8859-15";
    bytes = std::move(*text);
  }

  // Only a file that uses CR LF throughout is converted; stray carriage returns stay visible.
  std::size_t lineFeeds = 0, pairs = 0;
  for (std::size_t at = bytes.find('\n'); at != std::string::npos; at = bytes.find('\n', at + 1)) {
    ++lineFeeds;
    if (at > 0 && bytes[at - 1] == '\r') {
      ++pairs;
    }
  }
  if (pairs > 0 && pairs == lineFeeds) {
    file.format.lineEnding = LineEnding::CrLf;
    std::string text;
    text.reserve(bytes.size() - pairs);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
        continue;
      }
      text.push_back(bytes[i]);
    }
    bytes = std::move(text);
  }
  file.text = std::move(bytes);
  return file;
}

std::expected<std::string, std::string> plume::encodeFile(std::string_view text, const FileFormat& format) {
  std::string out;
  if (format.lineEnding == LineEnding::CrLf) {
    out.reserve(text.size() + text.size() / 32);
    for (const char byte : text) {
      if (byte == '\n') {
        out.push_back('\r');
      }
      out.push_back(byte);
    }
  } else {
    out.assign(text);
  }
  if (format.encoding != "UTF-8") {
    auto converted = convert(out, format.encoding.c_str(), "UTF-8");
    if (!converted) {
      return std::unexpected("the text cannot be stored as " + format.encoding + ": " + converted.error());
    }
    out = std::move(*converted);
  }
  if (format.byteOrderMark) {
    out.insert(0, format.encoding == "UTF-16LE" ? "\xFF\xFE" : format.encoding == "UTF-16BE" ? "\xFE\xFF" : "\xEF\xBB\xBF");
  }
  return out;
}

std::expected<plume::LoadedFile, std::string> plume::loadFile(const std::filesystem::path& path) {
  const GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
  gchar* contents = nullptr;
  gsize length = 0;
  GError* error = nullptr;
  if (!g_file_load_contents(file.get(), nullptr, &contents, &length, nullptr, &error)) {
    std::string message = error ? error->message : "cannot be read";
    g_clear_error(&error);
    return std::unexpected(std::move(message));
  }
  std::string bytes(contents, length);
  g_free(contents);
  return decodeFile(std::move(bytes));
}

std::expected<void, std::string>
plume::saveFile(const std::filesystem::path& path, std::string_view text, const FileFormat& format) {
  const auto bytes = encodeFile(text, format);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }
  const GioPtr<GFile> file(g_file_new_for_path(path.c_str()));
  GError* error = nullptr;
  if (!g_file_replace_contents(
          file.get(), bytes->data(), bytes->size(), nullptr, FALSE, G_FILE_CREATE_NONE, nullptr, nullptr, &error
      )) {
    std::string message = error ? error->message : "cannot be written";
    g_clear_error(&error);
    return std::unexpected(std::move(message));
  }
  return {};
}

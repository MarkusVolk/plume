#include "document/file_io.h"

#include "fs/gio_ptr.h"

#include <gio/gio.h>
#include <glib.h>
#include <utility>

namespace {
  constexpr const char* kLegacyEncoding = "WINDOWS-1252";
  // Windows-1252 differs from Latin-1 in 0x80..0x9F; the five bytes it leaves undefined keep
  // their Latin-1 control codes, so every byte decodes and encodes back to itself.
  constexpr char16_t kLegacyHigh[32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
      0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
      0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
  };

  // Done here instead of through iconv, whose module for it is often not installed.
  std::string decodeLegacy(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 8);
    for (const char byte : bytes) {
      const auto value = static_cast<unsigned char>(byte);
      const char32_t codepoint = value >= 0x80 && value < 0xA0 ? kLegacyHigh[value - 0x80] : value;
      if (codepoint < 0x80) {
        out.push_back(static_cast<char>(codepoint));
      } else if (codepoint < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
      }
    }
    return out;
  }

  std::expected<std::string, std::string> encodeLegacy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char* at = text.data(); at < text.data() + text.size(); at = g_utf8_next_char(at)) {
      const gunichar codepoint = g_utf8_get_char(at);
      if (codepoint < 0x80 || (codepoint >= 0xA0 && codepoint <= 0xFF)) {
        out.push_back(static_cast<char>(codepoint));
        continue;
      }
      bool found = false;
      for (unsigned i = 0; i < 32; ++i) {
        if (kLegacyHigh[i] == codepoint) {
          out.push_back(static_cast<char>(0x80 + i));
          found = true;
          break;
        }
      }
      if (!found) {
        gchar character[8] = {};
        g_unichar_to_utf8(codepoint, character);
        return std::unexpected(std::string("it has no “") + character + "”");
      }
    }
    return out;
  }

  // Binary data as text to look at: printable ASCII and line ends as they are, bytes above 0x7F
  // as their Windows-1252 characters, every other byte as a middle dot. A stretch without a line
  // end is broken after kBinaryLine bytes, where possible at a byte that is not text, so that
  // strings such as paths stay in one piece for searching.
  constexpr std::size_t kBinaryLine = 2048;
  std::string renderBinary(std::string_view bytes) {
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 2);
    std::size_t lineStart = 0;
    std::size_t lineBytes = 0;
    // Where the current line could be broken without cutting through text: offset in `out`.
    std::size_t lastGap = std::string::npos;
    for (const char byte : bytes) {
      const auto value = static_cast<unsigned char>(byte);
      if (value == '\n') {
        out.push_back('\n');
        lineStart = out.size();
        lineBytes = 0;
        lastGap = std::string::npos;
        continue;
      }
      if (value == '\t' || (value >= 0x20 && value < 0x7F)) {
        out.push_back(byte);
      } else if (value >= 0xA0) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
      } else {
        out.append("\xC2\xB7");
        lastGap = out.size();
      }
      if (++lineBytes >= kBinaryLine) {
        const std::size_t at = lastGap != std::string::npos && lastGap > lineStart ? lastGap : out.size();
        out.insert(at, 1, '\n');
        lineStart = at + 1;
        lineBytes = 0;
        for (std::size_t i = lineStart; i < out.size(); ++i) {
          lineBytes += (static_cast<unsigned char>(out[i]) & 0xC0) != 0x80 ? 1 : 0;
        }
        lastGap = std::string::npos;
      }
    }
    return out;
  }

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
      file.binary = true;
      file.text = renderBinary(bytes);
      return file;
    }
    file.format.encoding = kLegacyEncoding;
    bytes = decodeLegacy(bytes);
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
    auto converted = format.encoding == kLegacyEncoding ? encodeLegacy(out) : convert(out, format.encoding.c_str(), "UTF-8");
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

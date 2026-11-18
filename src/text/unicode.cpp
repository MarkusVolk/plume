#include "text/unicode.h"

#include <algorithm>
#include <glib.h>
#include <pango/pango.h>

plume::LineBreaks plume::analyzeLine(std::string_view line) {
  LineBreaks out;
  if (line.empty()) {
    out.stops.push_back(0);
    return out;
  }
  const auto length = static_cast<int>(line.size());
  const glong chars = g_utf8_strlen(line.data(), length);
  std::vector<PangoLogAttr> attrs(static_cast<std::size_t>(chars) + 1);
  pango_get_log_attrs(
      line.data(), length, -1, pango_language_get_default(), attrs.data(), static_cast<int>(attrs.size())
  );
  const char* at = line.data();
  for (std::size_t i = 0; i < attrs.size(); ++i) {
    const auto offset = static_cast<std::uint32_t>(at - line.data());
    if (attrs[i].is_cursor_position || i == 0 || i + 1 == attrs.size()) {
      out.stops.push_back(offset);
    }
    if (attrs[i].is_word_start) {
      out.wordStarts.push_back(offset);
    }
    if (attrs[i].is_word_end) {
      out.wordEnds.push_back(offset);
    }
    if (i + 1 < attrs.size()) {
      at = g_utf8_next_char(at);
    }
  }
  return out;
}

std::uint32_t plume::stopAtOrBefore(const std::vector<std::uint32_t>& stops, std::size_t offset) {
  const auto next = std::upper_bound(stops.begin(), stops.end(), offset);
  return next == stops.begin() ? 0 : *(next - 1);
}

std::uint32_t plume::stopAtOrAfter(const std::vector<std::uint32_t>& stops, std::size_t offset) {
  const auto at = std::lower_bound(stops.begin(), stops.end(), offset);
  return at == stops.end() ? stops.back() : *at;
}

std::uint32_t plume::stopBefore(const std::vector<std::uint32_t>& stops, std::size_t offset) {
  const auto at = std::lower_bound(stops.begin(), stops.end(), offset);
  return at == stops.begin() ? 0 : *(at - 1);
}

std::uint32_t plume::stopAfter(const std::vector<std::uint32_t>& stops, std::size_t offset) {
  const auto next = std::upper_bound(stops.begin(), stops.end(), offset);
  return next == stops.end() ? stops.back() : *next;
}

bool plume::isValidUtf8(std::string_view text) {
  return g_utf8_validate_len(text.data(), static_cast<gssize>(text.size()), nullptr) != FALSE;
}

std::size_t plume::utf8Length(std::string_view text) {
  return static_cast<std::size_t>(g_utf8_strlen(text.data(), static_cast<gssize>(text.size())));
}

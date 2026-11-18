#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace plume {
  // Where a caret may stand and where words begin and end in one line of UTF-8 text, as byte
  // offsets. Combining marks and emoji sequences stay one step.
  struct LineBreaks {
    std::vector<std::uint32_t> stops;
    std::vector<std::uint32_t> wordStarts;
    std::vector<std::uint32_t> wordEnds;
  };
  LineBreaks analyzeLine(std::string_view line);

  // Nearest stop at or before / at or after `offset`; a stop itself is returned unchanged.
  std::uint32_t stopAtOrBefore(const std::vector<std::uint32_t>& stops, std::size_t offset);
  std::uint32_t stopAtOrAfter(const std::vector<std::uint32_t>& stops, std::size_t offset);
  // Strictly before / after; the ends of the line clamp.
  std::uint32_t stopBefore(const std::vector<std::uint32_t>& stops, std::size_t offset);
  std::uint32_t stopAfter(const std::vector<std::uint32_t>& stops, std::size_t offset);

  // `text` with invalid UTF-8 sequences replaced by U+FFFD, so the rest stays editable.
  bool isValidUtf8(std::string_view text);
  std::size_t utf8Length(std::string_view text);
} // namespace plume

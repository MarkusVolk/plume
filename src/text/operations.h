#pragma once

#include "text/buffer.h"
#include "text/document.h"

#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace plume {
  struct SortOptions {
    bool descending = false;
    bool ignoreCase = true;
    bool unique = false;
  };
  // The lines of `text` (joined by '\n', no trailing one) in sorted order, by the locale's collation.
  std::string sortLines(std::string_view text, SortOptions options = {});

  // The whole lines a selection touches: [first, last] and their byte range without the last line's
  // newline. A selection that ends at the start of a line does not include that line.
  struct LineRange {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
  };
  LineRange selectedLines(const Buffer& buffer, Selection selection);

  struct SearchOptions {
    bool regex = false;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool operator==(const SearchOptions&) const = default;
  };
  struct Match {
    std::size_t offset = 0;
    std::size_t length = 0;
    bool operator==(const Match&) const = default;
  };

  // A compiled search pattern.
  class Pattern {
  public:
    static std::expected<Pattern, std::string> compile(std::string_view pattern, SearchOptions options);
    Pattern(Pattern&&) noexcept;
    Pattern& operator=(Pattern&&) noexcept;
    ~Pattern();

    // Every match in `text`, at most `limit`.
    std::vector<Match> findAll(std::string_view text, std::size_t limit = 100000) const;
    // What `replacement` becomes for the match at `match` in `text`: regex patterns expand \1 and
    // \g<name> references, literal patterns take the replacement as it is.
    std::string expand(std::string_view text, Match match, std::string_view replacement) const;

  private:
    struct State;
    explicit Pattern(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
  };
} // namespace plume

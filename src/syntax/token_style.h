#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace plume {
  // What a stretch of text is, as far as its color goes.
  enum class TokenStyle : std::uint8_t {
    None,
    Comment,
    Keyword,
    String,
    Escape,
    Number,
    Constant,
    Function,
    Type,
    Property,
    Operator,
    Punctuation,
    Builtin,
    Heading,
    Link,
    Emphasis,
    Added,
    Removed,
    Changed,
    Count,
  };

  // The style a highlight capture such as "keyword.control" or "string.special" stands for;
  // None for names that carry no color of their own.
  TokenStyle styleForCapture(std::string_view capture);

  // The name of a style in configuration ("keyword", "string"); empty for None.
  std::string_view styleName(TokenStyle style);

  // Who knows the styles of a text: the syntax tree of a document.
  class SyntaxSource {
  public:
    virtual ~SyntaxSource() = default;
    // One style per byte of [begin, end).
    virtual void styles(std::size_t begin, std::size_t end, std::vector<TokenStyle>& out) = 0;
    // Changes whenever the styles of any range may have.
    virtual std::uint64_t revision() const = 0;
  };
} // namespace plume

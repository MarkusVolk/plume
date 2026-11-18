#include "syntax/token_style.h"

#include <array>
#include <utility>

std::string_view plume::styleName(TokenStyle style) {
  static constexpr std::array<std::string_view, static_cast<std::size_t>(TokenStyle::Count)> kNames{
      "",         "comment",  "keyword",     "string",  "escape",  "number", "constant", "function", "type",    "property",
      "operator", "punctuation", "builtin", "heading", "link",    "emphasis", "added",   "removed",  "changed",
  };
  return style < TokenStyle::Count ? kNames[static_cast<std::size_t>(style)] : std::string_view();
}

plume::TokenStyle plume::styleForCapture(std::string_view capture) {
  // Longest prefix first: "function.builtin" before "function". Names of the tree-sitter
  // convention and those nvim-style queries use both appear in grammar repositories.
  static constexpr std::array<std::pair<std::string_view, TokenStyle>, 62> kStyles{{
      {"comment", TokenStyle::Comment},
      {"string.escape", TokenStyle::Escape},
      {"string.special.symbol", TokenStyle::Constant},
      {"string.special.key", TokenStyle::Property},
      {"string", TokenStyle::String},
      {"character.special", TokenStyle::Escape},
      {"character", TokenStyle::String},
      {"escape", TokenStyle::Escape},
      {"number", TokenStyle::Number},
      {"float", TokenStyle::Number},
      {"boolean", TokenStyle::Constant},
      {"constant", TokenStyle::Constant},
      {"keyword.operator", TokenStyle::Keyword},
      {"keyword", TokenStyle::Keyword},
      {"include", TokenStyle::Keyword},
      {"repeat", TokenStyle::Keyword},
      {"conditional", TokenStyle::Keyword},
      {"exception", TokenStyle::Keyword},
      {"storageclass", TokenStyle::Keyword},
      {"preproc", TokenStyle::Keyword},
      {"define", TokenStyle::Keyword},
      {"operator", TokenStyle::Operator},
      {"function.builtin", TokenStyle::Builtin},
      {"function", TokenStyle::Function},
      {"method", TokenStyle::Function},
      {"constructor", TokenStyle::Function},
      {"type", TokenStyle::Type},
      {"namespace", TokenStyle::Type},
      {"module", TokenStyle::Type},
      {"variable.builtin", TokenStyle::Builtin},
      {"variable.parameter", TokenStyle::None},
      {"variable", TokenStyle::None},
      {"parameter", TokenStyle::None},
      {"property", TokenStyle::Property},
      {"field", TokenStyle::Property},
      {"attribute", TokenStyle::Property},
      {"label", TokenStyle::Property},
      {"tag.attribute", TokenStyle::Property},
      {"tag", TokenStyle::Keyword},
      {"punctuation.special", TokenStyle::Operator},
      {"punctuation", TokenStyle::Punctuation},
      {"delimiter", TokenStyle::Punctuation},
      {"embedded", TokenStyle::None},
      {"text.title", TokenStyle::Heading},
      {"text.literal", TokenStyle::String},
      {"text.uri", TokenStyle::Link},
      {"text.reference", TokenStyle::Link},
      {"text.strong", TokenStyle::Emphasis},
      {"text.emphasis", TokenStyle::Emphasis},
      {"markup.heading", TokenStyle::Heading},
      {"markup.raw", TokenStyle::String},
      {"markup.link", TokenStyle::Link},
      {"markup.strong", TokenStyle::Emphasis},
      {"markup.italic", TokenStyle::Emphasis},
      {"markup.list", TokenStyle::Keyword},
      {"markup.quote", TokenStyle::Comment},
      {"diff.plus", TokenStyle::Added},
      {"diff.minus", TokenStyle::Removed},
      {"diff.delta", TokenStyle::Changed},
      {"addition", TokenStyle::Added},
      {"deletion", TokenStyle::Removed},
      {"title", TokenStyle::Heading},
  }};
  for (const auto& [prefix, style] : kStyles) {
    if (capture == prefix || (capture.starts_with(prefix) && capture[prefix.size()] == '.')) {
      return style;
    }
  }
  return TokenStyle::None;
}

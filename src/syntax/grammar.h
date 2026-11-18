#pragma once

#include "syntax/token_style.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TSLanguage;
struct TSQuery;
struct TSQueryMatch;
using GRegex = struct _GRegex;

namespace plume {
  class Buffer;

  // One condition of a query pattern, e.g. (#match? @constant "^[A-Z]+$").
  struct QueryPredicate {
    enum class Kind : std::uint8_t { Equal, Match, AnyOf };
    Kind kind = Kind::Equal;
    bool negated = false;
    std::uint32_t capture = 0;
    // Equal against another capture instead of a string.
    std::optional<std::uint32_t> otherCapture;
    std::vector<std::string> values;
    GRegex* regex = nullptr;
  };

  // A parser with its highlight query, ready to color text of one language.
  class Grammar {
  public:
    ~Grammar();
    Grammar(const Grammar&) = delete;
    Grammar& operator=(const Grammar&) = delete;

    const TSLanguage* language() const noexcept { return m_language; }
    const TSQuery* highlights() const noexcept { return m_highlights; }
    // Style of each capture of the highlight query, by capture id.
    const std::vector<TokenStyle>& captureStyles() const noexcept { return m_captureStyles; }
    // Whether the conditions of the match's pattern hold for the text in `buffer`.
    bool accepts(const TSQuery* query, const TSQueryMatch& match, const Buffer& buffer) const;

    // What other languages are embedded where: null without an injections query.
    const TSQuery* injections() const noexcept { return m_injections; }
    struct Injection {
      // Fixed language of the pattern, or empty when a capture names it.
      std::string language;
      std::optional<std::uint32_t> languageCapture;
      std::optional<std::uint32_t> contentCapture;
      bool includeChildren = false;
    };
    const std::vector<Injection>& injectionPatterns() const noexcept { return m_injectionPatterns; }

  private:
    friend class GrammarStore;
    Grammar() = default;
    const std::vector<QueryPredicate>& predicates(const TSQuery* query, std::uint32_t pattern) const;
    void readPredicates(const TSQuery* query, std::vector<std::vector<QueryPredicate>>& out);

    void* m_library = nullptr;
    const TSLanguage* m_language = nullptr;
    TSQuery* m_highlights = nullptr;
    TSQuery* m_injections = nullptr;
    std::vector<TokenStyle> m_captureStyles;
    std::vector<std::vector<QueryPredicate>> m_highlightPredicates;
    std::vector<std::vector<QueryPredicate>> m_injectionPredicates;
    std::vector<Injection> m_injectionPatterns;
  };

  // Finds parsers (<language>.so exporting tree_sitter_<language>) and their queries
  // (<language>/highlights.scm) in the folders of the system and of the user, once per language.
  class GrammarStore {
  public:
    static GrammarStore& instance();
    // Null when the parser or its highlight query is missing or does not fit this tree-sitter.
    std::shared_ptr<const Grammar> find(const std::string& language);
    // For tests: look here before anywhere else. Forgets what was found so far.
    void setSearchPaths(std::vector<std::filesystem::path> parsers, std::vector<std::filesystem::path> queries);

  private:
    GrammarStore();
    std::shared_ptr<const Grammar> load(const std::string& language);
    std::optional<std::string> query(const std::string& language, const char* name) const;

    std::vector<std::filesystem::path> m_parserPaths;
    std::vector<std::filesystem::path> m_queryPaths;
    std::map<std::string, std::shared_ptr<const Grammar>> m_grammars;
  };

  // The language of a file by its name, else by its first line ("#!/bin/sh"); empty when unknown.
  std::string detectLanguage(const std::filesystem::path& path, std::string_view firstLine);
  // The grammar name for what a fenced code block or similar calls its language ("sh", "c++").
  std::string languageForAlias(std::string_view name);
} // namespace plume

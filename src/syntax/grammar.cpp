#include "syntax/grammar.h"

#include "text/buffer.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <glib.h>
#include <sstream>
#include <tree_sitter/api.h>

namespace {
  // Queries of a language that build on another one's.
  constexpr std::array<std::pair<std::string_view, std::string_view>, 1> kInherits{{{"cpp", "c"}}};

  // A Lua pattern as the regular expression it means, for the #lua-match? of nvim-style queries.
  std::string luaPattern(std::string_view pattern) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
      const char c = pattern[i];
      if (c == '%' && i + 1 < pattern.size()) {
        const char next = pattern[++i];
        switch (next) {
        case 'a':
          out += "[A-Za-z]";
          break;
        case 'd':
          out += "\\d";
          break;
        case 'l':
          out += "[a-z]";
          break;
        case 's':
          out += "\\s";
          break;
        case 'u':
          out += "[A-Z]";
          break;
        case 'w':
          out += "[A-Za-z0-9]";
          break;
        case 'x':
          out += "[A-Fa-f0-9]";
          break;
        case 'p':
          out += "[[:punct:]]";
          break;
        default:
          out.push_back('\\');
          out.push_back(next);
          break;
        }
      } else if (c == '-' && !out.empty() && out.back() != '[' && (i + 1 == pattern.size() || pattern[i + 1] != ']')) {
        // Lua's shortest-match repetition.
        out += "*?";
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  std::string nodeText(const plume::Buffer& buffer, TSNode node) {
    const std::uint32_t start = ts_node_start_byte(node);
    return buffer.text(start, std::min<std::uint32_t>(ts_node_end_byte(node) - start, 4096));
  }

  std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(g_ascii_tolower(c)); });
    return out;
  }
} // namespace

plume::Grammar::~Grammar() {
  for (auto* predicates : {&m_highlightPredicates, &m_injectionPredicates}) {
    for (auto& pattern : *predicates) {
      for (auto& predicate : pattern) {
        if (predicate.regex) {
          g_regex_unref(predicate.regex);
        }
      }
    }
  }
  if (m_highlights) {
    ts_query_delete(m_highlights);
  }
  if (m_injections) {
    ts_query_delete(m_injections);
  }
  // The parser library stays loaded: trees and queries of it may outlive this object.
}

void plume::Grammar::readPredicates(const TSQuery* query, std::vector<std::vector<QueryPredicate>>& out) {
  const std::uint32_t patterns = ts_query_pattern_count(query);
  out.assign(patterns, {});
  for (std::uint32_t pattern = 0; pattern < patterns; ++pattern) {
    std::uint32_t count = 0;
    const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(query, pattern, &count);
    for (std::uint32_t at = 0; at < count;) {
      std::uint32_t end = at;
      while (end < count && steps[end].type != TSQueryPredicateStepTypeDone) {
        ++end;
      }
      const auto string = [&](std::uint32_t step) {
        std::uint32_t length = 0;
        const char* value = ts_query_string_value_for_id(query, steps[step].value_id, &length);
        return std::string(value, length);
      };
      // (name capture args...): anything else, such as #set!, is not a condition.
      if (end - at >= 3
          && steps[at].type == TSQueryPredicateStepTypeString
          && steps[at + 1].type == TSQueryPredicateStepTypeCapture) {
        std::string name = string(at);
        QueryPredicate predicate;
        predicate.capture = steps[at + 1].value_id;
        if (name.starts_with("not-")) {
          predicate.negated = true;
          name.erase(0, 4);
        }
        bool known = true;
        if (name == "eq?") {
          predicate.kind = QueryPredicate::Kind::Equal;
          if (steps[at + 2].type == TSQueryPredicateStepTypeCapture) {
            predicate.otherCapture = steps[at + 2].value_id;
          } else {
            predicate.values.push_back(string(at + 2));
          }
        } else if (name == "match?" || name == "lua-match?" || name == "vim-match?") {
          predicate.kind = QueryPredicate::Kind::Match;
          const std::string source = name == "lua-match?" ? luaPattern(string(at + 2)) : string(at + 2);
          predicate.regex = g_regex_new(source.c_str(), G_REGEX_OPTIMIZE, static_cast<GRegexMatchFlags>(0), nullptr);
          known = predicate.regex != nullptr;
        } else if (name == "any-of?" || name == "contains?") {
          predicate.kind = QueryPredicate::Kind::AnyOf;
          for (std::uint32_t step = at + 2; step < end; ++step) {
            if (steps[step].type == TSQueryPredicateStepTypeString) {
              predicate.values.push_back(string(step));
            }
          }
        } else {
          known = false;
        }
        if (known) {
          out[pattern].push_back(std::move(predicate));
        }
      }
      at = end + 1;
    }
  }
}

const std::vector<plume::QueryPredicate>& plume::Grammar::predicates(const TSQuery* query, std::uint32_t pattern) const {
  return (query == m_injections ? m_injectionPredicates : m_highlightPredicates)[pattern];
}

bool plume::Grammar::accepts(const TSQuery* query, const TSQueryMatch& match, const Buffer& buffer) const {
  for (const QueryPredicate& predicate : predicates(query, match.pattern_index)) {
    const auto captured = [&match](std::uint32_t capture) -> const TSNode* {
      for (std::uint16_t i = 0; i < match.capture_count; ++i) {
        if (match.captures[i].index == capture) {
          return &match.captures[i].node;
        }
      }
      return nullptr;
    };
    const TSNode* node = captured(predicate.capture);
    if (!node) {
      continue;
    }
    const std::string text = nodeText(buffer, *node);
    bool holds = false;
    switch (predicate.kind) {
    case QueryPredicate::Kind::Equal:
      if (predicate.otherCapture) {
        const TSNode* other = captured(*predicate.otherCapture);
        holds = other && nodeText(buffer, *other) == text;
      } else {
        holds = text == predicate.values.front();
      }
      break;
    case QueryPredicate::Kind::Match:
      holds = g_regex_match_full(
                  predicate.regex, text.data(), static_cast<gssize>(text.size()), 0, static_cast<GRegexMatchFlags>(0),
                  nullptr, nullptr
              )
          != FALSE;
      break;
    case QueryPredicate::Kind::AnyOf:
      holds = std::ranges::find(predicate.values, text) != predicate.values.end();
      break;
    }
    if (holds == predicate.negated) {
      return false;
    }
  }
  return true;
}

plume::GrammarStore& plume::GrammarStore::instance() {
  static GrammarStore store;
  return store;
}

plume::GrammarStore::GrammarStore() {
  const auto add = [](std::vector<std::filesystem::path>& paths, const char* value) {
    if (value && *value) {
      std::istringstream list(value);
      for (std::string entry; std::getline(list, entry, ':');) {
        if (!entry.empty()) {
          paths.emplace_back(entry);
        }
      }
    }
  };
  add(m_parserPaths, std::getenv("PLUME_PARSER_PATH"));
  add(m_queryPaths, std::getenv("PLUME_QUERY_PATH"));
  const char* home = std::getenv("HOME");
  const char* dataHome = std::getenv("XDG_DATA_HOME");
  const std::filesystem::path userData =
      dataHome && *dataHome ? std::filesystem::path(dataHome) : std::filesystem::path(home ? home : "/") / ".local/share";
  m_parserPaths.push_back(userData / "plume/parsers");
  m_queryPaths.push_back(userData / "plume/queries");
  // Where distributions put grammars; neovim's parser folder is shared when it is there.
  for (const char* prefix : {"/usr/local", "/usr"}) {
    for (const char* lib : {"lib", "lib64"}) {
      m_parserPaths.push_back(std::filesystem::path(prefix) / lib / "tree-sitter");
      m_parserPaths.push_back(std::filesystem::path(prefix) / lib / "nvim/parser");
    }
    m_queryPaths.push_back(std::filesystem::path(prefix) / "share/tree-sitter/queries");
  }
}

void plume::GrammarStore::setSearchPaths(
    std::vector<std::filesystem::path> parsers, std::vector<std::filesystem::path> queries
) {
  m_parserPaths.insert(m_parserPaths.begin(), parsers.begin(), parsers.end());
  m_queryPaths.insert(m_queryPaths.begin(), queries.begin(), queries.end());
  m_grammars.clear();
}

std::optional<std::string> plume::GrammarStore::query(const std::string& language, const char* name) const {
  for (const auto& directory : m_queryPaths) {
    std::ifstream file(directory / language / name, std::ios::binary);
    if (file) {
      std::ostringstream text;
      text << file.rdbuf();
      return text.str();
    }
  }
  return std::nullopt;
}

std::shared_ptr<const plume::Grammar> plume::GrammarStore::find(const std::string& language) {
  // A language name becomes a file and a symbol name: nothing but plain identifiers.
  if (language.empty() || !std::ranges::all_of(language, [](unsigned char c) { return g_ascii_isalnum(c) || c == '_'; })) {
    return nullptr;
  }
  if (const auto found = m_grammars.find(language); found != m_grammars.end()) {
    return found->second;
  }
  auto grammar = load(language);
  m_grammars.emplace(language, grammar);
  return grammar;
}

std::shared_ptr<const plume::Grammar> plume::GrammarStore::load(const std::string& language) {
  std::string highlights;
  for (const auto& [child, parent] : kInherits) {
    if (child == language) {
      if (auto text = query(std::string(parent), "highlights.scm")) {
        highlights += *text + "\n";
      }
    }
  }
  const auto own = query(language, "highlights.scm");
  if (!own) {
    return nullptr;
  }
  highlights += *own;

  void* library = nullptr;
  for (const auto& directory : m_parserPaths) {
    std::error_code error;
    const auto path = directory / (language + ".so");
    if (std::filesystem::exists(path, error)) {
      library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (library) {
        break;
      }
    }
  }
  if (!library) {
    return nullptr;
  }
  using Entry = const TSLanguage* (*)();
  const auto entry = reinterpret_cast<Entry>(dlsym(library, ("tree_sitter_" + language).c_str()));
  const TSLanguage* parser = entry ? entry() : nullptr;
  if (!parser
      || ts_language_abi_version(parser) > TREE_SITTER_LANGUAGE_VERSION
      || ts_language_abi_version(parser) < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION) {
    return nullptr;
  }

  std::shared_ptr<Grammar> grammar(new Grammar());
  grammar->m_library = library;
  grammar->m_language = parser;
  std::uint32_t errorOffset = 0;
  TSQueryError errorType = TSQueryErrorNone;
  grammar->m_highlights =
      ts_query_new(parser, highlights.data(), static_cast<std::uint32_t>(highlights.size()), &errorOffset, &errorType);
  if (!grammar->m_highlights) {
    g_warning("plume: the highlight query of %s does not fit its parser (offset %u)", language.c_str(), errorOffset);
    return nullptr;
  }
  const std::uint32_t captures = ts_query_capture_count(grammar->m_highlights);
  for (std::uint32_t id = 0; id < captures; ++id) {
    std::uint32_t length = 0;
    const char* name = ts_query_capture_name_for_id(grammar->m_highlights, id, &length);
    grammar->m_captureStyles.push_back(styleForCapture(std::string_view(name, length)));
  }
  grammar->readPredicates(grammar->m_highlights, grammar->m_highlightPredicates);

  if (const auto injections = query(language, "injections.scm")) {
    TSQuery* parsed =
        ts_query_new(parser, injections->data(), static_cast<std::uint32_t>(injections->size()), &errorOffset, &errorType);
    if (parsed) {
      grammar->m_injections = parsed;
      grammar->readPredicates(parsed, grammar->m_injectionPredicates);
      const std::uint32_t patterns = ts_query_pattern_count(parsed);
      grammar->m_injectionPatterns.assign(patterns, {});
      std::optional<std::uint32_t> content, named;
      for (std::uint32_t id = 0; id < ts_query_capture_count(parsed); ++id) {
        std::uint32_t length = 0;
        const char* start = ts_query_capture_name_for_id(parsed, id, &length);
        const std::string_view name(start, length);
        if (name == "injection.content" || name == "content") {
          content = id;
        } else if (name == "injection.language" || name == "language") {
          named = id;
        }
      }
      for (std::uint32_t pattern = 0; pattern < patterns; ++pattern) {
        auto& injection = grammar->m_injectionPatterns[pattern];
        injection.contentCapture = content;
        injection.languageCapture = named;
        // (#set! injection.language "bash") and (#set! injection.include-children)
        std::uint32_t count = 0;
        const TSQueryPredicateStep* steps = ts_query_predicates_for_pattern(parsed, pattern, &count);
        for (std::uint32_t at = 0; at < count;) {
          std::uint32_t end = at;
          while (end < count && steps[end].type != TSQueryPredicateStepTypeDone) {
            ++end;
          }
          const auto string = [&](std::uint32_t step) {
            std::uint32_t length = 0;
            const char* value = ts_query_string_value_for_id(parsed, steps[step].value_id, &length);
            return std::string(value, length);
          };
          if (end - at >= 2 && steps[at].type == TSQueryPredicateStepTypeString && string(at) == "set!"
              && steps[at + 1].type == TSQueryPredicateStepTypeString) {
            const std::string key = string(at + 1);
            if ((key == "injection.language" || key == "language") && end - at >= 3) {
              injection.language = languageForAlias(string(at + 2));
            } else if (key == "injection.include-children") {
              injection.includeChildren = true;
            }
          }
          at = end + 1;
        }
      }
    }
  }
  return grammar;
}

std::string plume::languageForAlias(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 16> kAliases{{
      {"sh", "bash"},
      {"shell", "bash"},
      {"zsh", "bash"},
      {"console", "bash"},
      {"c++", "cpp"},
      {"cc", "cpp"},
      {"cxx", "cpp"},
      {"h", "c"},
      {"py", "python"},
      {"python3", "python"},
      {"yml", "yaml"},
      {"rs", "rust"},
      {"md", "markdown"},
      {"patch", "diff"},
      {"makefile", "make"},
      {"bb", "bitbake"},
  }};
  const std::string lower = lowered(name);
  for (const auto& [alias, language] : kAliases) {
    if (lower == alias) {
      return std::string(language);
    }
  }
  return lower;
}

std::string plume::detectLanguage(const std::filesystem::path& path, std::string_view firstLine) {
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 44> kExtensions{{
      {".c", "c"},
      {".h", "c"},
      {".cc", "cpp"},
      {".cpp", "cpp"},
      {".cxx", "cpp"},
      {".hh", "cpp"},
      {".hpp", "cpp"},
      {".hxx", "cpp"},
      {".py", "python"},
      {".pyw", "python"},
      {".sh", "bash"},
      {".bash", "bash"},
      {".zsh", "bash"},
      {".bb", "bitbake"},
      {".bbappend", "bitbake"},
      {".bbclass", "bitbake"},
      {".inc", "bitbake"},
      {".json", "json"},
      {".toml", "toml"},
      {".yml", "yaml"},
      {".yaml", "yaml"},
      {".lua", "lua"},
      {".md", "markdown"},
      {".markdown", "markdown"},
      {".rs", "rust"},
      {".diff", "diff"},
      {".patch", "diff"},
      {".mk", "make"},
      {".cmake", "cmake"},
      {".vim", "vim"},
      {".scm", "query"},
      {".js", "javascript"},
      {".mjs", "javascript"},
      {".ts", "typescript"},
      {".go", "go"},
      {".java", "java"},
      {".css", "css"},
      {".html", "html"},
      {".htm", "html"},
      {".xml", "xml"},
      {".qml", "qmljs"},
      {".ini", "ini"},
      {".desktop", "ini"},
      {".service", "ini"},
  }};
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 14> kNames{{
      {"meson.build", "meson"},
      {"meson_options.txt", "meson"},
      {"meson.options", "meson"},
      {"Makefile", "make"},
      {"makefile", "make"},
      {"GNUmakefile", "make"},
      {"CMakeLists.txt", "cmake"},
      {".bashrc", "bash"},
      {".bash_profile", "bash"},
      {".profile", "bash"},
      {"PKGBUILD", "bash"},
      {"local.conf", "bitbake"},
      {"bblayers.conf", "bitbake"},
      {"layer.conf", "bitbake"},
  }};
  const std::string name = path.filename().string();
  for (const auto& [candidate, language] : kNames) {
    if (name == candidate) {
      return std::string(language);
    }
  }
  const std::string extension = path.extension().string();
  for (const auto& [candidate, language] : kExtensions) {
    if (extension == candidate) {
      return std::string(language);
    }
  }
  // BitBake configuration lives in conf/ folders of layers and builds.
  if (extension == ".conf" && path.parent_path().filename() == "conf") {
    return "bitbake";
  }
  if (firstLine.starts_with("#!")) {
    for (const auto& [word, language] : std::array<std::pair<std::string_view, std::string_view>, 6>{{
             {"python", "python"}, {"bash", "bash"}, {"/sh", "bash"}, {" sh", "bash"}, {"zsh", "bash"}, {"lua", "lua"}}}) {
      if (firstLine.find(word) != std::string_view::npos) {
        return std::string(language);
      }
    }
  }
  return {};
}

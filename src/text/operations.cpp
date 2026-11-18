#include "text/operations.h"

#include <algorithm>
#include <glib.h>
#include <utility>

std::string plume::sortLines(std::string_view text, SortOptions options) {
  struct Line {
    std::string_view text;
    std::string key;
  };
  std::vector<Line> lines;
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t end = std::min(text.find('\n', start), text.size());
    const std::string_view line = text.substr(start, end - start);
    gchar* folded = options.ignoreCase ? g_utf8_casefold(line.data(), static_cast<gssize>(line.size())) : nullptr;
    gchar* key = folded ? g_utf8_collate_key(folded, -1) : g_utf8_collate_key(line.data(), static_cast<gssize>(line.size()));
    lines.push_back({.text = line, .key = key});
    g_free(key);
    g_free(folded);
    start = end + 1;
  }
  std::stable_sort(lines.begin(), lines.end(), [&options](const Line& a, const Line& b) {
    return options.descending ? b.key < a.key : a.key < b.key;
  });
  if (options.unique) {
    lines.erase(
        std::unique(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.key == b.key; }),
        lines.end()
    );
  }
  std::string out;
  out.reserve(text.size());
  for (const Line& line : lines) {
    if (&line != &lines.front()) {
      out.push_back('\n');
    }
    out.append(line.text);
  }
  return out;
}

plume::LineRange plume::selectedLines(const Buffer& buffer, Selection selection) {
  LineRange range;
  range.first = buffer.lineOf(selection.begin());
  range.last = buffer.lineOf(selection.end());
  if (range.last > range.first && buffer.lineStart(range.last) == selection.end()) {
    --range.last;
  }
  range.begin = buffer.lineStart(range.first);
  range.end = buffer.lineEnd(range.last);
  return range;
}

struct plume::Pattern::State {
  GRegex* regex = nullptr;
  bool literal = true;
  ~State() {
    if (regex) {
      g_regex_unref(regex);
    }
  }
};

plume::Pattern::Pattern(std::unique_ptr<State> state) : m_state(std::move(state)) {}
plume::Pattern::Pattern(Pattern&&) noexcept = default;
plume::Pattern& plume::Pattern::operator=(Pattern&&) noexcept = default;
plume::Pattern::~Pattern() = default;

std::expected<plume::Pattern, std::string> plume::Pattern::compile(std::string_view pattern, SearchOptions options) {
  if (pattern.empty()) {
    return std::unexpected("empty pattern");
  }
  std::string source;
  if (options.regex) {
    source.assign(pattern);
  } else {
    gchar* escaped = g_regex_escape_string(pattern.data(), static_cast<gint>(pattern.size()));
    source = escaped;
    g_free(escaped);
  }
  if (options.wholeWord) {
    source = "\\b(?:" + source + ")\\b";
  }
  auto flags = static_cast<GRegexCompileFlags>(G_REGEX_MULTILINE | G_REGEX_OPTIMIZE);
  if (!options.caseSensitive) {
    flags = static_cast<GRegexCompileFlags>(flags | G_REGEX_CASELESS);
  }
  GError* error = nullptr;
  GRegex* regex = g_regex_new(source.c_str(), flags, static_cast<GRegexMatchFlags>(0), &error);
  if (!regex) {
    std::string message = error ? error->message : "invalid pattern";
    g_clear_error(&error);
    return std::unexpected(std::move(message));
  }
  auto state = std::make_unique<State>();
  state->regex = regex;
  state->literal = !options.regex;
  return Pattern(std::move(state));
}

std::vector<plume::Match> plume::Pattern::findAll(std::string_view text, std::size_t limit) const {
  std::vector<Match> out;
  GMatchInfo* info = nullptr;
  g_regex_match_full(
      m_state->regex, text.data(), static_cast<gssize>(text.size()), 0, static_cast<GRegexMatchFlags>(0), &info,
      nullptr
  );
  while (info && g_match_info_matches(info) && out.size() < limit) {
    gint start = 0, end = 0;
    if (g_match_info_fetch_pos(info, 0, &start, &end) && end > start) {
      out.push_back({.offset = static_cast<std::size_t>(start), .length = static_cast<std::size_t>(end - start)});
    }
    if (!g_match_info_next(info, nullptr)) {
      break;
    }
  }
  if (info) {
    g_match_info_free(info);
  }
  return out;
}

std::string plume::Pattern::expand(std::string_view text, Match match, std::string_view replacement) const {
  if (m_state->literal) {
    return std::string(replacement);
  }
  std::string out(replacement);
  GMatchInfo* info = nullptr;
  // Anchored at the match, so the references are those of this occurrence.
  if (g_regex_match_full(
          m_state->regex, text.data(), static_cast<gssize>(text.size()), static_cast<gint>(match.offset),
          G_REGEX_MATCH_ANCHORED, &info, nullptr
      )) {
    const std::string reference(replacement);
    if (gchar* expanded = g_match_info_expand_references(info, reference.c_str(), nullptr)) {
      out = expanded;
      g_free(expanded);
    }
  }
  if (info) {
    g_match_info_free(info);
  }
  return out;
}

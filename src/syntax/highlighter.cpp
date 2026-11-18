#include "syntax/highlighter.h"

#include <algorithm>
#include <map>
#include <tree_sitter/api.h>
#include <utility>

namespace {
  // Texts beyond this stay uncolored: parsing them would be felt.
  constexpr std::size_t kMaxSize = 8U * 1024U * 1024U;
  // Embedded stretches this close, with only blanks between, are parsed as one.
  constexpr std::size_t kMaxGap = 256;

  TSPoint point(const plume::TextPoint& value) {
    return {.row = static_cast<std::uint32_t>(value.line), .column = static_cast<std::uint32_t>(value.column)};
  }
  plume::TextPoint point(TSPoint value) { return {.line = value.row, .column = value.column}; }

  const char* read(void* payload, std::uint32_t byte, TSPoint, std::uint32_t* length) {
    const auto chunk = static_cast<const plume::Buffer*>(payload)->chunkAt(byte);
    *length = static_cast<std::uint32_t>(chunk.size());
    return chunk.data();
  }
} // namespace

std::unique_ptr<plume::Highlighter> plume::Highlighter::create(const std::string& language, const Buffer& buffer) {
  auto grammar = GrammarStore::instance().find(language);
  if (!grammar) {
    return nullptr;
  }
  return std::unique_ptr<Highlighter>(new Highlighter(std::move(grammar), buffer));
}

plume::Highlighter::Highlighter(std::shared_ptr<const Grammar> grammar, const Buffer& buffer)
    : m_buffer(buffer), m_parser(ts_parser_new()), m_cursor(ts_query_cursor_new()) {
  m_root.grammar = std::move(grammar);
}

plume::Highlighter::~Highlighter() {
  if (m_root.tree) {
    ts_tree_delete(m_root.tree);
  }
  for (const Layer& layer : m_injected) {
    if (layer.tree) {
      ts_tree_delete(layer.tree);
    }
  }
  ts_query_cursor_delete(m_cursor);
  ts_parser_delete(m_parser);
}

void plume::Highlighter::edited(const TextChange& change) {
  ++m_revision;
  m_dirty = true;
  if (change.reset) {
    if (m_root.tree) {
      ts_tree_delete(m_root.tree);
      m_root.tree = nullptr;
    }
    for (Layer& layer : m_injected) {
      if (layer.tree) {
        ts_tree_delete(layer.tree);
        layer.tree = nullptr;
      }
    }
    return;
  }
  const TSInputEdit edit{
      .start_byte = static_cast<std::uint32_t>(change.offset),
      .old_end_byte = static_cast<std::uint32_t>(change.oldEnd),
      .new_end_byte = static_cast<std::uint32_t>(change.newEnd),
      .start_point = point(change.start),
      .old_end_point = point(change.oldEndPoint),
      .new_end_point = point(change.newEndPoint),
  };
  if (m_root.tree) {
    ts_tree_edit(m_root.tree, &edit);
  }
  for (Layer& layer : m_injected) {
    if (layer.tree) {
      ts_tree_edit(layer.tree, &edit);
    }
  }
}

TSTree* plume::Highlighter::parseLayer(const Layer& layer, TSTree* old) {
  if (!ts_parser_set_language(m_parser, layer.grammar->language())) {
    return nullptr;
  }
  std::vector<TSRange> ranges;
  ranges.reserve(layer.ranges.size());
  for (const Range& range : layer.ranges) {
    ranges.push_back(
        {.start_point = point(range.beginPoint),
         .end_point = point(range.endPoint),
         .start_byte = static_cast<std::uint32_t>(range.begin),
         .end_byte = static_cast<std::uint32_t>(range.end)}
    );
  }
  // No ranges at all means the whole text again.
  if (!ts_parser_set_included_ranges(m_parser, ranges.data(), static_cast<std::uint32_t>(ranges.size()))) {
    return nullptr;
  }
  return ts_parser_parse(
      m_parser, old,
      {.payload = const_cast<Buffer*>(&m_buffer), .read = &read, .encoding = TSInputEncodingUTF8, .decode = nullptr}
  );
}

void plume::Highlighter::parse() {
  if (!m_dirty) {
    return;
  }
  m_dirty = false;
  m_tooLarge = m_buffer.size() > kMaxSize;
  if (m_tooLarge) {
    return;
  }
  TSTree* tree = parseLayer(m_root, m_root.tree);
  if (m_root.tree) {
    ts_tree_delete(m_root.tree);
  }
  m_root.tree = tree;
  findInjections();
}

void plume::Highlighter::findInjections() {
  std::map<std::string, std::vector<Range>> found;
  const Grammar& grammar = *m_root.grammar;
  if (m_root.tree && grammar.injections()) {
    ts_query_cursor_exec(m_cursor, grammar.injections(), ts_tree_root_node(m_root.tree));
    ts_query_cursor_set_byte_range(m_cursor, 0, static_cast<std::uint32_t>(m_buffer.size()));
    TSQueryMatch match;
    while (ts_query_cursor_next_match(m_cursor, &match)) {
      if (!grammar.accepts(grammar.injections(), match, m_buffer)) {
        continue;
      }
      const Grammar::Injection& injection = grammar.injectionPatterns()[match.pattern_index];
      std::string language = injection.language;
      for (std::uint16_t i = 0; i < match.capture_count && language.empty(); ++i) {
        if (injection.languageCapture && match.captures[i].index == *injection.languageCapture) {
          const TSNode node = match.captures[i].node;
          const std::uint32_t start = ts_node_start_byte(node);
          language = languageForAlias(m_buffer.text(start, std::min<std::uint32_t>(ts_node_end_byte(node) - start, 64)));
        }
      }
      if (language.empty()) {
        continue;
      }
      for (std::uint16_t i = 0; i < match.capture_count; ++i) {
        if (!injection.contentCapture || match.captures[i].index != *injection.contentCapture) {
          continue;
        }
        const TSNode node = match.captures[i].node;
        auto& ranges = found[language];
        // Without include-children the embedded text is the node minus what its named children
        // cover; punctuation tokens inside it stay, as the queries written for neovim expect.
        std::uint32_t from = ts_node_start_byte(node);
        TSPoint fromPoint = ts_node_start_point(node);
        const std::uint32_t children = injection.includeChildren ? 0 : ts_node_named_child_count(node);
        for (std::uint32_t child = 0; child < children; ++child) {
          const TSNode inner = ts_node_named_child(node, child);
          if (ts_node_start_byte(inner) > from) {
            ranges.push_back(
                {.begin = from, .end = ts_node_start_byte(inner), .beginPoint = point(fromPoint),
                 .endPoint = point(ts_node_start_point(inner))}
            );
          }
          from = ts_node_end_byte(inner);
          fromPoint = ts_node_end_point(inner);
        }
        if (ts_node_end_byte(node) > from) {
          ranges.push_back(
              {.begin = from, .end = ts_node_end_byte(node), .beginPoint = point(fromPoint),
               .endPoint = point(ts_node_end_point(node))}
          );
        }
      }
    }
  }

  std::vector<Layer> layers;
  for (auto& [language, ranges] : found) {
    auto embedded = GrammarStore::instance().find(language);
    if (!embedded || ranges.empty()) {
      continue;
    }
    std::ranges::sort(ranges, {}, &Range::begin);
    // Ranges handed to a parser must not overlap. Stretches with nothing but blanks between them
    // become one, so the embedded parser sees the line ends a grammar left out between them.
    std::vector<Range> flat;
    for (const Range& range : ranges) {
      if (flat.empty()) {
        flat.push_back(range);
        continue;
      }
      Range& last = flat.back();
      if (range.begin < last.end) {
        continue;
      }
      const std::string gap = m_buffer.text(last.end, std::min<std::size_t>(range.begin - last.end, kMaxGap + 1));
      if (gap.size() <= kMaxGap && gap.find_first_not_of(" \t\n") == std::string::npos) {
        last.end = range.end;
        last.endPoint = range.endPoint;
      } else {
        flat.push_back(range);
      }
    }
    Layer layer{.grammar = std::move(embedded), .tree = nullptr, .ranges = std::move(flat)};
    // The tree of the last round, already told about the edits, lets the parser reuse its work.
    TSTree* old = nullptr;
    for (Layer& previous : m_injected) {
      if (previous.grammar == layer.grammar) {
        old = std::exchange(previous.tree, nullptr);
        break;
      }
    }
    layer.tree = parseLayer(layer, old);
    if (old) {
      ts_tree_delete(old);
    }
    if (layer.tree) {
      layers.push_back(std::move(layer));
    }
  }
  for (const Layer& previous : m_injected) {
    if (previous.tree) {
      ts_tree_delete(previous.tree);
    }
  }
  m_injected = std::move(layers);
}

void plume::Highlighter::paint(const Layer& layer, std::size_t begin, std::size_t end, std::vector<TokenStyle>& out) {
  if (!layer.tree) {
    return;
  }
  struct Capture {
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t pattern;
    TokenStyle style;
  };
  std::vector<Capture> captures;
  const Grammar& grammar = *layer.grammar;
  ts_query_cursor_exec(m_cursor, grammar.highlights(), ts_tree_root_node(layer.tree));
  ts_query_cursor_set_byte_range(m_cursor, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end));
  TSQueryMatch match;
  while (ts_query_cursor_next_match(m_cursor, &match)) {
    if (!grammar.accepts(grammar.highlights(), match, m_buffer)) {
      continue;
    }
    for (std::uint16_t i = 0; i < match.capture_count; ++i) {
      const TokenStyle style = grammar.captureStyles()[match.captures[i].index];
      if (style == TokenStyle::None) {
        continue;
      }
      const TSNode node = match.captures[i].node;
      captures.push_back(
          {.begin = ts_node_start_byte(node), .end = ts_node_end_byte(node), .pattern = match.pattern_index, .style = style}
      );
    }
  }
  // Outer nodes first, so what lies inside them paints over; for one node the later pattern wins.
  std::ranges::sort(captures, [](const Capture& a, const Capture& b) {
    if (a.begin != b.begin) {
      return a.begin < b.begin;
    }
    if (a.end != b.end) {
      return a.end > b.end;
    }
    return a.pattern < b.pattern;
  });
  for (const Capture& capture : captures) {
    const std::size_t from = std::max<std::size_t>(capture.begin, begin);
    const std::size_t to = std::min<std::size_t>(capture.end, end);
    if (to > from) {
      std::fill(
          out.begin() + static_cast<std::ptrdiff_t>(from - begin), out.begin() + static_cast<std::ptrdiff_t>(to - begin),
          capture.style
      );
    }
  }
}

void plume::Highlighter::styles(std::size_t begin, std::size_t end, std::vector<TokenStyle>& out) {
  end = std::min(end, m_buffer.size());
  begin = std::min(begin, end);
  out.assign(end - begin, TokenStyle::None);
  parse();
  if (m_tooLarge || begin == end) {
    return;
  }
  paint(m_root, begin, end, out);
  for (const Layer& layer : m_injected) {
    // Only where the embedded text is in the range at all.
    const bool touches = std::ranges::any_of(layer.ranges, [begin, end](const Range& range) {
      return range.begin < end && range.end > begin;
    });
    if (touches) {
      paint(layer, begin, end, out);
    }
  }
}

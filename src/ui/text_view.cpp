#include "ui/text_view.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/text_clipboard.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/text_node.h"
#include "text/unicode.h"
#include "ui/controls/box.h"
#include "ui/controls/scrollbar.h"
#include "ui/style.h"
#include "wayland/text_input_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <linux/input-event-codes.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  TextClipboard* g_clipboard = nullptr;
  TextClipboard* g_primary = nullptr;
  plume::TextView::SyntaxColors g_syntaxColors;

  // Longest run of text handed to the text renderer as one node.
  constexpr std::size_t kPieceBytes = 512;
  constexpr float kTextPadding = 8.0F;
  constexpr float kGutterPadding = 10.0F;
  constexpr float kRightPadding = 14.0F;
  constexpr float kLineSpacing = 1.25F;
  constexpr float kCaretWidth = 2.0F;
  constexpr auto kBlinkInterval = std::chrono::milliseconds(530);
  constexpr auto kDragScrollInterval = std::chrono::milliseconds(30);
  constexpr std::uint32_t kMultiClickMs = 400;
  // Lines of the buffer handed to the input method around the caret.
  constexpr std::size_t kSurroundingBytes = 2000;
  // A view that shows more text than this at once (one huge line) is drawn without colors.
  constexpr std::size_t kMaxStyledBytes = 512U * 1024U;

  // The colors of the syntax styles, mixed from the palette so they follow the desktop theme.
  std::array<Color, static_cast<std::size_t>(plume::TokenStyle::Count)> styleColors() {
    using plume::TokenStyle;
    const Color text = colorForRole(ColorRole::OnSurface);
    const Color muted = colorForRole(ColorRole::OnSurfaceVariant);
    const Color surface = colorForRole(ColorRole::Surface);
    const Color primary = colorForRole(ColorRole::Primary);
    const Color secondary = colorForRole(ColorRole::Secondary);
    const Color tertiary = colorForRole(ColorRole::Tertiary);
    const Color error = colorForRole(ColorRole::Error);
    std::array<Color, static_cast<std::size_t>(TokenStyle::Count)> colors;
    colors.fill(text);
    const auto set = [&colors](TokenStyle style, const Color& color) { colors[static_cast<std::size_t>(style)] = color; };
    set(TokenStyle::Comment, lerpColor(muted, surface, 0.3F));
    set(TokenStyle::Keyword, primary);
    set(TokenStyle::String, tertiary);
    set(TokenStyle::Escape, lerpColor(tertiary, text, 0.45F));
    set(TokenStyle::Number, lerpColor(secondary, error, 0.4F));
    set(TokenStyle::Constant, lerpColor(secondary, error, 0.4F));
    set(TokenStyle::Function, secondary);
    set(TokenStyle::Type, lerpColor(primary, tertiary, 0.5F));
    set(TokenStyle::Property, lerpColor(text, secondary, 0.45F));
    set(TokenStyle::Operator, lerpColor(text, muted, 0.5F));
    set(TokenStyle::Punctuation, muted);
    set(TokenStyle::Builtin, lerpColor(primary, error, 0.45F));
    set(TokenStyle::Heading, primary);
    set(TokenStyle::Link, secondary);
    set(TokenStyle::Emphasis, tertiary);
    set(TokenStyle::Added, tertiary);
    set(TokenStyle::Removed, error);
    set(TokenStyle::Changed, secondary);
    for (std::size_t i = 0; i < colors.size(); ++i) {
      if (g_syntaxColors[i]) {
        colors[i] = *g_syntaxColors[i];
      }
    }
    return colors;
  }

  std::size_t digits(std::size_t value) {
    std::size_t count = 1;
    while (value >= 10) {
      value /= 10;
      ++count;
    }
    return count;
  }

  bool blank(char byte) { return byte == ' ' || byte == '\t'; }

  std::string utf8(std::uint32_t codepoint) {
    std::string out;
    if (codepoint < 0x80) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return out;
  }
} // namespace

std::size_t plume::TextView::LineLayout::rowOf(std::size_t stop) const {
  const auto next = std::upper_bound(rows.begin(), rows.end(), stop);
  return static_cast<std::size_t>(next - rows.begin()) - 1;
}

std::size_t plume::TextView::LineLayout::stopOf(std::size_t column) const {
  const auto next = std::upper_bound(stops.begin(), stops.end(), column);
  return next == stops.begin() ? 0 : static_cast<std::size_t>(next - stops.begin()) - 1;
}

void plume::TextView::setClipboard(TextClipboard* clipboard) noexcept { g_clipboard = clipboard; }

void plume::TextView::setPrimarySelection(TextClipboard* selection) noexcept { g_primary = selection; }

void plume::TextView::setSyntaxColors(const SyntaxColors& colors) { g_syntaxColors = colors; }

const plume::TextView::SyntaxColors& plume::TextView::syntaxColors() noexcept { return g_syntaxColors; }

plume::TextView::TextView() {
  setClipChildren(true);
  auto gutter = std::make_unique<Node>();
  gutter->setClipChildren(true);
  m_gutter = addChild(std::move(gutter));
  auto content = std::make_unique<Node>();
  content->setClipChildren(true);
  m_content = addChild(std::move(content));

  auto gutterLine = std::make_unique<Box>();
  gutterLine->setFill(colorSpecFromRole(ColorRole::OutlineVariant, 0.5F));
  m_gutterLine = static_cast<Box*>(addChild(std::move(gutterLine)));

  auto highlight = std::make_unique<Box>();
  highlight->setFill(colorSpecFromRole(ColorRole::OnSurface, 0.05F));
  highlight->setZIndex(0);
  m_lineHighlight = static_cast<Box*>(m_content->addChild(std::move(highlight)));

  auto preeditLine = std::make_unique<Box>();
  preeditLine->setFill(colorSpecFromRole(ColorRole::OnSurface));
  preeditLine->setZIndex(4);
  preeditLine->setVisible(false);
  m_preeditLine = static_cast<Box*>(m_content->addChild(std::move(preeditLine)));

  auto caret = std::make_unique<Box>();
  caret->setFill(colorSpecFromRole(ColorRole::Primary));
  caret->setZIndex(5);
  m_caret = static_cast<Box*>(m_content->addChild(std::move(caret)));

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
  area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE}));
  area->setTextInputClient(this);
  area->setOnKeyDown([this](const InputArea::KeyData& key) { handleKey(key); });
  area->setOnPress([this](const InputArea::PointerData& data) { pointerPress(data); });
  area->setOnMotion([this](const InputArea::PointerData& data) { pointerMotion(data); });
  area->setOnAxisHandler([this](const InputArea::PointerData& data) {
    if (data.axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
      if (m_options.wrap) {
        return false;
      }
      m_scrollX = std::max(0.0F, m_scrollX + data.scrollDelta(m_rowHeight * 3.0F));
      markLayoutDirty();
      return true;
    }
    scrollBy(data.scrollDelta(m_rowHeight * 3.0F));
    return true;
  });
  area->setOnFocusGain([this] { restartBlink(); });
  area->setOnFocusLoss([this] {
    m_blink.stop();
    clearPreedit();
    markLayoutDirty();
  });
  area->setZIndex(10);
  m_area = static_cast<InputArea*>(addChild(std::move(area)));

  auto vbar = std::make_unique<Scrollbar>();
  vbar->setOrientation(ScrollOrientation::Vertical);
  vbar->setOnScrollChanged([this](float offset) { scrollToOffset(offset); });
  vbar->setZIndex(20);
  m_vbar = static_cast<Scrollbar*>(addChild(std::move(vbar)));
  auto hbar = std::make_unique<Scrollbar>();
  hbar->setOrientation(ScrollOrientation::Horizontal);
  hbar->setOnScrollChanged([this](float offset) {
    m_scrollX = std::max(0.0F, offset);
    markLayoutDirty();
  });
  hbar->setZIndex(20);
  m_hbar = static_cast<Scrollbar*>(addChild(std::move(hbar)));

  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
}

plume::TextView::~TextView() {
  if (m_textInput) {
    m_textInput->clearFocusedClient(this);
  }
}

void plume::TextView::applyPalette() { markLayoutDirty(); }

void plume::TextView::setDocument(Document* document) {
  if (m_document == document) {
    return;
  }
  m_document = document;
  m_selection = {};
  m_topLine = 0;
  m_topOffset = 0.0F;
  m_scrollX = 0.0F;
  m_matches.clear();
  m_currentMatch.reset();
  clearPreedit();
  dropLayouts();
  markLayoutDirty();
}

void plume::TextView::documentReplaced() {
  if (!m_document) {
    return;
  }
  const std::size_t size = m_document->buffer().size();
  m_selection.anchor = std::min(m_selection.anchor, size);
  m_selection.cursor = std::min(m_selection.cursor, size);
  clearPreedit();
  dropLayouts();
  markLayoutDirty();
}

void plume::TextView::setOptions(const TextViewOptions& options) {
  if (m_options == options) {
    return;
  }
  m_options = options;
  m_options.tabWidth = std::clamp(m_options.tabWidth, 1, 32);
  m_options.fontSize = std::clamp(m_options.fontSize, 6.0F, 72.0F);
  if (m_options.wrap) {
    m_scrollX = 0.0F;
  }
  dropLayouts();
  m_revealCursor = true;
  markLayoutDirty();
}

void plume::TextView::dropLayouts() {
  m_layouts.clear();
  m_visible.clear();
}

void plume::TextView::setSelection(Selection selection, bool reveal) {
  if (!m_document) {
    return;
  }
  const std::size_t size = m_document->buffer().size();
  selection.anchor = std::min(selection.anchor, size);
  selection.cursor = std::min(selection.cursor, size);
  clearPreedit();
  m_selection = selection;
  m_goalX = -1.0F;
  m_document->breakRun();
  if (reveal) {
    m_revealCursor = true;
  }
  cursorMoved();
}

std::string plume::TextView::selectedText() const {
  return m_document ? m_document->buffer().text(m_selection.begin(), m_selection.length()) : std::string();
}

std::size_t plume::TextView::cursorLine() const {
  return m_document ? m_document->buffer().lineOf(m_selection.cursor) : 0;
}

std::size_t plume::TextView::cursorColumn() const {
  if (!m_document) {
    return 0;
  }
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(m_selection.cursor);
  const std::size_t start = buffer.lineStart(line);
  return utf8Length(buffer.text(start, m_selection.cursor - start));
}

// ── Layout of one line ───────────────────────────────────────────────────────

std::string plume::TextView::lineSource(std::size_t line) const {
  const Buffer& buffer = m_document->buffer();
  std::string source = buffer.lineText(line);
  if (!m_preedit.empty() && line == buffer.lineOf(m_selection.cursor)) {
    source.insert(std::min(m_selection.cursor - buffer.lineStart(line), source.size()), m_preedit);
  }
  return source;
}

std::shared_ptr<const plume::TextView::LineLayout> plume::TextView::layoutOf(std::size_t line) {
  const std::string source = lineSource(line);
  if (const auto found = m_layouts.find(source); found != m_layouts.end()) {
    return found->second;
  }
  // Lines scrolled past stay around for a while; a full cache starts over.
  if (m_layouts.size() > 4096) {
    m_layouts.clear();
  }
  auto layout = build(source);
  m_layouts.emplace(source, layout);
  return layout;
}

std::shared_ptr<const plume::TextView::LineLayout> plume::TextView::build(std::string_view source) {
  auto layout = std::make_shared<LineLayout>();
  layout->stops = analyzeLine(source).stops;
  const std::size_t count = layout->stops.size();
  const auto tab = static_cast<std::size_t>(m_options.tabWidth);

  // Tabs become spaces up to the next tab stop, control bytes their Unicode control pictures.
  std::size_t column = 0;
  layout->shown.reserve(count);
  for (std::size_t i = 0; i + 1 < count; ++i) {
    layout->shown.push_back(static_cast<std::uint32_t>(layout->display.size()));
    const std::string_view cluster = source.substr(layout->stops[i], layout->stops[i + 1] - layout->stops[i]);
    if (cluster == "\t") {
      const std::size_t fill = tab - column % tab;
      layout->display.append(fill, ' ');
      column += fill;
      continue;
    }
    for (const char byte : cluster) {
      const auto value = static_cast<unsigned char>(byte);
      if (value < 0x20 || value == 0x7F) {
        layout->display += utf8(value == 0x7F ? 0x2421U : 0x2400U + value);
      } else {
        layout->display.push_back(byte);
      }
    }
    ++column;
  }
  layout->shown.push_back(static_cast<std::uint32_t>(layout->display.size()));

  const TextFont font{.weight = FontWeight::Normal, .family = m_options.fontFamily, .features = {}};
  const auto pieceEnd = [&layout](std::size_t from, std::size_t limit) {
    std::size_t to = from;
    while (to < limit && layout->shown[to + 1] - layout->shown[from] <= kPieceBytes) {
      ++to;
    }
    return to == from ? from + 1 : to;
  };

  // Long lines are measured in pieces, so one huge line never becomes one huge layout.
  layout->x.assign(count, 0.0F);
  std::vector<std::size_t> offsets;
  std::vector<float> measured;
  for (std::size_t from = 0; from + 1 < count;) {
    const std::size_t to = pieceEnd(from, count - 1);
    offsets.clear();
    for (std::size_t i = from; i <= to; ++i) {
      offsets.push_back(layout->shown[i] - layout->shown[from]);
    }
    measured.clear();
    if (m_renderer) {
      m_renderer->measureTextCursorStops(
          std::string_view(layout->display).substr(layout->shown[from], layout->shown[to] - layout->shown[from]),
          m_options.fontSize, offsets, measured, font
      );
    }
    for (std::size_t i = from + 1; i <= to; ++i) {
      const std::size_t at = i - from;
      layout->x[i] = layout->x[from] + (at < measured.size() ? measured[at] : 0.0F);
    }
    from = to;
  }

  layout->rows.push_back(0);
  if (m_options.wrap && m_wrapWidth > 0.0F) {
    for (std::size_t start = 0;;) {
      std::size_t end = start + 1;
      while (end + 1 < count && layout->x[end + 1] - layout->x[start] <= m_wrapWidth) {
        ++end;
      }
      if (end + 1 >= count) {
        break;
      }
      // Prefer to break after whitespace; a word longer than the row breaks where it must.
      for (std::size_t at = end; at > start + 1; --at) {
        if (blank(source[layout->stops[at] - 1])) {
          end = at;
          break;
        }
      }
      layout->rows.push_back(static_cast<std::uint32_t>(end));
      start = end;
    }
  }
  for (std::size_t row = 0; row < layout->rows.size(); ++row) {
    const std::size_t rowEnd = layout->rowEnd(row);
    layout->width = std::max(layout->width, layout->x[rowEnd] - layout->x[layout->rows[row]]);
    for (std::size_t from = layout->rows[row]; from < rowEnd;) {
      layout->pieces.push_back(static_cast<std::uint32_t>(from));
      from = std::min(pieceEnd(from, rowEnd), rowEnd);
    }
  }
  return layout;
}

float plume::TextView::lineHeight(std::size_t line) {
  if (!m_options.wrap) {
    return m_rowHeight;
  }
  return static_cast<float>(layoutOf(line)->rows.size()) * m_rowHeight;
}

plume::TextView::Caret plume::TextView::caretAt(std::size_t offset) {
  const Buffer& buffer = m_document->buffer();
  Caret caret;
  caret.line = buffer.lineOf(offset);
  std::size_t column = offset - buffer.lineStart(caret.line);
  if (!m_preedit.empty() && caret.line == buffer.lineOf(m_selection.cursor)) {
    // The line is laid out with the pending composition in it.
    const std::size_t cursorColumn = m_selection.cursor - buffer.lineStart(caret.line);
    if (column > cursorColumn) {
      column += m_preedit.size();
    } else if (offset == m_selection.cursor) {
      column += m_preeditCursor;
    }
  }
  const auto layout = layoutOf(caret.line);
  caret.stop = layout->stopOf(column);
  caret.row = layout->rowOf(caret.stop);
  caret.x = layout->x[caret.stop] - layout->x[layout->rowBegin(caret.row)];
  return caret;
}

std::size_t plume::TextView::offsetAt(std::size_t line, std::size_t row, float x) {
  const auto layout = layoutOf(line);
  row = std::min(row, layout->rows.size() - 1);
  const std::size_t begin = layout->rowBegin(row);
  std::size_t end = layout->rowEnd(row);
  // The stop that ends a wrapped row is the first of the next one.
  if (row + 1 < layout->rows.size() && end > begin) {
    --end;
  }
  const float target = x + layout->x[begin];
  const auto first = layout->x.begin() + static_cast<std::ptrdiff_t>(begin);
  const auto last = layout->x.begin() + static_cast<std::ptrdiff_t>(end) + 1;
  auto at = std::lower_bound(first, last, target);
  if (at == last) {
    --at;
  } else if (at != first && target - *(at - 1) < *at - target) {
    --at;
  }
  const auto stop = static_cast<std::size_t>(at - layout->x.begin());
  return m_document->buffer().lineStart(line) + layout->stops[stop];
}

std::size_t plume::TextView::offsetAtPoint(float px, float py) {
  const Buffer& buffer = m_document->buffer();
  if (m_visible.empty()) {
    return m_selection.cursor;
  }
  const float x = px - m_textLeft + (m_options.wrap ? 0.0F : m_scrollX);
  if (py < m_visible.front().y) {
    return offsetAt(m_visible.front().line, 0, x);
  }
  for (const VisibleLine& visible : m_visible) {
    const float height = static_cast<float>(visible.layout->rows.size()) * m_rowHeight;
    if (py < visible.y + height) {
      return offsetAt(visible.line, static_cast<std::size_t>((py - visible.y) / m_rowHeight), x);
    }
  }
  const VisibleLine& last = m_visible.back();
  if (last.line + 1 >= buffer.lineCount()) {
    return buffer.size();
  }
  return offsetAt(last.line, last.layout->rows.size() - 1, x);
}

// ── Scrolling ────────────────────────────────────────────────────────────────

std::pair<std::size_t, float> plume::TextView::lastTop() {
  const std::size_t lines = m_document->buffer().lineCount();
  const float room = std::max(m_rowHeight, m_viewHeight - m_rowHeight);
  if (!m_options.wrap) {
    const float total = static_cast<float>(lines) * m_rowHeight;
    if (total <= room) {
      return {0, 0.0F};
    }
    const float top = total - room;
    const auto line = static_cast<std::size_t>(top / m_rowHeight);
    return {line, top - static_cast<float>(line) * m_rowHeight};
  }
  float above = 0.0F;
  for (std::size_t line = lines; line > 0; --line) {
    above += lineHeight(line - 1);
    if (above >= room) {
      return {line - 1, above - room};
    }
  }
  return {0, 0.0F};
}

void plume::TextView::clampScroll() {
  const auto [line, offset] = lastTop();
  if (m_topLine > line || (m_topLine == line && m_topOffset > offset)) {
    m_topLine = line;
    m_topOffset = offset;
  }
  m_topOffset = std::max(0.0F, m_topOffset);
}

void plume::TextView::scrollBy(float dy) {
  if (!m_document || dy == 0.0F) {
    return;
  }
  const std::size_t last = m_document->buffer().lineCount() - 1;
  m_topOffset += dy;
  while (m_topOffset < 0.0F && m_topLine > 0) {
    --m_topLine;
    m_topOffset += lineHeight(m_topLine);
  }
  while (m_topLine < last && m_topOffset >= lineHeight(m_topLine)) {
    m_topOffset -= lineHeight(m_topLine);
    ++m_topLine;
  }
  clampScroll();
  markLayoutDirty();
}

float plume::TextView::scrollOffset() {
  const float height = lineHeight(m_topLine);
  return (static_cast<float>(m_topLine) + (height > 0.0F ? m_topOffset / height : 0.0F)) * m_rowHeight;
}

void plume::TextView::scrollToOffset(float offset) {
  if (!m_document) {
    return;
  }
  const float rows = std::max(0.0F, offset) / m_rowHeight;
  m_topLine = std::min(static_cast<std::size_t>(rows), m_document->buffer().lineCount() - 1);
  m_topOffset = (rows - std::floor(rows)) * lineHeight(m_topLine);
  clampScroll();
  markLayoutDirty();
}

void plume::TextView::scrollToLine(std::size_t line) {
  if (!m_document) {
    return;
  }
  m_topLine = std::min(line, m_document->buffer().lineCount() - 1);
  m_topOffset = 0.0F;
  markLayoutDirty();
}

void plume::TextView::revealCursor() {
  m_revealCursor = true;
  markLayoutDirty();
}

void plume::TextView::centerCursor() {
  m_revealCursor = true;
  m_revealCentered = true;
  markLayoutDirty();
}

void plume::TextView::ensureCursorVisible() {
  const Caret caret = caretAt(m_selection.cursor);
  const float rowTop = static_cast<float>(caret.row) * m_rowHeight;
  const bool centered = std::exchange(m_revealCentered, false);
  // Where the caret's row goes when it is outside the view: `room` pixels of text stay above it.
  std::optional<float> room;
  if (caret.line < m_topLine || (caret.line == m_topLine && rowTop < m_topOffset)) {
    room = 0.0F;
  } else if (static_cast<float>(caret.line - m_topLine) > m_viewHeight / m_rowHeight + 1.0F) {
    room = std::max(0.0F, m_viewHeight - m_rowHeight);
  } else {
    float bottom = rowTop + m_rowHeight - m_topOffset;
    for (std::size_t line = m_topLine; line < caret.line; ++line) {
      bottom += lineHeight(line);
    }
    if (bottom > m_viewHeight) {
      room = std::max(0.0F, m_viewHeight - m_rowHeight);
    }
  }
  if (room) {
    if (centered) {
      room = std::floor(std::max(0.0F, m_viewHeight - m_rowHeight) * 0.5F / m_rowHeight) * m_rowHeight;
    }
    // Walk up from the caret's row until that much text is above it.
    float above = rowTop;
    std::size_t line = caret.line;
    while (above < *room && line > 0) {
      --line;
      above += lineHeight(line);
    }
    m_topLine = line;
    m_topOffset = std::max(0.0F, above - *room);
  }
  if (!m_options.wrap) {
    const float margin = std::min(m_textWidth * 0.25F, m_rowHeight * 2.0F);
    if (caret.x < m_scrollX + margin) {
      m_scrollX = std::max(0.0F, caret.x - margin);
    } else if (caret.x > m_scrollX + m_textWidth - margin) {
      m_scrollX = caret.x - m_textWidth + margin;
    }
  }
}

// ── Frame ────────────────────────────────────────────────────────────────────

Box* plume::TextView::box(std::vector<Box*>& pool, std::size_t& used, Node* parent, int z) {
  if (used == pool.size()) {
    auto fresh = std::make_unique<Box>();
    fresh->setZIndex(z);
    pool.push_back(static_cast<Box*>(parent->addChild(std::move(fresh))));
  }
  Box* node = pool[used++];
  node->setVisible(true);
  return node;
}

TextNode* plume::TextView::textNode(std::vector<TextNode*>& pool, std::size_t& used, Node* parent) {
  if (used == pool.size()) {
    auto fresh = std::make_unique<TextNode>();
    fresh->setZIndex(3);
    fresh->setEllipsize(TextEllipsize::End);
    pool.push_back(static_cast<TextNode*>(parent->addChild(std::move(fresh))));
  }
  TextNode* node = pool[used++];
  node->setVisible(true);
  return node;
}

void plume::TextView::doLayout(Renderer& renderer) {
  m_renderer = &renderer;
  const float viewWidth = width();
  const float viewHeight = height();
  const bool hasDocument = m_document != nullptr && viewWidth > 0.0F && viewHeight > 0.0F;
  m_content->setVisible(hasDocument);
  m_gutter->setVisible(hasDocument);
  if (!hasDocument) {
    return;
  }
  const Buffer& buffer = m_document->buffer();
  const TextFont font{.weight = FontWeight::Normal, .family = m_options.fontFamily, .features = {}};
  const TextMetrics fontMetrics = renderer.measureFont(m_options.fontSize, font);
  const float extent = fontMetrics.bottom - fontMetrics.top;
  const float rowHeight = std::ceil(extent * kLineSpacing);
  const float digitWidth = renderer.measureText("0", m_options.fontSize, font).width;
  const float gutterWidth = m_options.lineNumbers
      ? std::ceil(kGutterPadding * 2.0F + static_cast<float>(std::max<std::size_t>(3, digits(buffer.lineCount()))) * digitWidth)
      : 0.0F;
  const float textWidth = std::max(digitWidth, viewWidth - gutterWidth - kTextPadding - kRightPadding);
  const float wrapWidth = m_options.wrap ? textWidth : 0.0F;
  if (wrapWidth != m_wrapWidth
      || rowHeight != m_rowHeight
      || renderer.renderScale() != m_renderScale
      || renderer.textMetricsGeneration() != m_metricsGeneration) {
    dropLayouts();
  }
  m_wrapWidth = wrapWidth;
  m_rowHeight = rowHeight;
  m_baseline = std::round((rowHeight - extent) * 0.5F - fontMetrics.top);
  m_renderScale = renderer.renderScale();
  m_metricsGeneration = renderer.textMetricsGeneration();
  m_gutterWidth = gutterWidth;
  m_textLeft = gutterWidth + kTextPadding;
  m_textWidth = textWidth;
  m_viewHeight = viewHeight;

  m_topLine = std::min(m_topLine, buffer.lineCount() - 1);
  if (m_revealCursor) {
    ensureCursorVisible();
    m_revealCursor = false;
  }
  clampScroll();

  m_visible.clear();
  float y = -m_topOffset;
  for (std::size_t line = m_topLine; line < buffer.lineCount() && y < viewHeight; ++line) {
    auto layout = layoutOf(line);
    const float height = static_cast<float>(layout->rows.size()) * rowHeight;
    m_visible.push_back({.line = line, .y = y, .layout = std::move(layout)});
    y += height;
  }

  m_gutter->setPosition(0.0F, 0.0F);
  m_gutter->setSize(gutterWidth, viewHeight);
  m_content->setPosition(gutterWidth, 0.0F);
  m_content->setSize(viewWidth - gutterWidth, viewHeight);
  m_area->setPosition(0.0F, 0.0F);
  m_area->setFrameSize(viewWidth, viewHeight);
  m_gutterLine->setVisible(m_options.lineNumbers);
  m_gutterLine->setPosition(gutterWidth - 1.0F, 0.0F);
  m_gutterLine->setSize(1.0F, viewHeight);

  const float scrollX = m_options.wrap ? 0.0F : m_scrollX;
  const Color textColor = colorForRole(ColorRole::OnSurface);
  const Color numberColor = colorForRole(ColorRole::OnSurfaceVariant, 0.7F);
  const std::size_t cursorLine = buffer.lineOf(m_selection.cursor);
  const bool focused = m_area->focused();

  std::size_t usedText = 0, usedNumbers = 0, usedSelection = 0, usedMatches = 0;
  m_maxRowWidth = 0.0F;

  // The styles of everything in view, asked for again only when the text or the view moved.
  bool styled = false;
  if (m_syntax && !m_visible.empty()) {
    const std::size_t first = buffer.lineStart(m_visible.front().line);
    const std::size_t last = buffer.lineEnd(m_visible.back().line);
    if (last - first <= kMaxStyledBytes) {
      if (m_stylesRevision != m_syntax->revision() || first != m_stylesBegin || last != m_stylesEnd) {
        m_syntax->styles(first, last, m_styles);
        m_stylesBegin = first;
        m_stylesEnd = last;
        m_stylesRevision = m_syntax->revision();
      }
      styled = m_styles.size() == last - first;
    }
  }
  const auto colors = styleColors();

  // A marked range of the buffer as boxes, one per row it touches. The rows form one shape:
  // only its outer corners are round, not the ones where two rows meet.
  struct MarkedRow {
    float left = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
  };
  std::vector<MarkedRow> marked;
  const auto mark = [&](std::size_t begin, std::size_t end, std::vector<Box*>& pool, std::size_t& used,
                        const ColorSpec& fill, int z, bool toLineEnd) {
    marked.clear();
    for (const VisibleLine& visible : m_visible) {
      const std::size_t lineStart = buffer.lineStart(visible.line);
      const std::size_t lineEnd = lineStart + buffer.lineLength(visible.line);
      if (end <= lineStart || begin > lineEnd) {
        continue;
      }
      const LineLayout& layout = *visible.layout;
      const std::size_t from = layout.stopOf(std::max(begin, lineStart) - lineStart);
      const std::size_t to = layout.stopOf(std::min(end, lineEnd) - lineStart);
      const bool newline = toLineEnd && end > lineEnd;
      for (std::size_t row = layout.rowOf(from); row < layout.rows.size(); ++row) {
        const std::size_t rowBegin = layout.rowBegin(row), rowEnd = layout.rowEnd(row);
        if (rowBegin > to) {
          break;
        }
        const std::size_t a = std::max(from, rowBegin), b = std::min(to, rowEnd);
        float left = layout.x[a] - layout.x[rowBegin];
        float right = layout.x[b] - layout.x[rowBegin];
        // The newline a selection includes shows as a stub past the last character.
        if (newline && row + 1 == layout.rows.size()) {
          right += digitWidth;
        }
        if (right <= left) {
          continue;
        }
        marked.push_back(
            {.left = kTextPadding + left - scrollX, .right = kTextPadding + right - scrollX,
             .top = visible.y + static_cast<float>(row) * rowHeight}
        );
      }
    }
    static constexpr float kRadius = 4.0F;
    // A selection is one shape: a short row below the first one (an empty line, say) reaches over
    // to where the first one begins instead of floating apart from it.
    if (toLineEnd) {
      for (std::size_t i = 1; i < marked.size(); ++i) {
        if (marked[i - 1].top + rowHeight == marked[i].top && marked[i].right < marked[i - 1].left) {
          marked[i].right = marked[i - 1].left;
        }
      }
    }
    for (std::size_t i = 0; i < marked.size(); ++i) {
      const MarkedRow& row = marked[i];
      const MarkedRow* above = i > 0 && marked[i - 1].top + rowHeight == row.top ? &marked[i - 1] : nullptr;
      const MarkedRow* below = i + 1 < marked.size() && row.top + rowHeight == marked[i + 1].top ? &marked[i + 1] : nullptr;
      // A corner no neighbouring row touches is round. Where the neighbour ends flush it is
      // square, and where the neighbour reaches further out the edge sweeps outwards into it.
      struct Corner {
        float radius = 0.0F;
        bool outwards = false;
      };
      const auto corner = [&row](const MarkedRow* other, bool leftSide) -> Corner {
        // Rows that only touch at an edge still join; a gap between them leaves both round.
        if (!other || other->left > row.right + 0.5F || other->right < row.left - 0.5F) {
          return {.radius = kRadius, .outwards = false};
        }
        const float beyond = leftSide ? row.left - other->left : other->right - row.right;
        if (std::abs(beyond) <= 0.5F) {
          return {};
        }
        return {.radius = std::min(kRadius, std::abs(beyond)), .outwards = beyond > 0.0F};
      };
      const Corner topLeft = corner(above, true), topRight = corner(above, false);
      const Corner bottomRight = corner(below, false), bottomLeft = corner(below, true);
      // The sweep is drawn in a margin beside the row's own rectangle.
      const float insetLeft = topLeft.outwards || bottomLeft.outwards ? kRadius : 0.0F;
      const float insetRight = topRight.outwards || bottomRight.outwards ? kRadius : 0.0F;
      const auto shape = [](const Corner& value) { return value.outwards ? CornerShape::Concave : CornerShape::Convex; };
      Box* node = box(pool, used, m_content, z);
      node->setFill(fill);
      node->setRadii(Radii(topLeft.radius, topRight.radius, bottomRight.radius, bottomLeft.radius));
      node->setCornerShapes({.tl = shape(topLeft), .tr = shape(topRight), .br = shape(bottomRight), .bl = shape(bottomLeft)});
      node->setLogicalInset({.left = insetLeft, .top = 0.0F, .right = insetRight, .bottom = 0.0F});
      node->setPosition(row.left - insetLeft, row.top);
      node->setSize(row.right - row.left + insetLeft + insetRight, rowHeight);
    }
  };

  for (const VisibleLine& visible : m_visible) {
    const LineLayout& layout = *visible.layout;
    m_maxRowWidth = std::max(m_maxRowWidth, layout.width);
    if (m_options.lineNumbers) {
      TextNode* number = textNode(m_numberNodes, usedNumbers, m_gutter);
      const std::string label = std::to_string(visible.line + 1);
      number->setFontFamily(m_options.fontFamily);
      number->setFontSize(m_options.fontSize);
      number->setText(label);
      number->setColor(visible.line == cursorLine ? textColor : numberColor);
      number->setPosition(
          gutterWidth - kGutterPadding - static_cast<float>(label.size()) * digitWidth, visible.y + m_baseline
      );
    }
    for (std::size_t piece = 0; piece < layout.pieces.size(); ++piece) {
      const std::size_t from = layout.pieces[piece];
      const std::size_t row = layout.rowOf(from);
      const std::size_t to =
          std::min<std::size_t>(piece + 1 < layout.pieces.size() ? layout.pieces[piece + 1] : layout.stops.size() - 1, layout.rowEnd(row));
      const float left = layout.x[from] - layout.x[layout.rowBegin(row)] - scrollX;
      const float right = layout.x[to] - layout.x[layout.rowBegin(row)] - scrollX;
      const float top = visible.y + static_cast<float>(row) * rowHeight;
      if (right < -kTextPadding || left > textWidth + kRightPadding || top + rowHeight < 0.0F || top > viewHeight) {
        continue;
      }
      // One node per stretch of one color; blanks join the stretch before them.
      const std::size_t lineStart = buffer.lineStart(visible.line);
      const bool colored = styled && !(visible.line == cursorLine && !m_preedit.empty());
      const auto styleAt = [&](std::size_t stop) {
        const std::size_t offset = lineStart + layout.stops[stop];
        return colored && offset >= m_stylesBegin && offset - m_stylesBegin < m_styles.size()
            ? m_styles[offset - m_stylesBegin]
            : TokenStyle::None;
      };
      for (std::size_t run = from; run < to;) {
        const TokenStyle style = styleAt(run);
        std::size_t end = run + 1;
        while (end < to && (layout.display[layout.shown[end]] == ' ' || styleAt(end) == style)) {
          ++end;
        }
        TextNode* text = textNode(m_textNodes, usedText, m_content);
        text->setFontFamily(m_options.fontFamily);
        text->setFontSize(m_options.fontSize);
        text->setText(layout.display.substr(layout.shown[run], layout.shown[end] - layout.shown[run]));
        text->setColor(colored ? colors[static_cast<std::size_t>(style)] : textColor);
        text->setPosition(
            kTextPadding + layout.x[run] - layout.x[layout.rowBegin(row)] - scrollX, top + m_baseline
        );
        run = end;
      }
    }
  }

  if (!m_matches.empty() && !m_visible.empty()) {
    const std::size_t firstOffset = buffer.lineStart(m_visible.front().line);
    const std::size_t lastOffset = buffer.lineEnd(m_visible.back().line);
    auto at = std::lower_bound(m_matches.begin(), m_matches.end(), firstOffset, [](const Match& match, std::size_t offset) {
      return match.offset + match.length < offset;
    });
    for (; at != m_matches.end() && at->offset <= lastOffset; ++at) {
      const bool current = m_currentMatch && static_cast<std::size_t>(at - m_matches.begin()) == *m_currentMatch;
      mark(
          at->offset, at->offset + at->length, m_matchBoxes, usedMatches,
          colorSpecFromRole(current ? ColorRole::Primary : ColorRole::Secondary, current ? 0.5F : 0.25F), 1, false
      );
    }
  }
  if (!m_selection.empty()) {
    mark(
        m_selection.begin(), m_selection.end(), m_selectionBoxes, usedSelection,
        colorSpecFromRole(ColorRole::Primary, focused ? 0.35F : 0.2F), 2, true
    );
  }

  for (std::size_t i = usedText; i < m_textNodes.size(); ++i) {
    m_textNodes[i]->setVisible(false);
  }
  for (std::size_t i = usedNumbers; i < m_numberNodes.size(); ++i) {
    m_numberNodes[i]->setVisible(false);
  }
  for (std::size_t i = usedSelection; i < m_selectionBoxes.size(); ++i) {
    m_selectionBoxes[i]->setVisible(false);
  }
  for (std::size_t i = usedMatches; i < m_matchBoxes.size(); ++i) {
    m_matchBoxes[i]->setVisible(false);
  }

  // Caret, the line it is on and the composition underline.
  bool caretVisible = false;
  m_lineHighlight->setVisible(false);
  m_preeditLine->setVisible(false);
  for (const VisibleLine& visible : m_visible) {
    if (visible.line != cursorLine) {
      continue;
    }
    const Caret caret = caretAt(m_selection.cursor);
    const float top = visible.y + static_cast<float>(caret.row) * rowHeight;
    m_caret->setPosition(std::round(kTextPadding + caret.x - scrollX) - 1.0F, top + 1.0F);
    m_caret->setSize(kCaretWidth, rowHeight - 2.0F);
    caretVisible = true;
    if (m_options.highlightCurrentLine && m_selection.empty()) {
      m_lineHighlight->setVisible(true);
      m_lineHighlight->setPosition(0.0F, visible.y);
      m_lineHighlight->setSize(viewWidth - gutterWidth, static_cast<float>(visible.layout->rows.size()) * rowHeight);
    }
    if (!m_preedit.empty()) {
      const LineLayout& layout = *visible.layout;
      const std::size_t column = m_selection.cursor - buffer.lineStart(cursorLine);
      const std::size_t from = layout.stopOf(column), to = layout.stopOf(column + m_preedit.size());
      const std::size_t row = layout.rowOf(from);
      const float left = layout.x[from] - layout.x[layout.rowBegin(row)];
      const float right = layout.x[std::min(to, layout.rowEnd(row))] - layout.x[layout.rowBegin(row)];
      m_preeditLine->setVisible(right > left);
      m_preeditLine->setPosition(
          kTextPadding + left - scrollX, visible.y + static_cast<float>(row + 1) * rowHeight - 2.0F
      );
      m_preeditLine->setSize(right - left, 1.0F);
    }
    break;
  }
  m_caret->setVisible(caretVisible && focused && m_caretOn);

  const auto [lastLine, lastOffset] = lastTop();
  const float lastHeight = lineHeight(lastLine);
  const float maxScroll =
      (static_cast<float>(lastLine) + (lastHeight > 0.0F ? lastOffset / lastHeight : 0.0F)) * rowHeight;
  m_vbar->setPosition(viewWidth - m_vbar->reservedThickness() - 3.0F, 0.0F);
  m_vbar->setSize(m_vbar->reservedThickness(), viewHeight);
  m_vbar->update(viewHeight, maxScroll + viewHeight, scrollOffset());
  const float contentWidth = std::max(m_maxRowWidth + kRightPadding, m_scrollX + textWidth);
  const bool horizontal = !m_options.wrap && contentWidth > textWidth + 1.0F;
  m_hbar->setVisible(horizontal);
  if (horizontal) {
    m_hbar->setPosition(m_textLeft, viewHeight - m_hbar->reservedThickness() - 3.0F);
    m_hbar->setSize(textWidth, m_hbar->reservedThickness());
    m_hbar->update(textWidth, contentWidth, m_scrollX);
  }
}

// ── Editing ──────────────────────────────────────────────────────────────────

void plume::TextView::changed() {
  m_revealCursor = true;
  restartBlink();
  markLayoutDirty();
  if (m_onChange) {
    m_onChange();
  }
  cursorMoved();
}

void plume::TextView::publishSelection() {
  if (g_primary && m_document && !m_selection.empty()) {
    g_primary->setClipboardText(selectedText());
  }
}

void plume::TextView::pastePrimary(std::size_t offset) {
  if (m_readOnly || !g_primary || !m_document) {
    return;
  }
  g_primary->requestClipboardText([this, offset, alive = std::weak_ptr<int>(m_alive)](std::optional<std::string> text) {
    if (alive.expired() || !text || text->empty() || !m_document || !isValidUtf8(*text)) {
      return;
    }
    const std::size_t at = std::min(offset, m_document->buffer().size());
    m_selection = Selection::at(at);
    edit(at, 0, *text, at + text->size(), EditKind::Discrete);
  });
}

void plume::TextView::cursorMoved() {
  // A drag offers its text once, when the button is let go.
  if (!m_dragging) {
    publishSelection();
  }
  restartBlink();
  markLayoutDirty();
  notifyInputMethod(TextInputChangeCause::Other);
  if (m_onCursorMoved) {
    m_onCursorMoved();
  }
}

void plume::TextView::restartBlink() {
  m_caretOn = true;
  m_blink.startRepeating(kBlinkInterval, [this] {
    m_caretOn = !m_caretOn;
    markLayoutDirty();
  });
}

void plume::TextView::edit(
    std::size_t offset, std::size_t length, std::string_view text, std::size_t cursorAfter, EditKind kind
) {
  if (m_readOnly || !m_document) {
    return;
  }
  clearPreedit();
  m_document->edit(offset, length, text, m_selection, Selection::at(cursorAfter), kind);
  m_selection = Selection::at(cursorAfter);
  m_goalX = -1.0F;
  changed();
}

void plume::TextView::replaceRange(std::size_t offset, std::size_t length, std::string_view text) {
  edit(offset, length, text, offset + text.size(), EditKind::Discrete);
}

void plume::TextView::insertText(std::string_view text) {
  if (!m_document || text.empty()) {
    return;
  }
  const bool typing = m_selection.empty() && text.find('\n') == std::string_view::npos;
  edit(
      m_selection.begin(), m_selection.length(), text, m_selection.begin() + text.size(),
      typing ? EditKind::Typing : EditKind::Discrete
  );
}

void plume::TextView::deleteSelectionOr(std::size_t from, std::size_t to, EditKind kind) {
  if (!m_selection.empty()) {
    edit(m_selection.begin(), m_selection.length(), {}, m_selection.begin(), EditKind::Discrete);
  } else if (to > from) {
    edit(from, to - from, {}, from, kind);
  }
}

void plume::TextView::newLine() {
  const Buffer& buffer = m_document->buffer();
  std::string text = "\n";
  if (m_options.autoIndent) {
    const std::size_t line = buffer.lineOf(m_selection.begin());
    const std::size_t start = buffer.lineStart(line);
    const std::string head = buffer.text(start, m_selection.begin() - start);
    const std::size_t indent = std::min(head.find_first_not_of(" \t"), head.size());
    text.append(head, 0, indent);
  }
  edit(m_selection.begin(), m_selection.length(), text, m_selection.begin() + text.size(), EditKind::Discrete);
}

void plume::TextView::undo() {
  if (m_readOnly || !m_document) {
    return;
  }
  clearPreedit();
  if (const auto selection = m_document->undo()) {
    m_selection = *selection;
    m_goalX = -1.0F;
    changed();
  }
}

void plume::TextView::redo() {
  if (m_readOnly || !m_document) {
    return;
  }
  clearPreedit();
  if (const auto selection = m_document->redo()) {
    m_selection = *selection;
    m_goalX = -1.0F;
    changed();
  }
}

void plume::TextView::copy() {
  if (g_clipboard && m_document && !m_selection.empty()) {
    g_clipboard->setClipboardText(selectedText());
  }
}

void plume::TextView::cut() {
  if (m_readOnly) {
    copy();
    return;
  }
  if (g_clipboard && m_document && !m_selection.empty()) {
    g_clipboard->setClipboardText(selectedText());
    deleteSelectionOr(0, 0, EditKind::Discrete);
  }
}

void plume::TextView::paste() {
  if (m_readOnly || !g_clipboard || !m_document) {
    return;
  }
  g_clipboard->requestClipboardText([this, alive = std::weak_ptr<int>(m_alive)](std::optional<std::string> text) {
    if (alive.expired() || !text || text->empty() || !m_document) {
      return;
    }
    // Pasted text uses the buffer's line ends; whatever cannot be UTF-8 is left out.
    std::string clean;
    clean.reserve(text->size());
    for (std::size_t i = 0; i < text->size(); ++i) {
      if ((*text)[i] == '\r') {
        if (i + 1 >= text->size() || (*text)[i + 1] != '\n') {
          clean.push_back('\n');
        }
      } else if ((*text)[i] != '\0') {
        clean.push_back((*text)[i]);
      }
    }
    if (!isValidUtf8(clean)) {
      return;
    }
    edit(m_selection.begin(), m_selection.length(), clean, m_selection.begin() + clean.size(), EditKind::Discrete);
  });
}

void plume::TextView::selectAll() {
  if (m_document) {
    setSelection({.anchor = 0, .cursor = m_document->buffer().size()}, false);
  }
}

void plume::TextView::sortLines(SortOptions options) {
  if (m_readOnly || !m_document) {
    return;
  }
  const Buffer& buffer = m_document->buffer();
  LineRange range;
  if (m_selection.empty()) {
    range = {.first = 0, .last = buffer.lineCount() - 1, .begin = 0, .end = buffer.size()};
    // A final newline stays where it is instead of sorting an empty line to the top.
    if (range.end > 0 && buffer.byteAt(range.end - 1) == '\n') {
      --range.end;
    }
  } else {
    range = selectedLines(buffer, m_selection);
  }
  const std::string before = buffer.text(range.begin, range.end - range.begin);
  const std::string sorted = plume::sortLines(before, options);
  if (sorted == before) {
    return;
  }
  clearPreedit();
  const Selection after = m_selection.empty()
      ? Selection::at(std::min(m_selection.cursor, range.begin + sorted.size()))
      : Selection{.anchor = range.begin, .cursor = range.begin + sorted.size()};
  m_document->edit(range.begin, before.size(), sorted, m_selection, after);
  m_selection = after;
  m_goalX = -1.0F;
  changed();
}

void plume::TextView::indentLines(bool outdent) {
  if (m_readOnly || !m_document) {
    return;
  }
  const Buffer& buffer = m_document->buffer();
  const LineRange range = selectedLines(buffer, m_selection);
  const std::string unit = m_options.insertSpaces ? std::string(static_cast<std::size_t>(m_options.tabWidth), ' ') : "\t";
  clearPreedit();
  Selection after = m_selection;
  const auto shift = [](std::size_t& offset, std::size_t at, std::ptrdiff_t delta) {
    if (offset > at || (delta > 0 && offset == at)) {
      offset = delta < 0 ? std::max(at, offset - static_cast<std::size_t>(-delta)) : offset + static_cast<std::size_t>(delta);
    }
  };
  m_document->beginGroup(m_selection);
  for (std::size_t line = range.first; line <= range.last; ++line) {
    const std::size_t start = buffer.lineStart(line);
    if (!outdent) {
      if (buffer.lineLength(line) == 0) {
        continue;
      }
      m_document->replace(start, 0, unit);
      shift(after.anchor, start, static_cast<std::ptrdiff_t>(unit.size()));
      shift(after.cursor, start, static_cast<std::ptrdiff_t>(unit.size()));
      continue;
    }
    std::size_t remove = 0;
    if (buffer.byteAt(start) == '\t') {
      remove = 1;
    } else {
      while (remove < static_cast<std::size_t>(m_options.tabWidth) && buffer.byteAt(start + remove) == ' ') {
        ++remove;
      }
    }
    if (remove > 0) {
      m_document->replace(start, remove, {});
      shift(after.anchor, start, -static_cast<std::ptrdiff_t>(remove));
      shift(after.cursor, start, -static_cast<std::ptrdiff_t>(remove));
    }
  }
  m_document->endGroup(after);
  m_selection = after;
  m_goalX = -1.0F;
  changed();
}

void plume::TextView::replaceMatches(const std::vector<Match>& matches, const std::vector<std::string>& texts) {
  if (m_readOnly || !m_document || matches.empty() || matches.size() != texts.size()) {
    return;
  }
  clearPreedit();
  m_document->beginGroup(m_selection);
  std::size_t cursor = m_selection.cursor;
  for (std::size_t i = matches.size(); i > 0; --i) {
    const Match& match = matches[i - 1];
    m_document->replace(match.offset, match.length, texts[i - 1]);
    // The caret keeps its place in the text around the replacements.
    if (cursor >= match.offset + match.length) {
      cursor = cursor - match.length + texts[i - 1].size();
    } else if (cursor > match.offset) {
      cursor = match.offset + texts[i - 1].size();
    }
  }
  const Selection after = Selection::at(std::min(cursor, m_document->buffer().size()));
  m_document->endGroup(after);
  m_selection = after;
  m_goalX = -1.0F;
  changed();
}

void plume::TextView::setSyntax(SyntaxSource* syntax) {
  if (m_syntax == syntax) {
    return;
  }
  m_syntax = syntax;
  m_styles.clear();
  m_stylesRevision = 0;
  markLayoutDirty();
}

void plume::TextView::setMatches(std::vector<Match> matches, std::optional<std::size_t> current) {
  m_matches = std::move(matches);
  m_currentMatch = current;
  markLayoutDirty();
}

// ── Movement ─────────────────────────────────────────────────────────────────

std::size_t plume::TextView::stopBeforeOffset(std::size_t offset) const {
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(offset);
  const std::size_t start = buffer.lineStart(line);
  if (offset == start) {
    return offset == 0 ? 0 : offset - 1;
  }
  return start + stopBefore(analyzeLine(buffer.lineText(line)).stops, offset - start);
}

std::size_t plume::TextView::stopAfterOffset(std::size_t offset) const {
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(offset);
  const std::size_t start = buffer.lineStart(line);
  if (offset == buffer.lineEnd(line)) {
    return std::min(offset + 1, buffer.size());
  }
  return start + stopAfter(analyzeLine(buffer.lineText(line)).stops, offset - start);
}

std::size_t plume::TextView::wordBefore(std::size_t offset) const {
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(offset);
  const std::size_t start = buffer.lineStart(line);
  if (offset == start) {
    return offset == 0 ? 0 : offset - 1;
  }
  const auto breaks = analyzeLine(buffer.lineText(line));
  const auto at = std::lower_bound(breaks.wordStarts.begin(), breaks.wordStarts.end(), offset - start);
  return at == breaks.wordStarts.begin() ? start : start + *(at - 1);
}

std::size_t plume::TextView::wordAfter(std::size_t offset) const {
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(offset);
  const std::size_t start = buffer.lineStart(line);
  const std::size_t end = buffer.lineEnd(line);
  if (offset == end) {
    return std::min(offset + 1, buffer.size());
  }
  const auto breaks = analyzeLine(buffer.lineText(line));
  const auto at = std::upper_bound(breaks.wordEnds.begin(), breaks.wordEnds.end(), offset - start);
  return at == breaks.wordEnds.end() ? end : start + *at;
}

plume::Selection plume::TextView::wordAt(std::size_t offset) const {
  const Buffer& buffer = m_document->buffer();
  const std::size_t line = buffer.lineOf(offset);
  const std::size_t start = buffer.lineStart(line);
  const std::string text = buffer.lineText(line);
  const auto breaks = analyzeLine(text);
  const std::size_t column = offset - start;
  // Inside a word: its bounds. Between words: the run of whatever is under the pointer.
  const auto begin = std::upper_bound(breaks.wordStarts.begin(), breaks.wordStarts.end(), column);
  const auto finish = std::upper_bound(breaks.wordEnds.begin(), breaks.wordEnds.end(), column);
  if (begin != breaks.wordStarts.begin() && finish != breaks.wordEnds.end() && *(begin - 1) < *finish) {
    return {.anchor = start + *(begin - 1), .cursor = start + *finish};
  }
  const std::size_t left = begin != breaks.wordStarts.begin() && finish != breaks.wordEnds.begin()
      ? *(finish - 1)
      : (finish != breaks.wordEnds.begin() ? *(finish - 1) : 0);
  const std::size_t right = begin != breaks.wordStarts.end() ? *begin : text.size();
  return {.anchor = start + std::min(left, column), .cursor = start + std::max(right, column)};
}

void plume::TextView::moveTo(std::size_t offset, bool extend, bool keepGoal) {
  clearPreedit();
  m_selection.cursor = std::min(offset, m_document->buffer().size());
  if (!extend) {
    m_selection.anchor = m_selection.cursor;
  }
  if (!keepGoal) {
    m_goalX = -1.0F;
  }
  m_document->breakRun();
  m_revealCursor = true;
  cursorMoved();
}

void plume::TextView::moveVertically(int rows, bool extend) {
  const Buffer& buffer = m_document->buffer();
  Caret caret = caretAt(m_selection.cursor);
  if (m_goalX < 0.0F) {
    m_goalX = caret.x;
  }
  std::size_t line = caret.line;
  std::size_t row = caret.row;
  for (int left = std::abs(rows); left > 0; --left) {
    if (rows < 0) {
      if (row > 0) {
        --row;
      } else if (line > 0) {
        --line;
        row = layoutOf(line)->rows.size() - 1;
      } else {
        moveTo(0, extend, true);
        return;
      }
    } else if (row + 1 < layoutOf(line)->rows.size()) {
      ++row;
    } else if (line + 1 < buffer.lineCount()) {
      ++line;
      row = 0;
    } else {
      moveTo(buffer.size(), extend, true);
      return;
    }
  }
  moveTo(offsetAt(line, row, m_goalX), extend, true);
}

void plume::TextView::handleKey(const InputArea::KeyData& key) {
  if (!m_document) {
    return;
  }
  const Buffer& buffer = m_document->buffer();
  const bool shift = (key.modifiers & KeyMod::Shift) != 0;
  const bool ctrl = (key.modifiers & KeyMod::Ctrl) != 0;
  const bool alt = (key.modifiers & KeyMod::Alt) != 0;
  const bool super = (key.modifiers & KeyMod::Super) != 0;
  const std::uint32_t sym = key.sym;

  // A dead key shows what it would compose until the next key settles it.
  if (key.preedit) {
    if (m_readOnly) {
      return;
    }
    if (!m_selection.empty()) {
      deleteSelectionOr(0, 0, EditKind::Discrete);
    }
    m_preedit = key.utf32 >= 0x20 ? utf8(key.utf32) : std::string();
    m_preeditCursor = m_preedit.size();
    m_revealCursor = true;
    markLayoutDirty();
    return;
  }
  if (m_onKey && m_onKey(key)) {
    return;
  }
  clearPreedit();

  const std::size_t cursor = m_selection.cursor;
  const std::size_t line = buffer.lineOf(cursor);

  if (ctrl && !alt && !super) {
    switch (sym) {
    case XKB_KEY_a:
    case XKB_KEY_A:
      selectAll();
      return;
    case XKB_KEY_c:
    case XKB_KEY_C:
    case XKB_KEY_Insert:
      copy();
      return;
    case XKB_KEY_x:
    case XKB_KEY_X:
      cut();
      return;
    case XKB_KEY_v:
    case XKB_KEY_V:
      paste();
      return;
    case XKB_KEY_z:
    case XKB_KEY_Z:
      shift ? redo() : undo();
      return;
    case XKB_KEY_y:
    case XKB_KEY_Y:
      redo();
      return;
    default:
      break;
    }
  }
  if (shift && !ctrl && !alt && KeySymbol::isInsert(sym)) {
    paste();
    return;
  }
  if (shift && !ctrl && !alt && KeySymbol::isDelete(sym) && !m_selection.empty()) {
    cut();
    return;
  }
  if (alt || super) {
    return;
  }

  if (KeySymbol::isLeft(sym)) {
    if (!shift && !ctrl && !m_selection.empty()) {
      moveTo(m_selection.begin(), false);
    } else {
      moveTo(ctrl ? wordBefore(cursor) : stopBeforeOffset(cursor), shift);
    }
  } else if (KeySymbol::isRight(sym)) {
    if (!shift && !ctrl && !m_selection.empty()) {
      moveTo(m_selection.end(), false);
    } else {
      moveTo(ctrl ? wordAfter(cursor) : stopAfterOffset(cursor), shift);
    }
  } else if (KeySymbol::isUp(sym)) {
    if (ctrl) {
      scrollBy(-m_rowHeight);
    } else {
      moveVertically(-1, shift);
    }
  } else if (KeySymbol::isDown(sym)) {
    if (ctrl) {
      scrollBy(m_rowHeight);
    } else {
      moveVertically(1, shift);
    }
  } else if (KeySymbol::isPageUp(sym) || KeySymbol::isPageDown(sym)) {
    const int page = std::max(1, static_cast<int>(m_viewHeight / m_rowHeight) - 1);
    moveVertically(KeySymbol::isPageUp(sym) ? -page : page, shift);
  } else if (KeySymbol::isHome(sym)) {
    if (ctrl) {
      moveTo(0, shift);
    } else {
      // Home goes to the first character of the row, then to its very start.
      const Caret caret = caretAt(cursor);
      const auto layout = layoutOf(line);
      const std::size_t lineStart = buffer.lineStart(line);
      const std::size_t rowStart = lineStart + layout->stops[layout->rowBegin(caret.row)];
      std::size_t text = rowStart;
      if (caret.row == 0) {
        while (text < buffer.lineEnd(line) && blank(buffer.byteAt(text))) {
          ++text;
        }
      }
      moveTo(cursor == text ? rowStart : text, shift);
    }
  } else if (KeySymbol::isEnd(sym)) {
    if (ctrl) {
      moveTo(buffer.size(), shift);
    } else {
      const Caret caret = caretAt(cursor);
      const auto layout = layoutOf(line);
      std::size_t stop = layout->rowEnd(caret.row);
      if (caret.row + 1 < layout->rows.size() && stop > layout->rowBegin(caret.row)) {
        --stop;
      }
      moveTo(buffer.lineStart(line) + layout->stops[stop], shift);
    }
  } else if (KeySymbol::isBackspace(sym)) {
    deleteSelectionOr(ctrl ? wordBefore(cursor) : stopBeforeOffset(cursor), cursor, EditKind::Backspace);
  } else if (KeySymbol::isDelete(sym)) {
    deleteSelectionOr(cursor, ctrl ? wordAfter(cursor) : stopAfterOffset(cursor), EditKind::Delete);
  } else if (KeySymbol::isEnter(sym)) {
    newLine();
  } else if (KeySymbol::isTab(sym) || sym == XKB_KEY_ISO_Left_Tab) {
    const bool lines = !m_selection.empty() && buffer.lineOf(m_selection.begin()) != buffer.lineOf(m_selection.end());
    if (shift || sym == XKB_KEY_ISO_Left_Tab) {
      indentLines(true);
    } else if (lines) {
      indentLines(false);
    } else if (m_options.insertSpaces) {
      const std::size_t column = cursorColumn();
      const auto width = static_cast<std::size_t>(m_options.tabWidth);
      insertText(std::string(width - column % width, ' '));
    } else {
      insertText("\t");
    }
  } else if (!ctrl && key.utf32 >= 0x20 && key.utf32 != 0x7F) {
    insertText(utf8(key.utf32));
  }
}

// ── Pointer ──────────────────────────────────────────────────────────────────

void plume::TextView::pointerPress(const InputArea::PointerData& data) {
  if (!m_document) {
    return;
  }
  if (!data.pressed) {
    if (data.button == BTN_LEFT && m_dragging) {
      m_dragging = false;
      m_dragScroll.stop();
      publishSelection();
    }
    return;
  }
  clearPreedit();
  const std::size_t offset = offsetAtPoint(data.localX, data.localY);
  if (data.button == BTN_MIDDLE) {
    pastePrimary(offset);
    return;
  }
  if (data.button == BTN_RIGHT) {
    if (offset < m_selection.begin() || offset > m_selection.end() || m_selection.empty()) {
      moveTo(offset, false);
    }
    if (m_onContextMenu) {
      m_onContextMenu(data.sceneX, data.sceneY);
    }
    return;
  }
  if (data.button != BTN_LEFT) {
    return;
  }
  const bool near = std::abs(data.localX - m_lastClickX) < 4.0F && std::abs(data.localY - m_lastClickY) < 4.0F;
  m_clickCount = near && data.time - m_lastClickTime < kMultiClickMs ? m_clickCount % 3 + 1 : 1;
  m_lastClickTime = data.time;
  m_lastClickX = data.localX;
  m_lastClickY = data.localY;
  m_dragging = true;
  m_dragX = data.localX;
  m_dragY = data.localY;
  m_goalX = -1.0F;
  const Buffer& buffer = m_document->buffer();
  if (m_clickCount == 2) {
    m_dragGranularity = Granularity::Word;
    m_dragOrigin = wordAt(offset);
    m_selection = m_dragOrigin;
  } else if (m_clickCount == 3) {
    m_dragGranularity = Granularity::Line;
    const std::size_t line = buffer.lineOf(offset);
    m_dragOrigin = {.anchor = buffer.lineStart(line), .cursor = std::min(buffer.lineEnd(line) + 1, buffer.size())};
    m_selection = m_dragOrigin;
  } else {
    m_dragGranularity = Granularity::Character;
    const bool extend = (m_modifiers & KeyMod::Shift) != 0;
    m_selection.cursor = offset;
    if (!extend) {
      m_selection.anchor = offset;
    }
    m_dragOrigin = Selection::at(m_selection.anchor);
  }
  m_document->breakRun();
  cursorMoved();
}

void plume::TextView::pointerMotion(const InputArea::PointerData& data) {
  if (!m_dragging || !m_document) {
    return;
  }
  m_dragX = data.localX;
  m_dragY = data.localY;
  const auto extend = [this] {
    const Buffer& buffer = m_document->buffer();
    const std::size_t offset = offsetAtPoint(m_dragX, std::clamp(m_dragY, 0.0F, m_viewHeight - 1.0F));
    Selection target = Selection::at(offset);
    if (m_dragGranularity == Granularity::Word) {
      target = wordAt(offset);
    } else if (m_dragGranularity == Granularity::Line) {
      const std::size_t line = buffer.lineOf(offset);
      target = {.anchor = buffer.lineStart(line), .cursor = std::min(buffer.lineEnd(line) + 1, buffer.size())};
    }
    if (offset < m_dragOrigin.begin()) {
      m_selection = {.anchor = m_dragOrigin.end(), .cursor = target.begin()};
    } else {
      m_selection = {.anchor = m_dragOrigin.begin(), .cursor = std::max(target.end(), m_dragOrigin.end())};
    }
    if (m_dragGranularity == Granularity::Character) {
      m_selection = {.anchor = m_dragOrigin.anchor, .cursor = offset};
    }
    cursorMoved();
  };
  extend();
  // Past the top or bottom edge the view keeps scrolling while the button is held.
  if (m_dragY < 0.0F || m_dragY > m_viewHeight) {
    if (!m_dragScroll.active()) {
      m_dragScroll.startRepeating(kDragScrollInterval, [this, extend] {
        if (!m_dragging || (m_dragY >= 0.0F && m_dragY <= m_viewHeight)) {
          m_dragScroll.stop();
          return;
        }
        const float distance = m_dragY < 0.0F ? m_dragY : m_dragY - m_viewHeight;
        scrollBy(std::clamp(distance, -m_rowHeight * 4.0F, m_rowHeight * 4.0F));
        extend();
      });
    }
  } else {
    m_dragScroll.stop();
  }
}

// ── Input method ─────────────────────────────────────────────────────────────

void plume::TextView::clearPreedit() {
  if (m_preedit.empty()) {
    return;
  }
  m_preedit.clear();
  m_preeditCursor = 0;
  markLayoutDirty();
}

void plume::TextView::notifyInputMethod(TextInputChangeCause cause) {
  if (m_textInput) {
    m_textInput->notifyClientStateChanged(this, cause);
  }
}

TextInputState plume::TextView::textInputState() const {
  TextInputState state;
  if (!m_document) {
    return state;
  }
  const Buffer& buffer = m_document->buffer();
  // The input method sees the text around the caret, cut at character boundaries.
  std::size_t begin = m_selection.begin() > kSurroundingBytes ? m_selection.begin() - kSurroundingBytes : 0;
  std::size_t end = std::min(buffer.size(), m_selection.end() + kSurroundingBytes);
  while (begin > 0 && (static_cast<unsigned char>(buffer.byteAt(begin)) & 0xC0) == 0x80) {
    --begin;
  }
  while (end < buffer.size() && (static_cast<unsigned char>(buffer.byteAt(end)) & 0xC0) == 0x80) {
    ++end;
  }
  state.surroundingText = buffer.text(begin, end - begin);
  state.cursor = static_cast<std::int32_t>(m_selection.cursor - begin);
  state.anchor = static_cast<std::int32_t>(m_selection.anchor - begin);
  float sceneX = 0.0F, sceneY = 0.0F;
  Node::absolutePosition(m_caret, sceneX, sceneY);
  state.cursorRectX = static_cast<std::int32_t>(std::round(sceneX));
  state.cursorRectY = static_cast<std::int32_t>(std::round(sceneY));
  state.cursorRectWidth = 2;
  state.cursorRectHeight = static_cast<std::int32_t>(std::max(1.0F, m_rowHeight));
  return state;
}

void plume::TextView::textInputApplyEdit(const TextInputEdit& edit) {
  if (m_readOnly || !m_document) {
    return;
  }
  const Buffer& buffer = m_document->buffer();
  clearPreedit();
  if (edit.hasDelete) {
    const std::size_t from = m_selection.cursor - std::min<std::size_t>(edit.deleteBeforeLength, m_selection.cursor);
    const std::size_t to = std::min(buffer.size(), m_selection.cursor + edit.deleteAfterLength);
    if (to > from) {
      this->edit(from, to - from, {}, from, EditKind::Discrete);
    }
  }
  if (edit.hasCommitText && (!edit.commitText.empty() || !m_selection.empty())) {
    if (edit.commitText.empty()) {
      deleteSelectionOr(0, 0, EditKind::Discrete);
    } else {
      insertText(edit.commitText);
    }
  }
  if (edit.hasPreedit && !edit.preeditText.empty()) {
    if (!m_selection.empty()) {
      deleteSelectionOr(0, 0, EditKind::Discrete);
    }
    m_preedit = edit.preeditText;
    m_preeditCursor = edit.preeditCursorBegin < 0
        ? m_preedit.size()
        : std::min(static_cast<std::size_t>(edit.preeditCursorBegin), m_preedit.size());
    m_revealCursor = true;
    markLayoutDirty();
  }
  if (edit.submit) {
    newLine();
  }
}

void plume::TextView::textInputResetPreedit() { clearPreedit(); }

void plume::TextView::textInputActivated(TextInputService& service) { m_textInput = &service; }

void plume::TextView::textInputDeactivated(TextInputService& service) {
  if (m_textInput == &service) {
    clearPreedit();
    m_textInput = nullptr;
  }
}

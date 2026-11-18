#pragma once

#include "core/timer_manager.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "syntax/token_style.h"
#include "text/document.h"
#include "text/operations.h"
#include "ui/palette.h"
#include "ui/signal.h"
#include "ui/text_input_client.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Box;
class Scrollbar;
class TextClipboard;
class TextNode;

namespace plume {
  struct TextViewOptions {
    std::string fontFamily = "monospace";
    float fontSize = 13.0F;
    bool lineNumbers = true;
    bool wrap = true;
    bool highlightCurrentLine = true;
    bool autoIndent = true;
    bool insertSpaces = false;
    int tabWidth = 8;
    bool operator==(const TextViewOptions&) const = default;
  };

  // The editing surface for one document: draws only the lines in view, with an optional gutter of
  // line numbers, and turns keys, pointer and input-method events into edits of the document.
  class TextView final : public Node, public TextInputClient {
  public:
    TextView();
    ~TextView() override;

    // Not owned; outlives the view or is replaced before it goes.
    void setDocument(Document* document);
    Document* document() const noexcept { return m_document; }

    void setOptions(const TextViewOptions& options);
    const TextViewOptions& options() const noexcept { return m_options; }

    // A read-only view moves, selects and copies but changes nothing.
    void setReadOnly(bool readOnly) noexcept { m_readOnly = readOnly; }
    bool readOnly() const noexcept { return m_readOnly; }

    Selection selection() const noexcept { return m_selection; }
    void setSelection(Selection selection, bool reveal = true);
    std::string selectedText() const;
    // 0-based line and the column counted in caret stops.
    std::size_t cursorLine() const;
    std::size_t cursorColumn() const;

    // Scroll position as a line and the pixels of it above the view, for saving and restoring.
    std::size_t topLine() const noexcept { return m_topLine; }
    void scrollToLine(std::size_t line);
    void revealCursor();
    // Like revealCursor(), but a caret outside the view lands in its middle instead of at an edge.
    void centerCursor();

    void insertText(std::string_view text);
    void undo();
    void redo();
    void copy();
    void cut();
    void paste();
    void selectAll();
    // Sorts the selected lines, or every line without a selection.
    void sortLines(SortOptions options = {});
    void indentLines(bool outdent);
    // Replaces [offset, offset+length) as one undo step and leaves the caret after it.
    void replaceRange(std::size_t offset, std::size_t length, std::string_view text);

    // Replaces every match with its text, last to first, as one undo step; both lists are parallel.
    void replaceMatches(const std::vector<Match>& matches, const std::vector<std::string>& texts);
    // The keyboard modifiers held now: Shift extends the selection on click.
    void setModifiers(std::uint32_t modifiers) noexcept { m_modifiers = modifiers; }

    // Where the colors of the text come from; null for plain text. Not owned.
    void setSyntax(SyntaxSource* syntax);

    // Search hits to mark; `current` gets the stronger color.
    void setMatches(std::vector<Match> matches, std::optional<std::size_t> current);

    void setOnChange(std::function<void()> callback) { m_onChange = std::move(callback); }
    void setOnCursorMoved(std::function<void()> callback) { m_onCursorMoved = std::move(callback); }
    // Keys the view does not use itself; true when the callback took the key.
    void setOnKey(std::function<bool(const InputArea::KeyData&)> callback) { m_onKey = std::move(callback); }
    void setOnContextMenu(std::function<void(float, float)> callback) { m_onContextMenu = std::move(callback); }

    InputArea* inputArea() const noexcept { return m_area; }
    static void setClipboard(TextClipboard* clipboard) noexcept;
    // Colors of the syntax styles that the theme sets itself; the others are mixed from the palette.
    using SyntaxColors = std::array<std::optional<Color>, static_cast<std::size_t>(TokenStyle::Count)>;
    static void setSyntaxColors(const SyntaxColors& colors);
    static const SyntaxColors& syntaxColors() noexcept;

    // The document changed behind the view's back (reload, undo by the owner): drop layouts, clamp the selection.
    void documentReplaced();

    TextInputState textInputState() const override;
    void textInputApplyEdit(const TextInputEdit& edit) override;
    void textInputResetPreedit() override;
    void textInputActivated(TextInputService& service) override;
    void textInputDeactivated(TextInputService& service) override;

  private:
    // One line as it is drawn: the text with tabs expanded, where a caret may stand in it and how
    // it breaks into rows.
    struct LineLayout {
      std::string display;
      // Caret stops as byte offsets into the line, the same stops as offsets into `display`, and
      // their x on one unbroken row.
      std::vector<std::uint32_t> stops;
      std::vector<std::uint32_t> shown;
      std::vector<float> x;
      // First stop of each row, then of each piece drawn as one text node (rows split further when long).
      std::vector<std::uint32_t> rows;
      std::vector<std::uint32_t> pieces;
      float width = 0.0F;

      std::size_t rowOf(std::size_t stop) const;
      std::size_t rowBegin(std::size_t row) const { return rows[row]; }
      std::size_t rowEnd(std::size_t row) const { return row + 1 < rows.size() ? rows[row + 1] : stops.size() - 1; }
      std::size_t stopOf(std::size_t column) const;
    };
    struct VisibleLine {
      std::size_t line = 0;
      float y = 0.0F;
      std::shared_ptr<const LineLayout> layout;
    };
    struct Caret {
      std::size_t line = 0;
      std::size_t row = 0;
      std::size_t stop = 0;
      float x = 0.0F;
    };
    enum class Granularity { Character, Word, Line };

    void doLayout(Renderer& renderer) override;
    // The parent decides the size; measuring does not lay the text out.
    LayoutSize doMeasure(Renderer&, const LayoutConstraints& constraints) override {
      return constraints.constrain({.width = width(), .height = height()});
    }
    void doRebindRenderer(Renderer& renderer) override { m_renderer = &renderer; }

    std::string lineSource(std::size_t line) const;
    std::shared_ptr<const LineLayout> layoutOf(std::size_t line);
    std::shared_ptr<const LineLayout> build(std::string_view source);
    void dropLayouts();
    float lineHeight(std::size_t line);
    Caret caretAt(std::size_t offset);
    std::size_t offsetAt(std::size_t line, std::size_t row, float x);
    std::size_t offsetAtPoint(float px, float py);

    void scrollBy(float dy);
    void scrollToOffset(float offset);
    float scrollOffset();
    void clampScroll();
    // The last scroll position: the top line and the pixels of it above the view.
    std::pair<std::size_t, float> lastTop();
    void ensureCursorVisible();

    void handleKey(const InputArea::KeyData& key);
    void moveTo(std::size_t offset, bool extend, bool keepGoal = false);
    void moveVertically(int rows, bool extend);
    std::size_t wordBefore(std::size_t offset) const;
    std::size_t wordAfter(std::size_t offset) const;
    std::size_t stopBeforeOffset(std::size_t offset) const;
    std::size_t stopAfterOffset(std::size_t offset) const;
    Selection wordAt(std::size_t offset) const;
    void edit(std::size_t offset, std::size_t length, std::string_view text, std::size_t cursorAfter, EditKind kind);
    void deleteSelectionOr(std::size_t from, std::size_t to, EditKind kind);
    void newLine();
    void pointerPress(const InputArea::PointerData& data);
    void pointerMotion(const InputArea::PointerData& data);
    void changed();
    void cursorMoved();
    void restartBlink();
    void applyPalette();
    void clearPreedit();
    void notifyInputMethod(TextInputChangeCause cause);

    Box* box(std::vector<Box*>& pool, std::size_t& used, Node* parent, int z);
    TextNode* textNode(std::vector<TextNode*>& pool, std::size_t& used, Node* parent);

    Document* m_document = nullptr;
    TextViewOptions m_options;
    Selection m_selection;
    float m_goalX = -1.0F;
    std::uint32_t m_modifiers = 0;
    std::size_t m_topLine = 0;
    float m_topOffset = 0.0F;
    float m_scrollX = 0.0F;
    bool m_revealCursor = false;
    bool m_readOnly = false;
    bool m_revealCentered = false;

    // Layouts by line text, valid for the metrics below.
    std::unordered_map<std::string, std::shared_ptr<const LineLayout>> m_layouts;
    Renderer* m_renderer = nullptr;
    float m_wrapWidth = 0.0F;
    float m_rowHeight = 18.0F;
    float m_baseline = 13.0F;
    float m_renderScale = 0.0F;
    std::uint64_t m_metricsGeneration = 0;
    float m_gutterWidth = 0.0F;
    float m_textLeft = 0.0F;
    float m_textWidth = 0.0F;
    float m_viewHeight = 0.0F;
    float m_maxRowWidth = 0.0F;
    std::vector<VisibleLine> m_visible;

    SyntaxSource* m_syntax = nullptr;
    // Styles of the bytes [m_stylesBegin, m_stylesEnd), as of m_stylesRevision.
    std::vector<TokenStyle> m_styles;
    std::size_t m_stylesBegin = 0;
    std::size_t m_stylesEnd = 0;
    std::uint64_t m_stylesRevision = 0;

    std::vector<Match> m_matches;
    std::optional<std::size_t> m_currentMatch;

    std::string m_preedit;
    std::size_t m_preeditCursor = 0;
    TextInputService* m_textInput = nullptr;

    Node* m_gutter = nullptr;
    Node* m_content = nullptr;
    Box* m_gutterLine = nullptr;
    Box* m_lineHighlight = nullptr;
    Box* m_caret = nullptr;
    Box* m_preeditLine = nullptr;
    InputArea* m_area = nullptr;
    Scrollbar* m_vbar = nullptr;
    Scrollbar* m_hbar = nullptr;
    std::vector<Box*> m_selectionBoxes;
    std::vector<Box*> m_matchBoxes;
    std::vector<TextNode*> m_textNodes;
    std::vector<TextNode*> m_numberNodes;

    Timer m_blink;
    Timer m_dragScroll;
    bool m_caretOn = true;
    bool m_dragging = false;
    Granularity m_dragGranularity = Granularity::Character;
    Selection m_dragOrigin;
    float m_dragX = 0.0F;
    float m_dragY = 0.0F;
    std::uint32_t m_lastClickTime = 0;
    int m_clickCount = 0;
    float m_lastClickX = 0.0F;
    float m_lastClickY = 0.0F;

    std::function<void()> m_onChange;
    std::function<void()> m_onCursorMoved;
    std::function<bool(const InputArea::KeyData&)> m_onKey;
    std::function<void(float, float)> m_onContextMenu;
    Signal<>::ScopedConnection m_paletteConn;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace plume

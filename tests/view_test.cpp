#include "check.h"
#include "core/application_identity.h"
#include "core/input/key_modifiers.h"
#include "core/text_clipboard.h"
#include "render/core/renderer.h"
#include "text/unicode.h"
#include "ui/text_view.h"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace plume;

namespace {
  // Every character is 8 px wide and rows are 20 px high, so positions are easy to state.
  class FakeRenderer final : public Renderer {
  public:
    TextMetrics measureText(std::string_view text, float, const TextFont&, float, int, TextAlign, TextEllipsize, bool) override {
      TextMetrics metrics;
      metrics.width = 8.0F * static_cast<float>(utf8Length(text));
      metrics.top = -12.0F;
      metrics.bottom = 4.0F;
      return metrics;
    }
    TextMetrics measureFont(float, const TextFont&) override {
      TextMetrics metrics;
      metrics.top = -12.0F;
      metrics.bottom = 4.0F;
      return metrics;
    }
    void measureTextCursorStops(
        std::string_view text, float, const std::vector<std::size_t>& offsets, std::vector<float>& out, const TextFont&
    ) override {
      out.clear();
      for (const std::size_t offset : offsets) {
        out.push_back(8.0F * static_cast<float>(utf8Length(text.substr(0, offset))));
      }
    }
    void measureTextCursorStopsWrapped(
        std::string_view, float, const std::vector<std::size_t>&, float, std::vector<TextCursorStop>& out, const TextFont&
    ) override {
      out.clear();
    }
    TextMetrics measureGlyph(char32_t, float) override { return {}; }
    TextureManager& textureManager() override { std::abort(); }
    float renderScale() const noexcept override { return 1.0F; }
  };

  class FakeClipboard final : public TextClipboard {
  public:
    std::optional<std::string> clipboardText() override { return text; }
    void setClipboardText(std::string value) override { text = std::move(value); }
    std::optional<std::string> text;
  };

  struct Fixture {
    FakeRenderer renderer;
    Document document;
    TextView view;

    explicit Fixture(std::string text = {}, float width = 400.0F, bool wrap = true) {
      document.setText(std::move(text));
      TextViewOptions options;
      options.wrap = wrap;
      view.setOptions(options);
      view.setDocument(&document);
      view.arrange(renderer, {0, 0, width, 200});
      view.inputArea()->dispatchFocusGain();
    }
    void layout() { view.arrange(renderer, {0, 0, view.width(), 200}); }
    void key(std::uint32_t sym, std::uint32_t modifiers = 0) {
      view.inputArea()->dispatchKey(sym, 0, modifiers, true);
      layout();
    }
    void type(std::string_view text) {
      for (const char byte : text) {
        view.inputArea()->dispatchKey(static_cast<std::uint32_t>(byte), static_cast<std::uint32_t>(byte), 0, true);
      }
      layout();
    }
    void click(float x, float y, std::uint32_t time = 1000) {
      view.inputArea()->dispatchPress(x, y, BTN_LEFT, true, x, y, 1, time);
      view.inputArea()->dispatchPress(x, y, BTN_LEFT, false, x, y, 2, time + 10);
      layout();
    }
    std::string text() const { return document.buffer().text(); }
    std::size_t cursor() const { return view.selection().cursor; }
  };

  constexpr auto kShift = KeyMod::Shift;
  constexpr auto kCtrl = KeyMod::Ctrl;
} // namespace

int main() {
  setApplicationIdentity({.name = "plume-test", .envPrefix = "PLUME_TEST"});
  FakeClipboard clipboard;
  TextView::setClipboard(&clipboard);

  {
    // Typing, auto indent, undo in runs.
    Fixture f;
    f.type("  hello world");
    CHECK(f.text() == "  hello world");
    f.key(XKB_KEY_Return);
    f.type("next");
    CHECK(f.text() == "  hello world\n  next");
    CHECK(f.view.cursorLine() == 1 && f.view.cursorColumn() == 6);
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "  hello world\n  ");
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "  hello world");
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "  hello ");
    f.key(XKB_KEY_z, kCtrl | kShift);
    CHECK(f.text() == "  hello world");
    CHECK(f.document.modified());
  }
  {
    // Caret movement by character, word, line and document; selection with Shift.
    Fixture f("one two\nthree");
    f.key(XKB_KEY_Right);
    CHECK(f.cursor() == 1);
    f.key(XKB_KEY_Right, kCtrl);
    CHECK(f.cursor() == 3);
    f.key(XKB_KEY_Right, kCtrl);
    CHECK(f.cursor() == 7);
    f.key(XKB_KEY_Right);
    CHECK(f.cursor() == 8);
    f.key(XKB_KEY_Left, kCtrl);
    CHECK(f.cursor() == 7);
    f.key(XKB_KEY_Left, kCtrl);
    CHECK(f.cursor() == 4);
    f.key(XKB_KEY_End);
    CHECK(f.cursor() == 7);
    f.key(XKB_KEY_Home, kShift);
    CHECK(f.view.selection() == (Selection{.anchor = 7, .cursor = 0}));
    CHECK(f.view.selectedText() == "one two");
    f.key(XKB_KEY_Right);
    CHECK(f.view.selection() == Selection::at(7));
    f.key(XKB_KEY_End, kCtrl);
    CHECK(f.cursor() == 13);
    f.key(XKB_KEY_Up);
    CHECK(f.cursor() == 5);
    f.key(XKB_KEY_Down);
    CHECK(f.cursor() == 13);
    f.key(XKB_KEY_Home, kCtrl);
    CHECK(f.cursor() == 0);
    f.key(XKB_KEY_a, kCtrl);
    CHECK(f.view.selectedText() == "one two\nthree");
  }
  {
    // Deleting: characters, words, a selection; backspace at a line start joins lines.
    Fixture f("alpha beta\ngamma");
    f.key(XKB_KEY_End);
    f.key(XKB_KEY_BackSpace);
    CHECK(f.text() == "alpha bet\ngamma");
    f.key(XKB_KEY_BackSpace, kCtrl);
    CHECK(f.text() == "alpha \ngamma");
    f.key(XKB_KEY_Delete);
    CHECK(f.text() == "alpha gamma");
    f.key(XKB_KEY_Delete, kCtrl);
    CHECK(f.text() == "alpha ");
    f.key(XKB_KEY_Home, kShift);
    f.type("x");
    CHECK(f.text() == "x");
    // The whole run of deletions is not one step: the selection replace is its own.
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "alpha ");
  }
  {
    // Soft wrap: 400 px view, 3-digit gutter and paddings leave 334 px, 41 characters a row.
    const std::string words = "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm";
    Fixture f(words + "\nend");
    f.key(XKB_KEY_Down);
    // Still in the first line: the second row starts after "hhhh ".
    CHECK(f.view.cursorLine() == 0);
    CHECK(f.cursor() == 40);
    f.key(XKB_KEY_End);
    CHECK(f.cursor() == words.size());
    f.key(XKB_KEY_Home);
    CHECK(f.cursor() == 40);
    f.key(XKB_KEY_Up);
    CHECK(f.cursor() == 0);
    f.key(XKB_KEY_End);
    // The end of a wrapped row is before the stop that starts the next one.
    CHECK(f.cursor() == 39);
    f.key(XKB_KEY_Down);
    f.key(XKB_KEY_Down);
    CHECK(f.view.cursorLine() == 1);
    // Without wrapping the same keys move by lines.
    Fixture g(words + "\nend", 400.0F, false);
    g.key(XKB_KEY_Down);
    CHECK(g.view.cursorLine() == 1);
  }
  {
    // Pointer: click places the caret, double click takes the word, triple the line.
    Fixture f("first line\nsecond line");
    // Gutter 44 px + 8 px padding, rows 20 px: the 'c' of "second" starts at x = 52 + 2 * 8.
    f.click(52.0F + 2.0F * 8.0F + 1.0F, 30.0F);
    CHECK(f.cursor() == 13);
    f.click(52.0F + 2.0F * 8.0F + 1.0F, 30.0F, 1100);
    CHECK(f.view.selectedText() == "second");
    f.click(52.0F + 2.0F * 8.0F + 1.0F, 30.0F, 1200);
    CHECK(f.view.selectedText() == "second line");
    f.click(390.0F, 5.0F, 5000);
    CHECK(f.cursor() == 10);
  }
  {
    // Clipboard and tabs.
    Fixture f("abc def");
    f.key(XKB_KEY_Right, kCtrl | kShift);
    f.key(XKB_KEY_c, kCtrl);
    CHECK(clipboard.text == "abc");
    f.key(XKB_KEY_End);
    f.key(XKB_KEY_v, kCtrl);
    f.layout();
    CHECK(f.text() == "abc defabc");
    f.key(XKB_KEY_a, kCtrl);
    f.key(XKB_KEY_x, kCtrl);
    CHECK(f.text().empty() && clipboard.text == "abc defabc");
    clipboard.text = "one\r\ntwo\rthree";
    f.key(XKB_KEY_v, kCtrl);
    CHECK(f.text() == "one\ntwo\nthree");
    f.key(XKB_KEY_a, kCtrl);
    f.key(XKB_KEY_Tab);
    CHECK(f.text() == "\tone\n\ttwo\n\tthree");
    f.key(XKB_KEY_ISO_Left_Tab, kShift);
    CHECK(f.text() == "one\ntwo\nthree");
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "\tone\n\ttwo\n\tthree");
  }
  {
    // Sorting: the selected lines, or all of them, as one undo step; a final newline stays last.
    Fixture f("pear\napple\nfig\n");
    f.view.sortLines();
    CHECK(f.text() == "apple\nfig\npear\n");
    f.key(XKB_KEY_z, kCtrl);
    CHECK(f.text() == "pear\napple\nfig\n");
    f.key(XKB_KEY_Home, kCtrl);
    f.key(XKB_KEY_Down, kShift);
    f.key(XKB_KEY_Down, kShift);
    f.view.sortLines({.descending = true});
    CHECK(f.text() == "pear\napple\nfig\n");
    f.view.sortLines();
    CHECK(f.text() == "apple\npear\nfig\n");
    CHECK(f.view.selectedText() == "apple\npear");
  }
  {
    // Scrolling keeps the caret in view in a long text.
    std::string text;
    for (int i = 0; i < 500; ++i) {
      text += "line " + std::to_string(i) + "\n";
    }
    Fixture f(text);
    f.key(XKB_KEY_End, kCtrl);
    CHECK(f.view.cursorLine() == 500);
    CHECK(f.view.topLine() > 480);
    f.key(XKB_KEY_Home, kCtrl);
    CHECK(f.view.topLine() == 0);
    f.key(XKB_KEY_Page_Down);
    CHECK(f.view.cursorLine() == 9);
  }
  {
    // Marking offers the text as the primary selection; a middle click pastes it where it lands.
    FakeClipboard primary;
    TextView::setPrimarySelection(&primary);
    Fixture f("alpha beta\ngamma");
    f.key(XKB_KEY_Right, kCtrl | kShift);
    CHECK(primary.text == "alpha");
    // A drag offers its text once the button is let go.
    primary.text.reset();
    f.view.inputArea()->dispatchPress(52.0F + 6.0F * 8.0F + 1.0F, 5.0F, BTN_LEFT, true, 0, 0, 1, 9000);
    f.view.inputArea()->dispatchMotion(52.0F + 10.0F * 8.0F + 1.0F, 5.0F);
    CHECK(!primary.text);
    f.view.inputArea()->dispatchPress(52.0F + 10.0F * 8.0F + 1.0F, 5.0F, BTN_LEFT, false, 0, 0, 2, 9010);
    CHECK(primary.text == "beta");
    f.view.inputArea()->dispatchPress(52.0F + 5.0F * 8.0F + 1.0F, 25.0F, BTN_MIDDLE, true, 0, 0, 3, 9500);
    f.layout();
    CHECK(f.text() == "alpha beta\ngammabeta");
    TextView::setPrimarySelection(nullptr);
  }
  TextView::setClipboard(nullptr);
  return 0;
}

#include "check.h"
#include "text/buffer.h"

#include <random>
#include <string>

using plume::Buffer;

namespace {
  // The same text as a plain string, to compare every query against.
  struct Model {
    std::string text;
    std::size_t lineCount() const { return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1; }
    std::size_t lineStart(std::size_t line) const {
      std::size_t at = 0;
      for (std::size_t i = 0; i < line; ++i) {
        at = text.find('\n', at) + 1;
      }
      return at;
    }
    std::size_t lineOf(std::size_t offset) const {
      return static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
    }
  };

  void same(const Buffer& buffer, const Model& model) {
    CHECK(buffer.size() == model.text.size());
    CHECK(buffer.text() == model.text);
    CHECK(buffer.lineCount() == model.lineCount());
    for (std::size_t line = 0; line < model.lineCount(); ++line) {
      CHECK(buffer.lineStart(line) == model.lineStart(line));
      const std::size_t end = line + 1 < model.lineCount() ? model.lineStart(line + 1) - 1 : model.text.size();
      CHECK(buffer.lineEnd(line) == end);
      CHECK(buffer.lineText(line) == model.text.substr(model.lineStart(line), end - model.lineStart(line)));
    }
    for (std::size_t offset = 0; offset <= model.text.size(); ++offset) {
      CHECK(buffer.lineOf(offset) == model.lineOf(offset));
      CHECK(buffer.byteAt(offset) == (offset < model.text.size() ? model.text[offset] : '\0'));
    }
  }
} // namespace

int main() {
  {
    Buffer empty;
    CHECK(empty.size() == 0);
    CHECK(empty.lineCount() == 1);
    CHECK(empty.lineStart(0) == 0);
    CHECK(empty.lineEnd(0) == 0);
    CHECK(empty.lineText(0).empty());
    empty.insert(0, "a\nb");
    CHECK(empty.text() == "a\nb");
    CHECK(empty.lineCount() == 2);
    CHECK(empty.lineText(1) == "b");
  }
  {
    Buffer buffer("one\ntwo\nthree\n");
    CHECK(buffer.lineCount() == 4);
    CHECK(buffer.lineText(2) == "three");
    CHECK(buffer.lineText(3).empty());
    buffer.insert(4, "2: ");
    CHECK(buffer.lineText(1) == "2: two");
    buffer.erase(0, 4);
    CHECK(buffer.text() == "2: two\nthree\n");
    // Typing in one place stays one piece.
    const std::size_t pieces = buffer.pieceCount();
    buffer.insert(6, "!");
    buffer.insert(7, "?");
    buffer.insert(8, "\n");
    CHECK(buffer.pieceCount() == pieces + 2);
    CHECK(buffer.text() == "2: two!?\n\nthree\n");
  }
  std::mt19937 random(7);
  Buffer buffer("alpha\nbeta\n\ngamma");
  Model model{"alpha\nbeta\n\ngamma"};
  const std::string alphabet = "ab\ncd\n ";
  for (int round = 0; round < 1500; ++round) {
    const std::size_t offset = random() % (model.text.size() + 1);
    if (random() % 3 != 0) {
      std::string piece;
      for (std::size_t i = random() % 6; i > 0; --i) {
        piece.push_back(alphabet[random() % alphabet.size()]);
      }
      buffer.insert(offset, piece);
      model.text.insert(offset, piece);
    } else {
      const std::size_t length = random() % 9;
      buffer.erase(offset, length);
      model.text.erase(offset, std::min(length, model.text.size() - offset));
    }
    if (round % 25 == 0) {
      same(buffer, model);
    }
  }
  same(buffer, model);
  return 0;
}

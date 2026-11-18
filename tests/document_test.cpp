#include "check.h"
#include "text/document.h"

using plume::Document;
using plume::EditKind;
using plume::Selection;

int main() {
  Document document;
  document.setText("hello");
  CHECK(!document.modified());

  // A run of typing undoes as one step; a new word starts another.
  std::size_t at = 5;
  for (const char* key : {" ", "w", "o", "r", "l", "d"}) {
    document.edit(at, 0, key, Selection::at(at), Selection::at(at + 1), EditKind::Typing);
    ++at;
  }
  CHECK(document.buffer().text() == "hello world");
  CHECK(document.modified());
  auto selection = document.undo();
  CHECK(selection && selection->cursor == 6);
  CHECK(document.buffer().text() == "hello ");
  selection = document.undo();
  CHECK(document.buffer().text() == "hello");
  CHECK(!document.modified());
  CHECK(!document.undo());
  selection = document.redo();
  selection = document.redo();
  CHECK(selection && selection->cursor == 11);
  CHECK(document.buffer().text() == "hello world");
  CHECK(!document.redo());

  // Backspaces join too, and restore the caret where the run began.
  document.breakRun();
  for (at = 11; at > 8; --at) {
    document.edit(at - 1, 1, "", Selection::at(at), Selection::at(at - 1), EditKind::Backspace);
  }
  CHECK(document.buffer().text() == "hello wo");
  selection = document.undo();
  CHECK(selection && selection->cursor == 11);
  CHECK(document.buffer().text() == "hello world");

  // A group undoes as a whole.
  document.beginGroup(Selection::at(0));
  document.replace(0, 5, "HELLO");
  document.replace(6, 5, "WORLD");
  document.endGroup(Selection::at(11));
  CHECK(document.buffer().text() == "HELLO WORLD");
  document.undo();
  CHECK(document.buffer().text() == "hello world");

  // Saved in the middle of the history, then edited away from it: still modified after undo.
  document.markSaved();
  CHECK(!document.modified());
  document.edit(0, 0, "x", Selection::at(0), Selection::at(1));
  CHECK(document.modified());
  document.undo();
  CHECK(!document.modified());
  document.undo();
  CHECK(document.modified());
  document.edit(0, 0, "y", Selection::at(0), Selection::at(1));
  document.undo();
  CHECK(document.modified());
  return 0;
}

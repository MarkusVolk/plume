#include "check.h"
#include "document/file_io.h"
#include "document/session.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace plume;

int main() {
  {
    // Plain UTF-8 stays as it is.
    const auto file = decodeFile("a\nb\n");
    CHECK(file && file->text == "a\nb\n" && file->format == FileFormat{});
    CHECK(encodeFile(file->text, file->format) == "a\nb\n");
  }
  {
    // CR LF throughout is edited with '\n' and written back as it came.
    const auto file = decodeFile("\xEF\xBB\xBFone\r\ntwo\r\n");
    CHECK(file && file->text == "one\ntwo\n");
    CHECK(file->format.lineEnding == LineEnding::CrLf && file->format.byteOrderMark);
    CHECK(encodeFile(file->text, file->format) == "\xEF\xBB\xBFone\r\ntwo\r\n");
  }
  {
    // Mixed line ends are left alone.
    const auto file = decodeFile("one\r\ntwo\nthree");
    CHECK(file && file->text == "one\r\ntwo\nthree" && file->format.lineEnding == LineEnding::Lf);
  }
  {
    // Western text that is not UTF-8 round-trips through Windows-1252.
    const std::string latin = "Gr\xFC\xDF" "e \x80\n";
    const auto file = decodeFile(latin);
    CHECK(file && file->format.encoding == "WINDOWS-1252");
    CHECK(file->text == "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC\n");
    CHECK(encodeFile(file->text, file->format) == latin);
    // What the encoding cannot hold is refused instead of written wrong.
    CHECK(!encodeFile("\xE4\xB8\xAD", file->format));
  }
  {
    const std::string utf16("\xFF\xFEh\0i\0\n\0", 8);
    const auto file = decodeFile(utf16);
    CHECK(file && file->text == "hi\n" && file->format.encoding == "UTF-16LE");
    CHECK(encodeFile(file->text, file->format) == utf16);
  }
  {
    // Binary data opens as a rendition: text stays, other bytes become dots, long runs are broken
    // between strings rather than inside them.
    const auto file = decodeFile(std::string("\x7F" "ELF\0\0\xFF/home/x\n\x01", 16));
    CHECK(file && file->binary);
    CHECK(file->text == "\xC2\xB7" "ELF\xC2\xB7\xC2\xB7\xC3\xBF/home/x\n\xC2\xB7");
    std::string blob(3000, '\0');
    blob.replace(2040, 20, "/build/path/in/here/");
    const auto broken = decodeFile(blob);
    CHECK(broken && broken->binary);
    CHECK(broken->text.find("/build/path/in/here/") != std::string::npos);
    CHECK(broken->text.find('\n') != std::string::npos);
  }

  // Files, the session and drafts in a private home.
  const auto home = std::filesystem::temp_directory_path() / ("plume-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(home);
  setenv("HOME", home.c_str(), 1);
  unsetenv("XDG_STATE_HOME");
  unsetenv("XDG_DATA_HOME");
  {
    const auto path = home / "note.txt";
    CHECK(saveFile(path, "first\nsecond\n", {.encoding = "UTF-8", .byteOrderMark = false, .lineEnding = LineEnding::CrLf}));
    const auto loaded = loadFile(path);
    CHECK(loaded && loaded->text == "first\nsecond\n" && loaded->format.lineEnding == LineEnding::CrLf);
    CHECK(!loadFile(home / "missing.txt"));
  }
  {
    CHECK(loadSession().tabs.empty());
    Session session;
    session.active = 1;
    session.width = 800;
    session.height = 600;
    session.tabs.push_back({.path = "/tmp/a.txt", .draft = "", .cursor = 3, .anchor = 1, .topLine = 7});
    const std::string draft = newDraftName();
    session.tabs.push_back({.path = "", .draft = draft, .cursor = 0, .anchor = 0, .topLine = 0});
    CHECK(writeDraft(draft, "unsaved \xC3\xBC text"));
    CHECK(writeDraft("stale.txt", "old"));
    saveSession(session);
    CHECK(loadSession() == session);
    CHECK(readDraft(draft) == "unsaved \xC3\xBC text");
    pruneDrafts(session);
    CHECK(!readDraft("stale.txt"));
    CHECK(readDraft(draft).has_value());
    // A draft name never leaves the drafts folder.
    CHECK(!writeDraft("../escape.txt", "x"));
    CHECK(!readDraft("../note.txt"));
    removeDraft(draft);
    CHECK(!readDraft(draft));
  }
  std::filesystem::remove_all(home);
  return 0;
}

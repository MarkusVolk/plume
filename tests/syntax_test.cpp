#include "check.h"
#include "syntax/highlighter.h"

#include <cstdlib>

using namespace plume;

namespace {
  // The style of the first byte of `needle` in the document.
  TokenStyle styleOf(Highlighter& syntax, const Document& document, std::string_view needle) {
    const std::string text = document.buffer().text();
    const std::size_t at = text.find(needle);
    CHECK(at != std::string::npos);
    std::vector<TokenStyle> styles;
    syntax.styles(0, text.size(), styles);
    CHECK(styles.size() == text.size());
    return styles[at];
  }

  struct Colored {
    Document document;
    std::unique_ptr<Highlighter> syntax;
    Colored(const std::string& language, std::string text) {
      document.setText(std::move(text));
      syntax = Highlighter::create(language, document.buffer());
      if (syntax) {
        document.setObserver([this](const TextChange& change) { syntax->edited(change); });
      }
    }
    TokenStyle at(std::string_view needle) { return styleOf(*syntax, document, needle); }
  };
} // namespace

int main() {
  CHECK(detectLanguage("/x/recipe_1.0.bb", "") == "bitbake");
  CHECK(detectLanguage("/x/conf/local.conf", "") == "bitbake");
  CHECK(detectLanguage("/x/meson.build", "") == "meson");
  CHECK(detectLanguage("/x/main.cpp", "") == "cpp");
  CHECK(detectLanguage("/x/run", "#!/usr/bin/env python3") == "python");
  CHECK(detectLanguage("/x/run", "#!/bin/sh") == "bash");
  CHECK(detectLanguage("/x/notes.txt", "plain").empty());
  CHECK(languageForAlias("C++") == "cpp" && languageForAlias("sh") == "bash");
  CHECK(styleForCapture("keyword.control.return") == TokenStyle::Keyword);
  CHECK(styleForCapture("string.escape") == TokenStyle::Escape);
  CHECK(styleForCapture("variable") == TokenStyle::None);
  CHECK(styleForCapture("stringy") == TokenStyle::None);

  // The rest needs parsers and queries; a build without them skips it.
  const char* grammars = std::getenv("PLUME_TEST_GRAMMARS");
  if (!grammars) {
    return 77;
  }
  GrammarStore::instance().setSearchPaths({std::string(grammars) + "/parsers"}, {std::string(grammars) + "/queries"});
  CHECK(!Highlighter::create("no_such_language", Buffer()));
  CHECK(!Highlighter::create("../c", Buffer()));

  {
    Colored c("c", "// note\nint main(void) {\n  return printf(\"hi %d\\n\", 42);\n}\n");
    CHECK(c.syntax);
    CHECK(c.at("// note") == TokenStyle::Comment);
    CHECK(c.at("int") == TokenStyle::Type);
    CHECK(c.at("return") == TokenStyle::Keyword);
    CHECK(c.at("printf") == TokenStyle::Function);
    CHECK(c.at("\"hi") == TokenStyle::String);
    CHECK(c.at("42") == TokenStyle::Number);
    // An edit keeps the tree current: the string becomes a comment, undo brings it back.
    const std::size_t at = c.document.buffer().text().find("return");
    const std::uint64_t before = c.syntax->revision();
    c.document.edit(at, 0, "// ", Selection::at(at), Selection::at(at + 3));
    CHECK(c.syntax->revision() != before);
    CHECK(c.at("\"hi") == TokenStyle::Comment);
    c.document.undo();
    CHECK(c.at("\"hi") == TokenStyle::String);
    CHECK(c.at("return") == TokenStyle::Keyword);
    // A whole new text starts over.
    c.document.setText("#define LIMIT 7\n");
    CHECK(c.at("#define") == TokenStyle::Keyword);
    CHECK(c.at("LIMIT") == TokenStyle::Constant);
  }
  {
    // C++ builds on the C query.
    Colored cpp("cpp", "namespace a { class B { }; }\nconst char* s = \"x\";\n");
    CHECK(cpp.syntax);
    CHECK(cpp.at("namespace") == TokenStyle::Keyword);
    CHECK(cpp.at("class") == TokenStyle::Keyword);
    CHECK(cpp.at("const") == TokenStyle::Keyword);
    CHECK(cpp.at("\"x\"") == TokenStyle::String);
  }
  {
    // A recipe: BitBake itself, and the shell of a task through an injection.
    Colored bb("bitbake", "SUMMARY = \"text\"\ninherit meson\n\ndo_install() {\n\tinstall -d ${D}/usr # dir\n\tif true; then echo \"x\"; fi\n}\n");
    CHECK(bb.syntax);
    CHECK(bb.at("\"text\"") == TokenStyle::String);
    CHECK(bb.at("inherit") == TokenStyle::Keyword);
    CHECK(bb.at("if true") == TokenStyle::Keyword);
    CHECK(bb.at("# dir") == TokenStyle::Comment);
  }
  {
    // Markdown: headings from the block grammar, code spans from the inline one, C in a fence.
    Colored md("markdown", "# Title\n\nSome `code` here.\n\n```c\nint x = 1;\n```\n");
    CHECK(md.syntax);
    CHECK(md.at("Title") == TokenStyle::Heading);
    CHECK(md.at("code`") == TokenStyle::String);
    CHECK(md.at("int x") == TokenStyle::Type);
  }
  return 0;
}

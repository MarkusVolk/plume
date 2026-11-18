#include "check.h"
#include "text/operations.h"
#include "text/unicode.h"

using namespace plume;

int main() {
  CHECK(sortLines("pear\nApple\nbanana") == "Apple\nbanana\npear");
  CHECK(sortLines("pear\nApple\nbanana", {.descending = true}) == "pear\nbanana\nApple");
  CHECK(sortLines("b\na\nB\na", {.unique = true}) == "a\nb");
  CHECK(sortLines("b\n\na") == "\na\nb");
  CHECK(sortLines("single") == "single");

  Buffer buffer("one\ntwo\nthree\nfour");
  auto range = selectedLines(buffer, {.anchor = 5, .cursor = 9});
  CHECK(range.first == 1 && range.last == 2 && range.begin == 4 && range.end == 13);
  // Ending at a line start leaves that line out.
  range = selectedLines(buffer, {.anchor = 0, .cursor = 8});
  CHECK(range.first == 0 && range.last == 1 && range.end == 7);
  range = selectedLines(buffer, Selection::at(8));
  CHECK(range.first == 2 && range.last == 2);

  auto literal = Pattern::compile("a.b", {});
  CHECK(literal.has_value());
  CHECK(literal->findAll("a.b axb A.B") == (std::vector<Match>{{0, 3}, {8, 3}}));
  auto exact = Pattern::compile("a.b", {.caseSensitive = true});
  CHECK(exact->findAll("a.b axb A.B").size() == 1);
  auto word = Pattern::compile("cat", {.wholeWord = true});
  CHECK(word->findAll("cat concat cat.") == (std::vector<Match>{{0, 3}, {11, 3}}));
  auto regex = Pattern::compile("(\\w+)=(\\d+)", {.regex = true});
  CHECK(regex.has_value());
  const std::string text = "a=1, bb=22";
  const auto matches = regex->findAll(text);
  CHECK(matches == (std::vector<Match>{{0, 3}, {5, 5}}));
  CHECK(regex->expand(text, matches[1], "\\2:\\1") == "22:bb");
  CHECK(literal->expand("a.b", {0, 3}, "\\1") == "\\1");
  CHECK(!Pattern::compile("(", {.regex = true}).has_value());

  const auto breaks = analyzeLine("ab e\xCC\x81 \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD z");
  // "e" + combining acute is one stop, the thumbs-up with its skin tone too.
  CHECK(breaks.stops == (std::vector<std::uint32_t>{0, 1, 2, 3, 6, 7, 15, 16, 17}));
  CHECK(stopAfter(breaks.stops, 3) == 6);
  CHECK(stopBefore(breaks.stops, 15) == 7);
  CHECK(stopAtOrBefore(breaks.stops, 5) == 3);
  CHECK(stopAtOrAfter(breaks.stops, 8) == 15);
  CHECK(analyzeLine("").stops == (std::vector<std::uint32_t>{0}));
  return 0;
}

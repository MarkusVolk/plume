#pragma once

#include "syntax/grammar.h"
#include "syntax/token_style.h"
#include "text/document.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TSParser;
struct TSQueryCursor;
struct TSTree;

namespace plume {
  // The syntax tree of one document, kept current with its edits, and the colors that follow
  // from it. Other languages embedded in the text (shell in a recipe, code blocks in Markdown)
  // get their own trees.
  class Highlighter final : public SyntaxSource {
  public:
    // Null when there is no grammar for the language, or the text is too large to parse at ease.
    static std::unique_ptr<Highlighter> create(const std::string& language, const Buffer& buffer);
    ~Highlighter() override;
    Highlighter(const Highlighter&) = delete;
    Highlighter& operator=(const Highlighter&) = delete;

    // Tell every change of the buffer, in order.
    void edited(const TextChange& change);

    void styles(std::size_t begin, std::size_t end, std::vector<TokenStyle>& out) override;
    std::uint64_t revision() const override { return m_revision; }

  private:
    struct Range {
      std::size_t begin = 0;
      std::size_t end = 0;
      TextPoint beginPoint;
      TextPoint endPoint;
    };
    struct Layer {
      std::shared_ptr<const Grammar> grammar;
      TSTree* tree = nullptr;
      // Empty for the document's own language: the whole text.
      std::vector<Range> ranges;
    };

    Highlighter(std::shared_ptr<const Grammar> grammar, const Buffer& buffer);
    void parse();
    TSTree* parseLayer(const Layer& layer, TSTree* old);
    void findInjections();
    void paint(const Layer& layer, std::size_t begin, std::size_t end, std::vector<TokenStyle>& out);

    const Buffer& m_buffer;
    TSParser* m_parser = nullptr;
    TSQueryCursor* m_cursor = nullptr;
    Layer m_root;
    // One per embedded language, all its stretches in one tree.
    std::vector<Layer> m_injected;
    std::uint64_t m_revision = 1;
    bool m_dirty = true;
    bool m_tooLarge = false;
  };
} // namespace plume

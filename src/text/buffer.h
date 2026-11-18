#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace plume {
  // Text storage for one document: a piece table over the loaded text and an append-only buffer of
  // everything typed since. Offsets are bytes of UTF-8; lines end at '\n' (the loader normalises
  // line endings). Both buffers keep the positions of their newlines, so line lookups stay a binary
  // search however large the file is.
  class Buffer {
  public:
    Buffer() = default;
    explicit Buffer(std::string text);

    std::size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }
    // Always at least 1: text without a newline is one line, a trailing newline starts an empty last line.
    std::size_t lineCount() const noexcept { return m_newlines + 1; }

    std::size_t lineStart(std::size_t line) const;
    // Offset of the line's '\n', or size() for the last line.
    std::size_t lineEnd(std::size_t line) const;
    std::size_t lineLength(std::size_t line) const { return lineEnd(line) - lineStart(line); }
    std::size_t lineOf(std::size_t offset) const;

    std::string text(std::size_t offset, std::size_t length) const;
    std::string text() const { return text(0, m_size); }
    std::string lineText(std::size_t line) const;
    // The stored bytes from `offset` up to the end of the piece that holds it, without copying;
    // empty past the end. Valid until the next edit.
    std::string_view chunkAt(std::size_t offset) const;
    // The byte at offset, or '\0' past the end.
    char byteAt(std::size_t offset) const;

    void insert(std::size_t offset, std::string_view text);
    void erase(std::size_t offset, std::size_t length);

    // Changes with every edit; layout caches compare it.
    std::size_t revision() const noexcept { return m_revision; }
    std::size_t pieceCount() const noexcept { return m_pieces.size(); }

  private:
    struct Piece {
      bool added = false;
      std::size_t start = 0;
      std::size_t length = 0;
      std::size_t newlines = 0;
    };
    struct Position {
      std::size_t piece = 0;
      std::size_t within = 0;
    };

    const std::string& bytes(const Piece& piece) const noexcept { return piece.added ? m_added : m_original; }
    const std::vector<std::size_t>& breaks(const Piece& piece) const noexcept {
      return piece.added ? m_addedBreaks : m_originalBreaks;
    }
    std::size_t countBreaks(const Piece& piece, std::size_t start, std::size_t length) const;
    Piece slice(const Piece& piece, std::size_t from, std::size_t length) const;
    void index() const;
    // The piece holding `offset`; an offset on a boundary belongs to the piece it starts.
    Position locate(std::size_t offset) const;
    void changed();

    std::string m_original;
    std::string m_added;
    std::vector<std::size_t> m_originalBreaks;
    std::vector<std::size_t> m_addedBreaks;
    std::vector<Piece> m_pieces;
    std::size_t m_size = 0;
    std::size_t m_newlines = 0;
    std::size_t m_revision = 0;
    // Bytes and newlines before each piece, rebuilt on demand after an edit.
    mutable std::vector<std::size_t> m_byteIndex;
    mutable std::vector<std::size_t> m_lineIndex;
    mutable bool m_indexed = false;
  };
} // namespace plume

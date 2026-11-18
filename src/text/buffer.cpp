#include "text/buffer.h"

#include <algorithm>
#include <cstring>

namespace {
  void appendBreaks(std::vector<std::size_t>& breaks, std::string_view text, std::size_t base) {
    const char* data = text.data();
    const char* end = data + text.size();
    for (const char* at = data; (at = static_cast<const char*>(std::memchr(at, '\n', static_cast<std::size_t>(end - at)))); ++at) {
      breaks.push_back(base + static_cast<std::size_t>(at - data));
    }
  }
} // namespace

plume::Buffer::Buffer(std::string text) : m_original(std::move(text)) {
  appendBreaks(m_originalBreaks, m_original, 0);
  m_size = m_original.size();
  m_newlines = m_originalBreaks.size();
  if (m_size > 0) {
    m_pieces.push_back({.added = false, .start = 0, .length = m_size, .newlines = m_newlines});
  }
}

std::size_t plume::Buffer::countBreaks(const Piece& piece, std::size_t start, std::size_t length) const {
  const auto& all = breaks(piece);
  const auto first = std::lower_bound(all.begin(), all.end(), start);
  const auto last = std::lower_bound(first, all.end(), start + length);
  return static_cast<std::size_t>(last - first);
}

plume::Buffer::Piece plume::Buffer::slice(const Piece& piece, std::size_t from, std::size_t length) const {
  Piece part{.added = piece.added, .start = piece.start + from, .length = length, .newlines = 0};
  part.newlines = countBreaks(piece, part.start, length);
  return part;
}

void plume::Buffer::index() const {
  if (m_indexed) {
    return;
  }
  m_byteIndex.resize(m_pieces.size() + 1);
  m_lineIndex.resize(m_pieces.size() + 1);
  std::size_t bytesBefore = 0, linesBefore = 0;
  for (std::size_t i = 0; i < m_pieces.size(); ++i) {
    m_byteIndex[i] = bytesBefore;
    m_lineIndex[i] = linesBefore;
    bytesBefore += m_pieces[i].length;
    linesBefore += m_pieces[i].newlines;
  }
  m_byteIndex.back() = bytesBefore;
  m_lineIndex.back() = linesBefore;
  m_indexed = true;
}

plume::Buffer::Position plume::Buffer::locate(std::size_t offset) const {
  index();
  // First piece that starts after the offset, minus one: the piece that holds it.
  const auto next = std::upper_bound(m_byteIndex.begin(), m_byteIndex.end() - 1, offset);
  const auto piece = static_cast<std::size_t>(next - m_byteIndex.begin()) - 1;
  return {.piece = piece, .within = offset - m_byteIndex[piece]};
}

void plume::Buffer::changed() {
  m_indexed = false;
  ++m_revision;
}

std::size_t plume::Buffer::lineStart(std::size_t line) const {
  if (line == 0) {
    return 0;
  }
  if (line > m_newlines) {
    return m_size;
  }
  index();
  const std::size_t wanted = line - 1;
  const auto next = std::upper_bound(m_lineIndex.begin(), m_lineIndex.end() - 1, wanted);
  const auto at = static_cast<std::size_t>(next - m_lineIndex.begin()) - 1;
  const Piece& piece = m_pieces[at];
  const auto& all = breaks(piece);
  const auto first = std::lower_bound(all.begin(), all.end(), piece.start);
  const std::size_t position = *(first + static_cast<std::ptrdiff_t>(wanted - m_lineIndex[at]));
  return m_byteIndex[at] + (position - piece.start) + 1;
}

std::size_t plume::Buffer::lineEnd(std::size_t line) const {
  return line >= m_newlines ? m_size : lineStart(line + 1) - 1;
}

std::size_t plume::Buffer::lineOf(std::size_t offset) const {
  if (m_pieces.empty()) {
    return 0;
  }
  if (offset >= m_size) {
    return m_newlines;
  }
  const auto [at, within] = locate(offset);
  const Piece& piece = m_pieces[at];
  return m_lineIndex[at] + countBreaks(piece, piece.start, within);
}

std::string plume::Buffer::text(std::size_t offset, std::size_t length) const {
  std::string out;
  if (offset >= m_size || length == 0) {
    return out;
  }
  length = std::min(length, m_size - offset);
  out.reserve(length);
  auto [at, within] = locate(offset);
  while (length > 0 && at < m_pieces.size()) {
    const Piece& piece = m_pieces[at];
    const std::size_t take = std::min(length, piece.length - within);
    out.append(bytes(piece), piece.start + within, take);
    length -= take;
    within = 0;
    ++at;
  }
  return out;
}

std::string plume::Buffer::lineText(std::size_t line) const {
  const std::size_t start = lineStart(line);
  return text(start, lineEnd(line) - start);
}

char plume::Buffer::byteAt(std::size_t offset) const {
  if (offset >= m_size) {
    return '\0';
  }
  const auto [at, within] = locate(offset);
  const Piece& piece = m_pieces[at];
  return bytes(piece)[piece.start + within];
}

void plume::Buffer::insert(std::size_t offset, std::string_view text) {
  if (text.empty()) {
    return;
  }
  offset = std::min(offset, m_size);
  const std::size_t start = m_added.size();
  appendBreaks(m_addedBreaks, text, start);
  m_added.append(text);
  Piece fresh{.added = true, .start = start, .length = text.size(), .newlines = 0};
  fresh.newlines = m_addedBreaks.size() - countBreaks(fresh, 0, start);

  if (m_pieces.empty()) {
    m_pieces.push_back(fresh);
  } else if (offset == m_size) {
    Piece& last = m_pieces.back();
    if (last.added && last.start + last.length == start) {
      last.length += fresh.length;
      last.newlines += fresh.newlines;
    } else {
      m_pieces.push_back(fresh);
    }
  } else {
    const auto [at, within] = locate(offset);
    if (within == 0) {
      // Typing continues the piece before the caret when that piece ends where the added buffer did.
      if (at > 0 && m_pieces[at - 1].added && m_pieces[at - 1].start + m_pieces[at - 1].length == start) {
        m_pieces[at - 1].length += fresh.length;
        m_pieces[at - 1].newlines += fresh.newlines;
      } else {
        m_pieces.insert(m_pieces.begin() + static_cast<std::ptrdiff_t>(at), fresh);
      }
    } else {
      const Piece whole = m_pieces[at];
      const Piece head = slice(whole, 0, within);
      const Piece tail = slice(whole, within, whole.length - within);
      m_pieces[at] = head;
      m_pieces.insert(m_pieces.begin() + static_cast<std::ptrdiff_t>(at) + 1, {fresh, tail});
    }
  }
  m_size += fresh.length;
  m_newlines += fresh.newlines;
  changed();
}

void plume::Buffer::erase(std::size_t offset, std::size_t length) {
  if (offset >= m_size || length == 0) {
    return;
  }
  length = std::min(length, m_size - offset);
  const auto [first, within] = locate(offset);
  std::vector<Piece> kept;
  std::size_t removedBreaks = 0;
  std::size_t at = first;
  std::size_t from = within;
  std::size_t left = length;
  while (left > 0 && at < m_pieces.size()) {
    const Piece whole = m_pieces[at];
    const std::size_t take = std::min(left, whole.length - from);
    removedBreaks += countBreaks(whole, whole.start + from, take);
    if (from > 0) {
      kept.push_back(slice(whole, 0, from));
    }
    if (from + take < whole.length) {
      kept.push_back(slice(whole, from + take, whole.length - from - take));
    }
    left -= take;
    from = 0;
    ++at;
  }
  m_pieces.erase(m_pieces.begin() + static_cast<std::ptrdiff_t>(first), m_pieces.begin() + static_cast<std::ptrdiff_t>(at));
  m_pieces.insert(m_pieces.begin() + static_cast<std::ptrdiff_t>(first), kept.begin(), kept.end());
  m_size -= length;
  m_newlines -= removedBreaks;
  changed();
}

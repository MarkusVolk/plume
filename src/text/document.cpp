#include "text/document.h"

#include <utility>

void plume::Document::setText(std::string text) {
  m_buffer = Buffer(std::move(text));
  m_undo.clear();
  m_redo.clear();
  m_group.reset();
  m_savedDepth = 0;
  m_sealed = true;
}

void plume::Document::apply(std::size_t offset, std::size_t length, std::string_view text, Step& step) {
  Change change{.offset = offset, .removed = m_buffer.text(offset, length), .inserted = std::string(text)};
  m_buffer.erase(offset, change.removed.size());
  m_buffer.insert(offset, text);
  step.changes.push_back(std::move(change));
}

bool plume::Document::continues(
    const Step& step, std::size_t offset, std::size_t length, std::string_view text, EditKind kind
) const {
  if (m_sealed || kind == EditKind::Discrete || step.kind != kind || step.changes.size() != 1) {
    return false;
  }
  const Change& last = step.changes.front();
  switch (kind) {
  case EditKind::Typing:
    // A new line or a new word after whitespace starts its own step, as in most editors.
    return length == 0
        && last.removed.empty()
        && offset == last.offset + last.inserted.size()
        && text.find('\n') == std::string_view::npos
        && !(text.front() != ' ' && text.front() != '\t' && (last.inserted.back() == ' ' || last.inserted.back() == '\t'));
  case EditKind::Backspace:
    return text.empty() && last.inserted.empty() && offset + length == last.offset;
  case EditKind::Delete:
    return text.empty() && last.inserted.empty() && offset == last.offset;
  case EditKind::Discrete:
    break;
  }
  return false;
}

void plume::Document::edit(
    std::size_t offset, std::size_t length, std::string_view text, Selection before, Selection after, EditKind kind
) {
  if (m_group) {
    replace(offset, length, text);
    return;
  }
  if (length == 0 && text.empty()) {
    return;
  }
  m_redo.clear();
  if (!m_undo.empty() && continues(m_undo.back(), offset, length, text, kind)) {
    Step& step = m_undo.back();
    Change& last = step.changes.front();
    if (kind == EditKind::Typing) {
      last.inserted.append(text);
      m_buffer.insert(offset, text);
    } else {
      const std::string removed = m_buffer.text(offset, length);
      m_buffer.erase(offset, length);
      if (kind == EditKind::Backspace) {
        last.removed.insert(0, removed);
        last.offset = offset;
      } else {
        last.removed.append(removed);
      }
    }
    step.after = after;
    return;
  }
  // A saved state inside the history that this edit discards can no longer be reached.
  if (m_savedDepth && *m_savedDepth > m_undo.size()) {
    m_savedDepth.reset();
  }
  Step step{.changes = {}, .before = before, .after = after, .kind = kind};
  apply(offset, length, text, step);
  m_undo.push_back(std::move(step));
  m_sealed = false;
}

void plume::Document::beginGroup(Selection before) {
  m_group = Step{.changes = {}, .before = before, .after = before, .kind = EditKind::Discrete};
}

void plume::Document::replace(std::size_t offset, std::size_t length, std::string_view text) {
  if (!m_group || (length == 0 && text.empty())) {
    return;
  }
  apply(offset, length, text, *m_group);
}

void plume::Document::endGroup(Selection after) {
  if (!m_group) {
    return;
  }
  Step step = std::move(*m_group);
  m_group.reset();
  if (step.changes.empty()) {
    return;
  }
  step.after = after;
  m_redo.clear();
  if (m_savedDepth && *m_savedDepth > m_undo.size()) {
    m_savedDepth.reset();
  }
  m_undo.push_back(std::move(step));
  m_sealed = true;
}

std::optional<plume::Selection> plume::Document::undo() {
  if (m_undo.empty()) {
    return std::nullopt;
  }
  Step step = std::move(m_undo.back());
  m_undo.pop_back();
  for (auto change = step.changes.rbegin(); change != step.changes.rend(); ++change) {
    m_buffer.erase(change->offset, change->inserted.size());
    m_buffer.insert(change->offset, change->removed);
  }
  const Selection selection = step.before;
  m_redo.push_back(std::move(step));
  m_sealed = true;
  return selection;
}

std::optional<plume::Selection> plume::Document::redo() {
  if (m_redo.empty()) {
    return std::nullopt;
  }
  Step step = std::move(m_redo.back());
  m_redo.pop_back();
  for (const Change& change : step.changes) {
    m_buffer.erase(change.offset, change.removed.size());
    m_buffer.insert(change.offset, change.inserted);
  }
  const Selection selection = step.after;
  m_undo.push_back(std::move(step));
  m_sealed = true;
  return selection;
}

void plume::Document::markSaved() {
  m_savedDepth = m_undo.size();
  m_sealed = true;
}

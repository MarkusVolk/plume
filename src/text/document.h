#pragma once

#include "text/buffer.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace plume {
  struct Selection {
    std::size_t anchor = 0;
    std::size_t cursor = 0;

    std::size_t begin() const noexcept { return anchor < cursor ? anchor : cursor; }
    std::size_t end() const noexcept { return anchor < cursor ? cursor : anchor; }
    std::size_t length() const noexcept { return end() - begin(); }
    bool empty() const noexcept { return anchor == cursor; }
    static Selection at(std::size_t offset) noexcept { return {.anchor = offset, .cursor = offset}; }
    bool operator==(const Selection&) const = default;
  };

  // How an edit joins the undo history: runs of typing, and runs of deleting, undo as one step.
  enum class EditKind { Discrete, Typing, Backspace, Delete };

  enum class LineEnding { Lf, CrLf };

  // A position as a line and the bytes into it.
  struct TextPoint {
    std::size_t line = 0;
    std::size_t column = 0;
  };

  // One replacement in the buffer, told to whoever follows the text (the syntax tree): the bytes
  // [offset, oldEnd) became [offset, newEnd).
  struct TextChange {
    std::size_t offset = 0;
    std::size_t oldEnd = 0;
    std::size_t newEnd = 0;
    TextPoint start;
    TextPoint oldEndPoint;
    TextPoint newEndPoint;
    // The whole text was replaced; the other fields mean nothing.
    bool reset = false;
  };

  // One open text: the buffer and its undo history. Every change goes through edit() or a group.
  class Document {
  public:
    Document() = default;

    const Buffer& buffer() const noexcept { return m_buffer; }
    // Replaces the whole text and forgets the history; the result counts as saved.
    void setText(std::string text);

    // Replaces `length` bytes at `offset` with `text`. `before` and `after` are the selections
    // undo and redo restore.
    void edit(
        std::size_t offset, std::size_t length, std::string_view text, Selection before, Selection after,
        EditKind kind = EditKind::Discrete
    );
    // Several replacements that undo as one step. Offsets of each call are those at the time of the call.
    void beginGroup(Selection before);
    void replace(std::size_t offset, std::size_t length, std::string_view text);
    void endGroup(Selection after);
    // The next edit starts a new undo step even when it would continue a run.
    void breakRun() noexcept { m_sealed = true; }

    bool canUndo() const noexcept { return !m_undo.empty(); }
    bool canRedo() const noexcept { return !m_redo.empty(); }
    std::optional<Selection> undo();
    std::optional<Selection> redo();

    // Called for every change of the buffer, undo and redo included. One observer at a time.
    void setObserver(std::function<void(const TextChange&)> observer) { m_observer = std::move(observer); }

    bool modified() const noexcept { return !m_savedDepth || *m_savedDepth != m_undo.size(); }
    void markSaved();
    // The text differs from the file although the history says otherwise (a restored draft).
    void markModified() noexcept { m_savedDepth.reset(); }

  private:
    struct Change {
      std::size_t offset = 0;
      std::string removed;
      std::string inserted;
    };
    struct Step {
      std::vector<Change> changes;
      Selection before;
      Selection after;
      EditKind kind = EditKind::Discrete;
    };

    void apply(std::size_t offset, std::size_t length, std::string_view text, Step& step);
    // The one place the buffer changes: replaces and tells the observer.
    void mutate(std::size_t offset, std::size_t length, std::string_view text);
    TextPoint pointOf(std::size_t offset) const;
    bool continues(const Step& step, std::size_t offset, std::size_t length, std::string_view text, EditKind kind) const;

    Buffer m_buffer;
    std::function<void(const TextChange&)> m_observer;
    std::vector<Step> m_undo;
    std::vector<Step> m_redo;
    std::optional<Step> m_group;
    std::optional<std::size_t> m_savedDepth = 0;
    bool m_sealed = true;
  };
} // namespace plume

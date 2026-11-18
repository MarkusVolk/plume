#pragma once

#include "text/operations.h"
#include "ui/controls/flex.h"

#include <functional>
#include <string>

class Button;
class Input;
class InputArea;
class Label;

namespace plume {
  // The find and replace card that floats over the top right corner of the text.
  class SearchBar final : public Flex {
  public:
    struct Callbacks {
      // The pattern or an option changed.
      std::function<void()> changed;
      std::function<void(bool forward)> step;
      std::function<void()> replace;
      std::function<void()> replaceAll;
      std::function<void()> close;
    };

    explicit SearchBar(Callbacks callbacks);

    const std::string& query() const;
    const std::string& replacement() const;
    SearchOptions options() const noexcept { return m_options; }
    void setQuery(const std::string& query);
    void setReplaceVisible(bool visible);
    bool replaceVisible() const noexcept { return m_replaceVisible; }
    // "3 of 17", "No results" or the pattern's error; `invalid` marks the field.
    void setStatus(const std::string& status, bool invalid);
    InputArea* findArea() const;
    InputArea* replaceArea() const;
    // Selects the whole pattern so typing replaces it.
    void selectQuery();

  private:
    void toggle(bool SearchOptions::* flag, Button* button);

    Callbacks m_callbacks;
    SearchOptions m_options;
    Input* m_find = nullptr;
    Input* m_replace = nullptr;
    Label* m_status = nullptr;
    Flex* m_replaceRow = nullptr;
    Button* m_caseButton = nullptr;
    Button* m_wordButton = nullptr;
    Button* m_regexButton = nullptr;
    Button* m_replaceToggle = nullptr;
    bool m_replaceVisible = false;
  };
} // namespace plume

#pragma once

#include "ui/controls/flex.h"

#include <functional>
#include <string>
#include <vector>

class InputArea;

namespace plume {
  // A question over the window: a title, a line of explanation and a row of answers.
  class ConfirmDialog final : public Flex {
  public:
    struct Answer {
      std::string label;
      // The answer Enter gives.
      bool primary = false;
      bool destructive = false;
      std::function<void()> chosen;
    };

    ConfirmDialog(std::string title, std::string message, std::vector<Answer> answers);
    // The control that takes the keyboard when the dialog opens.
    InputArea* defaultArea() const noexcept { return m_default; }

  private:
    InputArea* m_default = nullptr;
  };
} // namespace plume

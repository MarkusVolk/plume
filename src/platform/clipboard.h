#pragma once

#include "core/text_clipboard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <wayland-client.h>

namespace plume {
  // Plain-text clipboard over the seat's data device: offers what was copied here and reads what
  // other clients offer without blocking the window.
  class WaylandClipboard final : public TextClipboard {
  public:
    // `serial` yields the input serial that authorises setting the selection, 0 when there is none.
    WaylandClipboard(
        wl_display* display, wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial
    );
    ~WaylandClipboard() override;
    WaylandClipboard(const WaylandClipboard&) = delete;
    WaylandClipboard& operator=(const WaylandClipboard&) = delete;

    std::optional<std::string> clipboardText() override;
    void setClipboardText(std::string text) override;
    void requestClipboardText(std::function<void(std::optional<std::string>)> callback) override;

  private:
    struct State;
    std::shared_ptr<State> m_state;
  };
} // namespace plume

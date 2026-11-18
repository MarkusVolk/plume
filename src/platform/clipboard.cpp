#include "platform/clipboard.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  constexpr std::array<std::string_view, 5> kTextTypes{
      "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"
  };
  // What a paste may bring in; a client that keeps writing past this is cut off.
  constexpr std::size_t kReadLimit = 256U * 1024U * 1024U;
} // namespace

struct plume::WaylandClipboard::State : std::enable_shared_from_this<State> {
  struct Read {
    std::weak_ptr<State> owner;
    int fd = -1;
    guint source = 0;
    std::string data;
    std::function<void(std::optional<std::string>)> callback;
  };

  wl_display* display = nullptr;
  wl_data_device_manager* manager = nullptr;
  wl_data_device* device = nullptr;
  std::function<std::uint32_t()> serial;
  // Every offer announced so far with the types it carries; `selected` is the current clipboard.
  std::map<wl_data_offer*, std::set<std::string>> offers;
  wl_data_offer* selected = nullptr;
  // Set while this process owns the selection.
  wl_data_source* source = nullptr;
  std::optional<std::string> text;
  std::set<Read*> reads;

  void discard(wl_data_offer* offer) {
    if (offer && offers.erase(offer) > 0) {
      wl_data_offer_destroy(offer);
    }
  }

  static void finish(Read* read, bool ok) {
    if (read->source) {
      g_source_remove(read->source);
    }
    close(read->fd);
    if (const auto owner = read->owner.lock()) {
      owner->reads.erase(read);
    }
    const auto callback = std::move(read->callback);
    auto data = ok ? std::optional(std::move(read->data)) : std::nullopt;
    delete read;
    if (callback) {
      callback(std::move(data));
    }
  }

  static gboolean readable(gint fd, GIOCondition, gpointer data) {
    auto* read = static_cast<Read*>(data);
    std::array<char, 65536> chunk;
    while (true) {
      const ssize_t count = ::read(fd, chunk.data(), chunk.size());
      if (count > 0) {
        read->data.append(chunk.data(), static_cast<std::size_t>(count));
        if (read->data.size() > kReadLimit) {
          read->source = 0;
          finish(read, false);
          return G_SOURCE_REMOVE;
        }
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return G_SOURCE_CONTINUE;
      }
      read->source = 0;
      finish(read, count == 0);
      return G_SOURCE_REMOVE;
    }
  }

  void receive(std::function<void(std::optional<std::string>)> callback) {
    const auto found = offers.find(selected);
    if (found == offers.end()) {
      callback(std::nullopt);
      return;
    }
    const std::string* type = nullptr;
    for (const auto candidate : kTextTypes) {
      if (const auto at = found->second.find(std::string(candidate)); at != found->second.end()) {
        type = &*at;
        break;
      }
    }
    std::array<int, 2> pipeFds{};
    if (!type || pipe2(pipeFds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
      callback(std::nullopt);
      return;
    }
    wl_data_offer_receive(selected, type->c_str(), pipeFds[1]);
    close(pipeFds[1]);
    wl_display_flush(display);
    auto* read = new Read{.owner = weak_from_this(), .fd = pipeFds[0], .source = 0, .data = {}, .callback = std::move(callback)};
    reads.insert(read);
    read->source = g_unix_fd_add(pipeFds[0], static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), &readable, read);
  }
};

plume::WaylandClipboard::WaylandClipboard(
    wl_display* display, wl_data_device_manager* manager, wl_seat* seat, std::function<std::uint32_t()> serial
)
    : m_state(std::make_shared<State>()) {
  auto& state = *m_state;
  state.display = display;
  state.manager = manager;
  state.serial = std::move(serial);
  if (!manager || !seat) {
    return;
  }
  state.device = wl_data_device_manager_get_data_device(manager, seat);
  static const wl_data_offer_listener offerListener{
      .offer = [](void* data, wl_data_offer* offer, const char* type) {
        auto& state = *static_cast<State*>(data);
        if (const auto found = state.offers.find(offer); found != state.offers.end()) {
          found->second.emplace(type);
        }
      },
      .source_actions = [](void*, wl_data_offer*, std::uint32_t) {},
      .action = [](void*, wl_data_offer*, std::uint32_t) {},
  };
  static const wl_data_device_listener listener{
      .data_offer = [](void* data, wl_data_device*, wl_data_offer* offer) {
        auto& state = *static_cast<State*>(data);
        state.offers.emplace(offer, std::set<std::string>{});
        wl_data_offer_add_listener(offer, &offerListener, data);
      },
      // Drops are not taken: the offer of a drag only has to be released.
      .enter = [](void* data, wl_data_device*, std::uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer) {
        static_cast<State*>(data)->discard(offer);
      },
      .leave = [](void*, wl_data_device*) {},
      .motion = [](void*, wl_data_device*, std::uint32_t, wl_fixed_t, wl_fixed_t) {},
      .drop = [](void*, wl_data_device*) {},
      .selection = [](void* data, wl_data_device*, wl_data_offer* offer) {
        auto& state = *static_cast<State*>(data);
        if (state.selected != offer) {
          state.discard(std::exchange(state.selected, offer));
        }
      },
  };
  wl_data_device_add_listener(state.device, &listener, &state);
}

plume::WaylandClipboard::~WaylandClipboard() {
  auto& state = *m_state;
  for (auto* read : std::set(state.reads)) {
    read->callback = nullptr;
    State::finish(read, false);
  }
  for (const auto& [offer, types] : state.offers) {
    (void)types;
    wl_data_offer_destroy(offer);
  }
  state.offers.clear();
  if (state.source) {
    wl_data_source_destroy(state.source);
  }
  if (state.device) {
    wl_data_device_release(state.device);
  }
}

std::optional<std::string> plume::WaylandClipboard::clipboardText() { return m_state->text; }

void plume::WaylandClipboard::setClipboardText(std::string text) {
  auto& state = *m_state;
  const std::uint32_t serial = state.serial ? state.serial() : 0;
  if (!state.device || serial == 0) {
    return;
  }
  if (state.source) {
    wl_data_source_destroy(state.source);
  }
  state.source = wl_data_device_manager_create_data_source(state.manager);
  static const wl_data_source_listener listener{
      .target = [](void*, wl_data_source*, const char*) {},
      .send = [](void* data, wl_data_source*, const char*, std::int32_t fd) {
        // A reader may take its time; writing on a thread keeps a full pipe from stalling the window.
        std::thread([copy = static_cast<State*>(data)->text.value_or(std::string()), fd] {
          std::size_t done = 0;
          while (done < copy.size()) {
            const ssize_t count = ::write(fd, copy.data() + done, copy.size() - done);
            if (count < 0 && errno == EINTR) {
              continue;
            }
            if (count <= 0) {
              break;
            }
            done += static_cast<std::size_t>(count);
          }
          close(fd);
        }).detach();
      },
      .cancelled = [](void* data, wl_data_source* source) {
        auto& state = *static_cast<State*>(data);
        if (state.source == source) {
          state.source = nullptr;
          state.text.reset();
        }
        wl_data_source_destroy(source);
      },
      .dnd_drop_performed = [](void*, wl_data_source*) {},
      .dnd_finished = [](void*, wl_data_source*) {},
      .action = [](void*, wl_data_source*, std::uint32_t) {},
  };
  wl_data_source_add_listener(state.source, &listener, &state);
  for (const auto type : kTextTypes) {
    wl_data_source_offer(state.source, std::string(type).c_str());
  }
  state.text = std::move(text);
  wl_data_device_set_selection(state.device, state.source, serial);
}

void plume::WaylandClipboard::requestClipboardText(std::function<void(std::optional<std::string>)> callback) {
  auto& state = *m_state;
  if (state.source) {
    callback(state.text);
    return;
  }
  state.receive(std::move(callback));
}

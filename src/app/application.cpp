#include "app/application.h"

#include "app/editor_window.h"
#include "app/main_loop.h"
#include "app/palette_watcher.h"
#include "core/application_identity.h"
#include "core/files/resource_paths.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_strings.h"
#include "event_loop/deferred_call_poll_source.h"
#include "event_loop/glib_poll_source.h"
#include "event_loop/timer_poll_source.h"
#include "platform/clipboard.h"
#include "platform/desktop_integration.h"
#include "plume_build_info.h"
#include "render/animation/animation_manager.h"
#include "render/gl_shared_context.h"
#include "render/render_context.h"
#include "ui/controls/input.h"
#include "ui/palette_file.h"
#include "ui/text_view.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <print>

namespace {
  volatile std::sig_atomic_t g_stopRequested = 0;
  void stopRequested(int) { g_stopRequested = 1; }

  class KeyRepeatPollSource final : public PollSource {
  public:
    explicit KeyRepeatPollSource(WaylandConnection& seat) : m_seat(seat) {}
    int pollTimeoutMs() const override { return m_seat.repeatPollTimeoutMs(); }
    void dispatch(const std::vector<pollfd>&, std::size_t) override { m_seat.repeatTick(); }

  private:
    void doAddPollFds(std::vector<pollfd>&) override {}
    WaylandConnection& m_seat;
  };

  // The chords the toolkit's own controls (inputs, menus, lists) ask for by action.
  void initializeKeys() {
    const std::array<std::pair<KeybindAction, std::uint32_t>, 7> keys{{
        {KeybindAction::Cancel, XKB_KEY_Escape},
        {KeybindAction::Left, XKB_KEY_Left},
        {KeybindAction::Right, XKB_KEY_Right},
        {KeybindAction::Up, XKB_KEY_Up},
        {KeybindAction::Down, XKB_KEY_Down},
        {KeybindAction::TabNext, XKB_KEY_Tab},
        {KeybindAction::Delete, XKB_KEY_Delete},
    }};
    for (const auto& [action, sym] : keys) {
      KeybindMatcher::setMatcher(action, [sym](std::uint32_t key, std::uint32_t modifiers) {
        return key == sym && modifiers == 0U;
      });
    }
    KeybindMatcher::setMatcher(KeybindAction::Save, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_s && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::Validate, [](std::uint32_t sym, std::uint32_t modifiers) {
      return modifiers == 0 && KeySymbol::isEnterOrSpace(sym);
    });
    KeybindMatcher::setMatcher(KeybindAction::Copy, [](std::uint32_t sym, std::uint32_t modifiers) {
      return sym == XKB_KEY_c && modifiers == KeyMod::Ctrl;
    });
    KeybindMatcher::setMatcher(KeybindAction::TabPrevious, [](std::uint32_t sym, std::uint32_t modifiers) {
      return KeySymbol::isTab(sym) && modifiers == KeyMod::Shift;
    });
  }

  void initializeLibrary() {
    setApplicationIdentity({.name = "plume", .envPrefix = "PLUME"});
    const char* overridePath = std::getenv("PLUME_ASSETS_DIR");
    const std::filesystem::path assets = overridePath ? overridePath : PLUME_INSTALL_ASSETSDIR;
    paths::setAssetRoots({.application = assets, .library = assets});
    setUiStrings({.keyName = {}, .selectPlaceholder = [] { return std::string("Select…"); }});
    setPalette(plume::defaultPalette());
  }
} // namespace

int plume::runApplication(const std::vector<std::string>& files) {
  initLogLevelFromEnvironment();
  initializeKeys();
  Input::setValidateKeyMatcher([](std::uint32_t sym, std::uint32_t modifiers) {
    return modifiers == 0 && KeySymbol::isEnter(sym);
  });
  g_stopRequested = 0;
  const auto previousTerm = std::signal(SIGTERM, stopRequested);
  const auto previousInt = std::signal(SIGINT, stopRequested);
  int result = EXIT_SUCCESS;
  try {
    initializeLibrary();
    GlibPollSource glib;
    TextInputService textInput;
    WaylandConnection connection;
    if (!connection.connect()) {
      throw std::runtime_error("could not connect to Wayland: " + connection.describeDisplayError());
    }
    connection.setTextInputService(&textInput);
    auto& desktop = connection.extension<DesktopIntegration>();
    WaylandClipboard clipboard(connection.display(), desktop.dataDeviceManager(), connection.seat(), [&connection] {
      return connection.lastKeyboardSurface() ? connection.lastInputSerial() : 0;
    });
    Input::setTextClipboard(&clipboard);
    TextView::setClipboard(&clipboard);
    struct ClipboardBinding {
      ~ClipboardBinding() {
        Input::setTextClipboard(nullptr);
        plume::TextView::setClipboard(nullptr);
      }
    } clipboardBinding;
    GlSharedContext graphics;
    graphics.initialize(connection.display());
    RenderContext renderer;
    renderer.initialize(graphics);
    PaletteWatcher palette;
    AnimationManager animations;

    EditorWindow window({.connection = connection, .renderer = renderer, .textInput = textInput, .animations = animations});
    for (const auto& file : files) {
      window.openFile(file);
    }
    if (files.empty()) {
      window.newTab();
    }

    connection.setPointerEventCallback([&window, &desktop](const PointerEvent& event) {
      desktop.pointer(event);
      if (window.owns(event.surface)) {
        window.pointer(event);
      }
    });
    connection.setKeyboardEventCallback([&window](const KeyboardEvent& event) {
      if (event.pressed && !event.preedit && event.modifiers == KeyMod::Ctrl && event.sym == XKB_KEY_q) {
        g_stopRequested = 1;
        return;
      }
      window.keyboard(event);
    });
    connection.setKeyboardFocusCallback([&window](wl_surface* surface, bool entered) {
      if (window.owns(surface)) {
        window.focus(entered);
      }
    });

    DeferredCallPollSource deferred;
    TimerPollSource timers;
    KeyRepeatPollSource repeat(connection);
    MainLoop loop(connection, {&deferred, &timers, &repeat, &glib}, [&window] {
      if (g_stopRequested && window.running()) {
        window.close();
      }
      return window.running();
    });
    loop.run();
    connection.setPointerEventCallback({});
    connection.setKeyboardEventCallback({});
    connection.setKeyboardFocusCallback({});
    textInput.cleanup();
    connection.setTextInputService(nullptr);
  } catch (const std::exception& error) {
    std::println(stderr, "plume: {}", error.what());
    result = EXIT_FAILURE;
  }
  std::signal(SIGTERM, previousTerm);
  std::signal(SIGINT, previousInt);
  return result;
}

#pragma once

#include "document/file_io.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "text/document.h"
#include "ui/text_view.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AnimationManager;
class Button;
class Flex;
class Label;
class RenderContext;
class ScrollView;
class TextInputService;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace plume {
  struct Services {
    WaylandConnection& connection;
    RenderContext& renderer;
    TextInputService& textInput;
    AnimationManager& animations;
  };

  // One window: a strip of tabs, the text of the selected one and a status bar.
  class EditorWindow {
  public:
    explicit EditorWindow(Services services);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Opens the file in a new tab, or selects the tab that already shows it.
    void openFile(const std::filesystem::path& path);
    void newTab();
    bool running() const { return m_surface.isRunning(); }
    void close() { m_surface.close(); }
    bool owns(wl_surface* surface) const { return m_surface.wlSurface() == surface; }

    void keyboard(const KeyboardEvent& event);
    void pointer(const PointerEvent& event);
    void focus(bool entered);

  private:
    struct Tab {
      std::uint64_t id = 0;
      std::unique_ptr<Document> document;
      std::filesystem::path path;
      FileFormat format;
      // Owned here while the tab is not shown, by the scene while it is.
      std::unique_ptr<TextView> detached;
      TextView* view = nullptr;
      std::string title() const;
    };

    void prepare();
    void build();
    void rebuildStrip();
    void mount(std::size_t index);
    void select(std::size_t index);
    void closeTab(std::size_t index);
    Tab& addTab();
    Tab* active() { return m_active < m_tabs.size() ? m_tabs[m_active].get() : nullptr; }
    void save();
    void zoom(float delta);
    void applyOptions();
    void status(std::string message);
    void requestUpdate();

    Services m_services;
    WindowSurface m_surface;
    InputDispatcher m_dispatcher;
    std::unique_ptr<Node> m_root;
    Flex* m_strip = nullptr;
    ScrollView* m_tabScroll = nullptr;
    Flex* m_statusBar = nullptr;
    Label* m_statusText = nullptr;
    Label* m_positionText = nullptr;
    Label* m_formatText = nullptr;
    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::size_t m_active = 0;
    std::size_t m_mounted = static_cast<std::size_t>(-1);
    std::uint64_t m_nextTab = 1;
    std::vector<std::string> m_stripLabels;
    std::size_t m_stripActive = static_cast<std::size_t>(-1);
    bool m_stripDecorated = false;
    TextViewOptions m_options;
    std::string m_status;
    bool m_focusPending = true;
  };
} // namespace plume

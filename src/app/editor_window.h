#pragma once

#include "app/settings.h"
#include "core/timer_manager.h"
#include "document/file_io.h"
#include "document/session.h"
#include "platform/desktop_integration.h"
#include "render/scene/input_dispatcher.h"
#include "text/document.h"
#include "text/operations.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/text_view.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class AnimationManager;
class Button;
class Flex;
class Label;
class RenderContext;
class ScrollView;
class TextInputService;
class TooltipPresenter;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;

namespace plume {
  class ConfirmDialog;
  class FilePortal;
  class SearchBar;

  struct Services {
    WaylandConnection& connection;
    RenderContext& renderer;
    TextInputService& textInput;
    AnimationManager& animations;
    FilePortal& portal;
    SettingsStore& settings;
  };

  // The window: a strip of tabs, the text of the selected one, a search card and a status bar.
  class EditorWindow {
  public:
    EditorWindow(Services services, const Session& session);
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Opens the file in a new tab, or selects the tab that already shows it.
    void openFile(const std::filesystem::path& path);
    void newTab();
    // Runs a named action ("find", "sort-lines", ...); false when there is none of that name.
    bool action(const std::string& name, const std::string& parameter);
    bool hasTabs() const noexcept { return !m_tabs.empty(); }
    bool running() const { return m_surface.isRunning(); }
    void close() { m_surface.close(); }
    wl_surface* surface() const { return m_surface.wlSurface(); }
    // Writes the drafts of unsaved texts and the list of tabs, so the next start brings them back.
    void saveSession(bool force);
    void setOnQuit(std::function<void()> callback) { m_onQuit = std::move(callback); }

    void keyboard(const KeyboardEvent& event);
    void pointer(const PointerEvent& event);
    void modifiers(std::uint32_t modifiers);
    void focus(wl_surface* surface, bool entered);

  private:
    struct DiskStamp {
      std::filesystem::file_time_type modified;
      std::uintmax_t size = 0;
      bool operator==(const DiskStamp&) const = default;
    };
    struct Tab {
      std::uint64_t id = 0;
      std::unique_ptr<Document> document;
      std::filesystem::path path;
      FileFormat format;
      // Owned here while the tab is not shown, by the scene while it is.
      std::unique_ptr<TextView> detached;
      TextView* view = nullptr;
      // The file as it was when loaded or saved, to notice changes made elsewhere.
      std::optional<DiskStamp> stamp;
      // Where the unsaved text is kept, and the buffer revision that was written there.
      std::string draft;
      std::optional<std::size_t> draftRevision;
      std::string title() const;
    };
    enum class Command : std::int32_t;

    void prepare();
    void build();
    void rebuildStrip();
    void mount(std::size_t index);
    void select(std::size_t index);
    void requestClose(std::size_t index);
    void closeTab(std::size_t index);
    Tab& addTab();
    Tab* active() { return m_active < m_tabs.size() ? m_tabs[m_active].get() : nullptr; }
    std::size_t indexOf(std::uint64_t id) const;
    bool load(Tab& tab);
    void save(std::uint64_t id, std::function<void()> then = {});
    void saveAs(std::uint64_t id, std::function<void()> then = {});
    bool write(Tab& tab);
    void open();
    void checkDisk();
    static std::optional<DiskStamp> stampOf(const std::filesystem::path& path);

    void run(Command command);
    void showMainMenu();
    void showTextMenu(float x, float y);
    void openMenu(std::vector<ContextMenuControlEntry> entries, PopupAnchorRect anchor);
    void showDialog(std::unique_ptr<ConfirmDialog> dialog);
    void closeDialog();

    void showSearch(bool replace);
    void hideSearch();
    void updateSearch();
    void stepSearch(bool forward);
    void replaceCurrent();
    void replaceAll();

    void zoom(float delta);
    void applyOptions();
    void status(std::string message);
    void requestUpdate();
    void focusText();

    Services m_services;
    WindowSurface m_surface;
    InputDispatcher m_dispatcher;
    ContextMenuPopup m_menu;
    std::unique_ptr<Node> m_root;
    Flex* m_strip = nullptr;
    ScrollView* m_tabScroll = nullptr;
    Button* m_menuButton = nullptr;
    Flex* m_statusBar = nullptr;
    Label* m_statusText = nullptr;
    Label* m_positionText = nullptr;
    Label* m_formatText = nullptr;
    TooltipPresenter* m_tooltip = nullptr;
    SearchBar* m_search = nullptr;
    InputArea* m_backdrop = nullptr;
    ConfirmDialog* m_dialog = nullptr;

    std::vector<std::unique_ptr<Tab>> m_tabs;
    std::size_t m_active = 0;
    std::size_t m_mounted = static_cast<std::size_t>(-1);
    std::uint64_t m_nextTab = 1;
    std::vector<std::string> m_stripLabels;
    std::size_t m_stripActive = static_cast<std::size_t>(-1);
    bool m_stripDecorated = false;
    Settings m_settings;
    float m_defaultFontSize = 13.0F;
    std::string m_status;
    bool m_focusPending = true;
    float m_pointerX = 0.0F;
    float m_pointerY = 0.0F;
    std::uint32_t m_modifiers = 0;

    // The matches of the search card in the selected tab, valid for one revision of its text.
    std::optional<Pattern> m_pattern;
    std::string m_searchText;
    std::vector<Match> m_matches;
    std::optional<std::size_t> m_currentMatch;
    std::uint64_t m_searchTab = 0;
    std::size_t m_searchRevision = static_cast<std::size_t>(-1);
    bool m_searchStale = true;

    Timer m_autosave;
    Session m_savedSession;
    std::function<void()> m_onQuit;
    std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  };
} // namespace plume

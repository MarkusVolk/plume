#include "app/editor_window.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/ui_phase.h"
#include "platform/file_portal.h"
#include "render/render_context.h"
#include "text/unicode.h"
#include "ui/builders.h"
#include "ui/confirm_dialog.h"
#include "ui/search_bar.h"
#include "ui/style.h"
#include "ui/tooltip_presenter.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <linux/input-event-codes.h>
#include <stdexcept>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr float kStripHeight = Style::controlHeightSm + 2.0F * Style::spaceXs;
  constexpr float kStatusHeight = 28.0F;
  constexpr float kSearchWidth = 440.0F;
  constexpr auto kAutosaveInterval = std::chrono::seconds(3);
  constexpr auto kStatusTime = std::chrono::seconds(6);
  constexpr auto kMenuReopenGuard = std::chrono::milliseconds(250);
  // Drafts of texts larger than this are written less often.
  constexpr std::size_t kLargeDraft = 4U * 1024U * 1024U;

  std::uint32_t lower(std::uint32_t sym) { return sym >= XKB_KEY_A && sym <= XKB_KEY_Z ? sym + 32 : sym; }

  std::filesystem::path homeFolder() {
    const char* home = std::getenv("HOME");
    return home ? std::filesystem::path(home) : std::filesystem::path("/");
  }
} // namespace

enum class plume::EditorWindow::Command : std::int32_t {
  NewTab = 1,
  Open,
  Save,
  SaveAs,
  CloseTab,
  Quit,
  Undo,
  Redo,
  Cut,
  Copy,
  Paste,
  SelectAll,
  Find,
  Replace,
  SortAscending,
  SortDescending,
  SortUnique,
  LineNumbers,
  Wrap,
  HighlightLine,
  Syntax,
  InsertSpaces,
  TabWidth2,
  TabWidth4,
  TabWidth8,
  ZoomIn,
  ZoomOut,
  ZoomReset,
};

std::string plume::EditorWindow::Tab::title() const {
  std::string name = path.empty() ? std::string("Untitled") : path.filename().string();
  return document && document->modified() ? "• " + name : name;
}

plume::EditorWindow::EditorWindow(Services services, const Session& session)
    : m_services(services), m_surface(services.connection), m_menu(services.connection, services.renderer),
      m_settings(services.settings.settings()), m_defaultFontSize(Settings{}.editor.fontSize) {
  m_surface.setRenderContext(&m_services.renderer);
  m_surface.setAnimationManager(&m_services.animations);
  m_surface.setPrepareFrameCallback([this](bool, bool) { prepare(); });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) {
    if (m_tooltip) {
      m_tooltip->dismiss();
    }
    m_surface.requestLayout();
  });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.connection.extension<DesktopIntegration>().setCursorShape(serial, shape);
  });
  m_menu.setOnActivate([this](const ContextMenuControlEntry& entry) { run(static_cast<Command>(entry.id)); });
  m_menu.setOnDismissed([this] {
    m_menuClosedAt = std::chrono::steady_clock::now();
    requestUpdate();
  });
  if (!m_surface.initialize(
          nullptr,
          {.width = session.width >= 360 ? session.width : 960U,
           .height = session.height >= 240 ? session.height : 720U,
           .minWidth = 360,
           .minHeight = 240,
           .title = "Plume",
           .appId = "de.flk.Plume"}
      )) {
    throw std::runtime_error("cannot create the window");
  }
  m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);

  // Bring back the tabs of the last run: the file as it is on disk, or the draft of its unsaved text.
  for (const SessionTab& saved : session.tabs) {
    Tab& tab = addTab();
    tab.path = saved.path;
    std::error_code error;
    bool restored = !tab.path.empty() && std::filesystem::exists(tab.path, error) && load(tab);
    if (!saved.draft.empty()) {
      if (auto text = readDraft(saved.draft)) {
        tab.document->setText(std::move(*text));
        tab.document->markModified();
        tab.draft = saved.draft;
        tab.draftRevision = tab.document->buffer().revision();
        tab.view->documentReplaced();
        attachSyntax(tab);
        restored = true;
      }
    }
    if (!restored) {
      m_tabs.pop_back();
      continue;
    }
    tab.view->setSelection({.anchor = saved.anchor, .cursor = saved.cursor}, false);
    tab.view->scrollToLine(saved.topLine);
    if (&saved - session.tabs.data() == static_cast<std::ptrdiff_t>(session.active)) {
      m_active = m_tabs.size() - 1;
    }
  }
  m_status.clear();
  m_savedSession = session;
  m_autosave.startRepeating(kAutosaveInterval, [this] { saveSession(false); });
}

plume::EditorWindow::~EditorWindow() = default;

void plume::EditorWindow::requestUpdate() { m_surface.requestLayout(); }

void plume::EditorWindow::status(std::string message) {
  m_status = std::move(message);
  m_statusTimer.start(kStatusTime, [this] {
    m_status.clear();
    requestUpdate();
  });
  requestUpdate();
}

std::size_t plume::EditorWindow::indexOf(std::uint64_t id) const {
  for (std::size_t i = 0; i < m_tabs.size(); ++i) {
    if (m_tabs[i]->id == id) {
      return i;
    }
  }
  return m_tabs.size();
}

plume::EditorWindow::Tab& plume::EditorWindow::addTab() {
  auto tab = std::make_unique<Tab>();
  tab->id = m_nextTab++;
  tab->document = std::make_unique<Document>();
  tab->detached = std::make_unique<TextView>();
  tab->view = tab->detached.get();
  tab->view->setParticipatesInLayout(false);
  tab->view->setOptions(m_settings.editor);
  tab->view->setDocument(tab->document.get());
  tab->view->setModifiers(m_modifiers);
  tab->view->setOnChange([this] { requestUpdate(); });
  tab->view->setOnCursorMoved([this] { requestUpdate(); });
  tab->view->setOnContextMenu([this](float x, float y) { showTextMenu(x, y); });
  m_tabs.push_back(std::move(tab));
  return *m_tabs.back();
}

void plume::EditorWindow::newTab() {
  addTab();
  select(m_tabs.size() - 1);
}

std::optional<plume::EditorWindow::DiskStamp> plume::EditorWindow::stampOf(const std::filesystem::path& path) {
  std::error_code error;
  DiskStamp stamp;
  stamp.modified = std::filesystem::last_write_time(path, error);
  if (error) {
    return std::nullopt;
  }
  stamp.size = std::filesystem::file_size(path, error);
  return error ? std::nullopt : std::optional(stamp);
}

void plume::EditorWindow::attachSyntax(Tab& tab) {
  const Buffer& buffer = tab.document->buffer();
  std::string language;
  if (m_settings.syntaxHighlighting && !tab.readOnly) {
    language = detectLanguage(tab.path, buffer.text(0, std::min<std::size_t>(buffer.lineLength(0), 200)));
  }
  if (language == tab.language && (tab.syntax != nullptr || language.empty())) {
    return;
  }
  tab.view->setSyntax(nullptr);
  tab.document->setObserver({});
  tab.syntax = language.empty() ? nullptr : Highlighter::create(language, buffer);
  tab.language = tab.syntax ? language : std::string();
  if (tab.syntax) {
    tab.document->setObserver([syntax = tab.syntax.get()](const TextChange& change) { syntax->edited(change); });
    tab.view->setSyntax(tab.syntax.get());
  }
}

bool plume::EditorWindow::load(Tab& tab) {
  auto loaded = loadFile(tab.path);
  if (!loaded) {
    status(std::format("{}: {}", tab.path.filename().string(), loaded.error()));
    return false;
  }
  tab.format = loaded->format;
  tab.readOnly = loaded->binary;
  tab.view->setReadOnly(tab.readOnly);
  tab.document->setText(std::move(loaded->text));
  tab.view->documentReplaced();
  tab.stamp = stampOf(tab.path);
  attachSyntax(tab);
  return true;
}

void plume::EditorWindow::openFile(const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
  for (std::size_t i = 0; i < m_tabs.size(); ++i) {
    if (m_tabs[i]->path == absolute) {
      select(i);
      return;
    }
  }
  if (std::filesystem::is_directory(absolute, error)) {
    status(std::format("{} is a folder", absolute.string()));
    return;
  }
  // A lone untouched empty tab is replaced instead of left behind.
  const bool reuse = m_tabs.size() == 1
      && m_tabs[0]->path.empty()
      && !m_tabs[0]->document->modified()
      && m_tabs[0]->document->buffer().empty();
  Tab& tab = reuse ? *m_tabs[0] : addTab();
  tab.path = absolute;
  // A name that does not exist yet opens empty and is created on save.
  if (std::filesystem::exists(absolute, error) && !load(tab)) {
    if (!reuse) {
      m_tabs.pop_back();
    } else {
      tab.path.clear();
    }
    return;
  }
  attachSyntax(tab);
  select(reuse ? 0 : m_tabs.size() - 1);
}

void plume::EditorWindow::select(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  m_active = index;
  m_focusPending = true;
  m_searchStale = true;
  requestUpdate();
}

void plume::EditorWindow::focusText() {
  m_focusPending = true;
  requestUpdate();
}

void plume::EditorWindow::requestClose(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  Tab& tab = *m_tabs[index];
  // A text that was never saved and is empty has nothing to lose.
  if (!tab.document->modified() || (tab.path.empty() && tab.document->buffer().empty())) {
    closeTab(index);
    return;
  }
  select(index);
  const std::uint64_t id = tab.id;
  const std::string name = tab.path.empty() ? std::string("Untitled") : tab.path.filename().string();
  showDialog(
      std::make_unique<ConfirmDialog>(
          std::format("Save changes to “{}”?", name), "Changes that are not saved are lost when the tab closes.",
          std::vector<ConfirmDialog::Answer>{
              {.label = "Cancel", .chosen = [this] { closeDialog(); }},
              {.label = "Discard",
               .destructive = true,
               .chosen =
                   [this, id] {
                     closeDialog();
                     closeTab(indexOf(id));
                   }},
              {.label = "Save",
               .primary = true,
               .chosen =
                   [this, id] {
                     closeDialog();
                     save(id, [this, id] { closeTab(indexOf(id)); });
                   }},
          }
      )
  );
}

void plume::EditorWindow::closeTab(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  Tab& tab = *m_tabs[index];
  if (!tab.draft.empty()) {
    removeDraft(tab.draft);
  }
  if (m_mounted == index) {
    tab.detached.reset(static_cast<TextView*>(m_root->removeChild(tab.view).release()));
  }
  m_mounted = static_cast<std::size_t>(-1);
  // The view may be the one handling the event that closes it: it goes after this turn of the loop.
  DeferredCall::callLater([doomed = std::shared_ptr<Tab>(std::move(m_tabs[index]))] {});
  m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (m_tabs.empty()) {
    // Nothing left to restore: the next start begins with an empty window.
    saveSession(true);
    m_surface.close();
    return;
  }
  if (index < m_active || m_active >= m_tabs.size()) {
    m_active = m_active == 0 ? 0 : m_active - 1;
  }
  m_focusPending = true;
  m_searchStale = true;
  requestUpdate();
}

bool plume::EditorWindow::write(Tab& tab) {
  const auto result = saveFile(tab.path, tab.document->buffer().text(), tab.format);
  if (!result) {
    status(std::format("{}: {}", tab.path.filename().string(), result.error()));
    return false;
  }
  tab.document->markSaved();
  tab.stamp = stampOf(tab.path);
  status(std::format("Saved {}", tab.path.string()));
  return true;
}

void plume::EditorWindow::save(std::uint64_t id, std::function<void()> then) {
  const std::size_t index = indexOf(id);
  if (index >= m_tabs.size()) {
    return;
  }
  Tab& tab = *m_tabs[index];
  if (tab.readOnly) {
    status(std::format("{} is binary data and shown read-only", tab.path.filename().string()));
    return;
  }
  if (tab.path.empty()) {
    saveAs(id, std::move(then));
    return;
  }
  if (write(tab) && then) {
    then();
  }
}

void plume::EditorWindow::saveAs(std::uint64_t id, std::function<void()> then) {
  const std::size_t index = indexOf(id);
  if (index >= m_tabs.size()) {
    return;
  }
  if (!m_services.portal.available()) {
    status("No file dialog is available: xdg-desktop-portal is not running");
    return;
  }
  const Tab& tab = *m_tabs[index];
  if (tab.readOnly) {
    status(std::format("{} is binary data and shown read-only", tab.path.filename().string()));
    return;
  }
  const auto folder = tab.path.empty() ? homeFolder() : tab.path.parent_path();
  const std::string name = tab.path.empty() ? std::string("Untitled.txt") : tab.path.filename().string();
  m_services.portal.saveFile(
      m_surface.portalHandle(), folder, name,
      [this, id, then = std::move(then), alive = std::weak_ptr<int>(m_alive)](std::vector<std::filesystem::path> paths) {
        if (alive.expired() || paths.empty()) {
          return;
        }
        const std::size_t at = indexOf(id);
        if (at >= m_tabs.size()) {
          return;
        }
        Tab& target = *m_tabs[at];
        target.path = paths.front().lexically_normal();
        attachSyntax(target);
        if (write(target) && then) {
          then();
        }
      }
  );
}

void plume::EditorWindow::open() {
  if (!m_services.portal.available()) {
    status("No file dialog is available: xdg-desktop-portal is not running");
    return;
  }
  const Tab* tab = active();
  const auto folder = tab && !tab->path.empty() ? tab->path.parent_path() : homeFolder();
  m_services.portal.openFiles(
      m_surface.portalHandle(), folder,
      [this, alive = std::weak_ptr<int>(m_alive)](std::vector<std::filesystem::path> paths) {
        if (alive.expired()) {
          return;
        }
        for (const auto& path : paths) {
          openFile(path);
        }
      }
  );
}

void plume::EditorWindow::checkDisk() {
  for (const auto& tab : m_tabs) {
    if (tab->path.empty() || !tab->stamp) {
      continue;
    }
    const auto now = stampOf(tab->path);
    if (now == tab->stamp) {
      continue;
    }
    const std::string name = tab->path.filename().string();
    if (!now) {
      // The text here is now the only copy.
      tab->stamp.reset();
      tab->document->markModified();
      status(std::format("{} was removed on disk", name));
    } else if (!tab->document->modified()) {
      const Selection selection = tab->view->selection();
      const std::size_t top = tab->view->topLine();
      if (load(*tab)) {
        tab->view->setSelection(selection, false);
        tab->view->scrollToLine(top);
        status(std::format("{} changed on disk and was reloaded", name));
      }
    } else {
      tab->stamp = now;
      status(std::format("{} changed on disk; saving overwrites it", name));
    }
    m_searchStale = true;
  }
}

void plume::EditorWindow::saveSession(bool force) {
  static unsigned tick = 0;
  ++tick;
  Session session;
  session.width = m_surface.width();
  session.height = m_surface.height();
  for (std::size_t index = 0; index < m_tabs.size(); ++index) {
    Tab& tab = *m_tabs[index];
    const Buffer& buffer = tab.document->buffer();
    const bool modified = tab.document->modified() && !(tab.path.empty() && buffer.empty());
    if (modified) {
      if (tab.draft.empty()) {
        tab.draft = newDraftName();
      }
      const bool due = force || buffer.size() < kLargeDraft || tick % 10 == 0;
      if (tab.draftRevision != buffer.revision() && due && writeDraft(tab.draft, buffer.text())) {
        tab.draftRevision = buffer.revision();
      }
    } else if (!tab.draft.empty()) {
      removeDraft(tab.draft);
      tab.draft.clear();
      tab.draftRevision.reset();
    }
    if (tab.path.empty() && !modified) {
      continue;
    }
    const Selection selection = tab.view->selection();
    session.tabs.push_back(
        {.path = tab.path.string(),
         .draft = modified ? tab.draft : std::string(),
         .cursor = selection.cursor,
         .anchor = selection.anchor,
         .topLine = tab.view->topLine()}
    );
    if (index == m_active) {
      session.active = session.tabs.size() - 1;
    }
  }
  if (force || session != m_savedSession) {
    plume::saveSession(session);
    m_savedSession = session;
  }
  if (force) {
    pruneDrafts(session);
  }
}

void plume::EditorWindow::applyOptions() {
  for (const auto& tab : m_tabs) {
    tab->view->setOptions(m_settings.editor);
  }
  m_services.settings.save(m_settings);
  requestUpdate();
}

void plume::EditorWindow::zoom(float delta) {
  auto& size = m_settings.editor.fontSize;
  size = delta == 0.0F ? m_defaultFontSize : std::clamp(size + delta, 6.0F, 48.0F);
  applyOptions();
  status(std::format("Font size {:.0f}", size));
}

// ── Commands and menus ───────────────────────────────────────────────────────

void plume::EditorWindow::run(Command command) {
  Tab* tab = active();
  auto& editor = m_settings.editor;
  switch (command) {
  case Command::NewTab:
    newTab();
    break;
  case Command::Open:
    open();
    break;
  case Command::Save:
    if (tab) {
      save(tab->id);
    }
    break;
  case Command::SaveAs:
    if (tab) {
      saveAs(tab->id);
    }
    break;
  case Command::CloseTab:
    requestClose(m_active);
    break;
  case Command::Quit:
    if (m_onQuit) {
      m_onQuit();
    }
    break;
  case Command::Find:
    showSearch(false);
    break;
  case Command::Replace:
    showSearch(true);
    break;
  case Command::LineNumbers:
    editor.lineNumbers = !editor.lineNumbers;
    applyOptions();
    break;
  case Command::Wrap:
    editor.wrap = !editor.wrap;
    applyOptions();
    break;
  case Command::HighlightLine:
    editor.highlightCurrentLine = !editor.highlightCurrentLine;
    applyOptions();
    break;
  case Command::Syntax:
    m_settings.syntaxHighlighting = !m_settings.syntaxHighlighting;
    for (const auto& each : m_tabs) {
      attachSyntax(*each);
    }
    applyOptions();
    break;
  case Command::InsertSpaces:
    editor.insertSpaces = !editor.insertSpaces;
    applyOptions();
    break;
  case Command::TabWidth2:
  case Command::TabWidth4:
  case Command::TabWidth8:
    editor.tabWidth = command == Command::TabWidth2 ? 2 : command == Command::TabWidth4 ? 4 : 8;
    applyOptions();
    break;
  case Command::ZoomIn:
    zoom(1.0F);
    break;
  case Command::ZoomOut:
    zoom(-1.0F);
    break;
  case Command::ZoomReset:
    zoom(0.0F);
    break;
  default:
    if (!tab) {
      break;
    }
    switch (command) {
    case Command::Undo:
      tab->view->undo();
      break;
    case Command::Redo:
      tab->view->redo();
      break;
    case Command::Cut:
      tab->view->cut();
      break;
    case Command::Copy:
      tab->view->copy();
      break;
    case Command::Paste:
      tab->view->paste();
      break;
    case Command::SelectAll:
      tab->view->selectAll();
      break;
    case Command::SortAscending:
      tab->view->sortLines();
      break;
    case Command::SortDescending:
      tab->view->sortLines({.descending = true});
      break;
    case Command::SortUnique:
      tab->view->sortLines({.unique = true});
      break;
    default:
      break;
    }
    focusText();
    break;
  }
  requestUpdate();
}

bool plume::EditorWindow::action(const std::string& name, const std::string& parameter) {
  static const std::pair<const char*, Command> kActions[] = {
      {"new-tab", Command::NewTab},
      {"open", Command::Open},
      {"save", Command::Save},
      {"save-as", Command::SaveAs},
      {"close-tab", Command::CloseTab},
      {"quit", Command::Quit},
      {"undo", Command::Undo},
      {"redo", Command::Redo},
      {"cut", Command::Cut},
      {"copy", Command::Copy},
      {"paste", Command::Paste},
      {"select-all", Command::SelectAll},
      {"find", Command::Find},
      {"replace", Command::Replace},
      {"sort-lines", Command::SortAscending},
      {"sort-lines-descending", Command::SortDescending},
      {"sort-lines-unique", Command::SortUnique},
      {"line-numbers", Command::LineNumbers},
      {"wrap", Command::Wrap},
      {"highlight-line", Command::HighlightLine},
      {"syntax-highlighting", Command::Syntax},
      {"zoom-in", Command::ZoomIn},
      {"zoom-out", Command::ZoomOut},
      {"zoom-reset", Command::ZoomReset},
  };
  if (m_menu.isOpen()) {
    m_menu.close();
  }
  closeDialog();
  Tab* tab = active();
  if (name == "menu") {
    showMainMenu();
    return true;
  }
  if (name == "insert-text" && tab) {
    tab->view->insertText(parameter);
    return true;
  }
  if (name == "go-to-line" && tab) {
    const auto line = static_cast<std::size_t>(std::max(1L, std::atol(parameter.c_str()))) - 1;
    const Buffer& buffer = tab->document->buffer();
    tab->view->setSelection(Selection::at(buffer.lineStart(std::min(line, buffer.lineCount() - 1))));
    tab->view->centerCursor();
    return true;
  }
  if (name == "select-lines" && tab) {
    // "3-7": from the start of line 3 into line 7.
    const Buffer& buffer = tab->document->buffer();
    const std::size_t dash = parameter.find('-');
    const auto line = [&buffer](const std::string& text) {
      const auto number = static_cast<std::size_t>(std::max(1L, std::atol(text.c_str()))) - 1;
      return std::min(number, buffer.lineCount() - 1);
    };
    const std::size_t first = line(parameter.substr(0, dash));
    const std::size_t last = dash == std::string::npos ? first : line(parameter.substr(dash + 1));
    tab->view->setSelection({.anchor = buffer.lineStart(first), .cursor = buffer.lineEnd(last)});
    return true;
  }
  for (const auto& [candidate, command] : kActions) {
    if (name == candidate) {
      run(command);
      // "find" and "replace" take the pattern to look for.
      if ((command == Command::Find || command == Command::Replace) && !parameter.empty() && m_search) {
        m_search->setQuery(parameter);
        m_searchStale = true;
        stepSearch(true);
      }
      return true;
    }
  }
  return false;
}

void plume::EditorWindow::openMenu(std::vector<ContextMenuControlEntry> entries, PopupAnchorRect anchor) {
  if (m_tooltip) {
    m_tooltip->dismiss();
  }
  m_dispatcher.cancelPointerCapture();
  m_menu.open(
      {.entries = std::move(entries),
       .minMenuWidth = 220.0F,
       .anchor = anchor,
       .parent = {.attach = {}, .xdgSurface = m_surface.xdgSurface(), .output = nullptr, .keyboardGrab = {}},
       .pointerParentSurface = m_surface.wlSurface(),
       .inputSerial = m_services.connection.lastInputSerial()}
  );
}

void plume::EditorWindow::showMainMenu() {
  const auto& editor = m_settings.editor;
  const auto id = [](Command command) { return static_cast<std::int32_t>(command); };
  const auto check = [&id](Command command, std::string label, bool on, std::optional<std::string> shortcut = {}) {
    return ContextMenuControlEntry{
        .id = id(command), .label = std::move(label), .checkmark = true, .toggleState = on ? 1 : 0, .shortcut = std::move(shortcut)
    };
  };
  const auto radio = [&id](Command command, std::string label, bool on) {
    return ContextMenuControlEntry{.id = id(command), .label = std::move(label), .radio = true, .toggleState = on ? 1 : 0};
  };
  std::vector<ContextMenuControlEntry> entries{
      {.id = id(Command::NewTab), .label = "New Tab", .shortcut = "Ctrl+T"},
      {.id = id(Command::Open), .label = "Open…", .shortcut = "Ctrl+O"},
      {.id = id(Command::Save), .label = "Save", .shortcut = "Ctrl+S"},
      {.id = id(Command::SaveAs), .label = "Save As…", .shortcut = "Ctrl+Shift+S"},
      {.label = "", .separator = true},
      {.id = id(Command::Find), .label = "Find…", .shortcut = "Ctrl+F"},
      {.id = id(Command::Replace), .label = "Find and Replace…", .shortcut = "Ctrl+H"},
      {.label = "", .separator = true},
      {.id = id(Command::SortAscending), .label = "Sort Lines", .shortcut = "F9"},
      {.id = id(Command::SortDescending), .label = "Sort Lines Descending", .shortcut = "Shift+F9"},
      {.id = id(Command::SortUnique), .label = "Sort Lines, Remove Duplicates"},
      {.label = "", .separator = true},
      check(Command::LineNumbers, "Line Numbers", editor.lineNumbers, "F7"),
      check(Command::Wrap, "Wrap Lines", editor.wrap, "F8"),
      check(Command::HighlightLine, "Highlight Current Line", editor.highlightCurrentLine),
      check(Command::Syntax, "Syntax Highlighting", m_settings.syntaxHighlighting),
      {.id = id(Command::ZoomIn), .label = "Zoom In", .shortcut = "Ctrl++"},
      {.id = id(Command::ZoomOut), .label = "Zoom Out", .shortcut = "Ctrl+-"},
      {.id = id(Command::ZoomReset), .label = "Reset Zoom", .shortcut = "Ctrl+0"},
      {.label = "", .separator = true},
      check(Command::InsertSpaces, "Indent with Spaces", editor.insertSpaces),
      radio(Command::TabWidth2, "Tab Width 2", editor.tabWidth == 2),
      radio(Command::TabWidth4, "Tab Width 4", editor.tabWidth == 4),
      radio(Command::TabWidth8, "Tab Width 8", editor.tabWidth == 8),
      {.label = "", .separator = true},
      {.id = id(Command::CloseTab), .label = "Close Tab", .shortcut = "Ctrl+W"},
      {.id = id(Command::Quit), .label = "Quit", .shortcut = "Ctrl+Q"},
  };
  float x = 0.0F, y = 0.0F;
  Node::absolutePosition(m_menuButton, x, y);
  openMenu(
      std::move(entries),
      {.x = static_cast<std::int32_t>(x),
       .y = static_cast<std::int32_t>(y),
       .width = static_cast<std::int32_t>(m_menuButton->width()),
       .height = static_cast<std::int32_t>(m_menuButton->height())}
  );
}

void plume::EditorWindow::showTextMenu(float x, float y) {
  const Tab* tab = active();
  if (!tab) {
    return;
  }
  const auto id = [](Command command) { return static_cast<std::int32_t>(command); };
  const bool selection = !tab->view->selection().empty();
  openMenu(
      {
          {.id = id(Command::Undo), .label = "Undo", .enabled = tab->document->canUndo(), .shortcut = "Ctrl+Z"},
          {.id = id(Command::Redo), .label = "Redo", .enabled = tab->document->canRedo(), .shortcut = "Ctrl+Shift+Z"},
          {.label = "", .separator = true},
          {.id = id(Command::Cut), .label = "Cut", .enabled = selection, .shortcut = "Ctrl+X"},
          {.id = id(Command::Copy), .label = "Copy", .enabled = selection, .shortcut = "Ctrl+C"},
          {.id = id(Command::Paste), .label = "Paste", .shortcut = "Ctrl+V"},
          {.id = id(Command::SelectAll), .label = "Select All", .shortcut = "Ctrl+A"},
          {.label = "", .separator = true},
          {.id = id(Command::SortAscending), .label = selection ? "Sort Selected Lines" : "Sort Lines", .shortcut = "F9"},
          {.id = id(Command::SortDescending), .label = "Sort Descending", .shortcut = "Shift+F9"},
      },
      {.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y), .width = 1, .height = 1}
  );
}

void plume::EditorWindow::showDialog(std::unique_ptr<ConfirmDialog> dialog) {
  if (!m_root) {
    return;
  }
  closeDialog();
  m_dialog = dialog.get();
  m_root->addChild(std::move(dialog));
  m_backdrop->setVisible(true);
  m_dispatcher.setFocus(m_dialog->defaultArea());
  requestUpdate();
}

void plume::EditorWindow::closeDialog() {
  if (!m_dialog) {
    return;
  }
  // A button of the dialog may be running this: the dialog is destroyed after this turn of the loop.
  m_dispatcher.cancelPointerCapture();
  std::shared_ptr<Node> doomed(m_root->removeChild(m_dialog).release());
  DeferredCall::callLater([doomed] {});
  m_dialog = nullptr;
  m_backdrop->setVisible(false);
  focusText();
}

// ── Search ───────────────────────────────────────────────────────────────────

void plume::EditorWindow::showSearch(bool replace) {
  Tab* tab = active();
  if (!tab || !m_search) {
    return;
  }
  m_search->setVisible(true);
  if (replace) {
    m_search->setReplaceVisible(true);
  }
  // A selection within one line becomes the pattern.
  const std::string selected = tab->view->selectedText();
  if (!selected.empty() && selected.find('\n') == std::string::npos && selected.size() < 200) {
    m_search->setQuery(selected);
  }
  m_search->selectQuery();
  m_dispatcher.setFocus(m_search->findArea());
  m_focusPending = false;
  m_searchStale = true;
  requestUpdate();
}

void plume::EditorWindow::hideSearch() {
  if (!m_search || !m_search->visible()) {
    return;
  }
  m_search->setVisible(false);
  m_matches.clear();
  m_currentMatch.reset();
  if (Tab* tab = active()) {
    tab->view->setMatches({}, std::nullopt);
  }
  focusText();
}

void plume::EditorWindow::updateSearch() {
  Tab* tab = active();
  if (!tab || !m_search || !m_search->visible()) {
    return;
  }
  const Buffer& buffer = tab->document->buffer();
  if (m_searchStale || m_searchTab != tab->id || m_searchRevision != buffer.revision()) {
    m_matches.clear();
    m_pattern.reset();
    std::string problem;
    if (!m_search->query().empty()) {
      auto pattern = Pattern::compile(m_search->query(), m_search->options());
      if (pattern) {
        m_pattern = std::move(*pattern);
        m_searchText = buffer.text();
        m_matches = m_pattern->findAll(m_searchText);
      } else {
        problem = "Invalid";
      }
    }
    m_searchTab = tab->id;
    m_searchRevision = buffer.revision();
    m_searchStale = false;
    if (!problem.empty()) {
      m_search->setStatus(problem, true);
      tab->view->setMatches({}, std::nullopt);
      return;
    }
  }
  // The match that is selected right now is the current one.
  const Selection selection = tab->view->selection();
  m_currentMatch.reset();
  const auto at = std::lower_bound(m_matches.begin(), m_matches.end(), selection.begin(), [](const Match& match, std::size_t offset) {
    return match.offset < offset;
  });
  if (at != m_matches.end() && at->offset == selection.begin() && at->length == selection.length()) {
    m_currentMatch = static_cast<std::size_t>(at - m_matches.begin());
  }
  tab->view->setMatches(m_matches, m_currentMatch);
  if (m_search->query().empty()) {
    m_search->setStatus("", false);
  } else if (m_matches.empty()) {
    m_search->setStatus("No results", false);
  } else if (m_currentMatch) {
    m_search->setStatus(std::format("{} of {}", *m_currentMatch + 1, m_matches.size()), false);
  } else {
    m_search->setStatus(std::format("{} found", m_matches.size()), false);
  }
}

void plume::EditorWindow::stepSearch(bool forward) {
  Tab* tab = active();
  if (!tab) {
    return;
  }
  if (!m_search->visible()) {
    showSearch(false);
  }
  updateSearch();
  if (m_matches.empty()) {
    return;
  }
  const Selection selection = tab->view->selection();
  std::size_t index = 0;
  if (forward) {
    // The match under the caret counts when it is not the selected one already.
    const std::size_t from = m_currentMatch ? selection.begin() + 1 : selection.begin();
    const auto at = std::lower_bound(m_matches.begin(), m_matches.end(), from, [](const Match& match, std::size_t offset) {
      return match.offset < offset;
    });
    index = at == m_matches.end() ? 0 : static_cast<std::size_t>(at - m_matches.begin());
  } else {
    const auto at = std::lower_bound(m_matches.begin(), m_matches.end(), selection.begin(), [](const Match& match, std::size_t offset) {
      return match.offset < offset;
    });
    index = at == m_matches.begin() ? m_matches.size() - 1 : static_cast<std::size_t>(at - m_matches.begin()) - 1;
  }
  const Match& match = m_matches[index];
  tab->view->setSelection({.anchor = match.offset, .cursor = match.offset + match.length});
  tab->view->centerCursor();
  requestUpdate();
}

void plume::EditorWindow::replaceCurrent() {
  Tab* tab = active();
  if (!tab) {
    return;
  }
  updateSearch();
  if (m_pattern && m_currentMatch) {
    const Match match = m_matches[*m_currentMatch];
    tab->view->replaceRange(match.offset, match.length, m_pattern->expand(m_searchText, match, m_search->replacement()));
  }
  stepSearch(true);
}

void plume::EditorWindow::replaceAll() {
  Tab* tab = active();
  if (!tab) {
    return;
  }
  updateSearch();
  if (!m_pattern || m_matches.empty()) {
    return;
  }
  std::vector<std::string> texts;
  texts.reserve(m_matches.size());
  for (const Match& match : m_matches) {
    texts.push_back(m_pattern->expand(m_searchText, match, m_search->replacement()));
  }
  const std::size_t count = m_matches.size();
  tab->view->replaceMatches(m_matches, texts);
  status(std::format("Replaced {} {}", count, count == 1 ? "match" : "matches"));
}

// ── Scene ────────────────────────────────────────────────────────────────────

void plume::EditorWindow::build() {
  m_root = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Surface)});
  m_root->setAnimationManager(&m_services.animations);
  m_surface.setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());

  auto tooltip = std::make_unique<TooltipPresenter>([this] { m_surface.requestUpdate(); });
  m_tooltip = tooltip.get();
  m_root->addChild(std::move(tooltip));

  const auto caption = [](Label** out) {
    return ui::label(
        {.out = out,
         .fontSize = Style::fontSizeCaption,
         .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
         .maxLines = 1,
         .configure = [](Label& label) { label.setUseMarkup(false); }}
    );
  };
  auto bar = ui::row(
      {.out = &m_statusBar,
       .align = FlexAlign::Center,
       .gap = Style::spaceLg,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kStatusHeight,
       .participatesInLayout = false},
      ui::label(
          {.out = &m_statusText,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1,
           .ellipsize = TextEllipsize::Start,
           .flexGrow = 1,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      caption(&m_formatText), caption(&m_positionText)
  );
  bar->setZIndex(40);
  m_root->addChild(std::move(bar));

  auto search = std::make_unique<SearchBar>(
      SearchBar::Callbacks{
          .changed =
              [this] {
                m_searchStale = true;
                // Typing searches on from where the caret stands.
                if (Tab* tab = active()) {
                  const std::size_t from = tab->view->selection().begin();
                  tab->view->setSelection(Selection::at(from), false);
                }
                stepSearch(true);
              },
          .step = [this](bool forward) { stepSearch(forward); },
          .replace = [this] { replaceCurrent(); },
          .replaceAll = [this] { replaceAll(); },
          .close = [this] { hideSearch(); },
      }
  );
  m_search = search.get();
  m_root->addChild(std::move(search));

  // Swallows the pointer behind a dialog.
  auto backdrop = std::make_unique<InputArea>();
  backdrop->setParticipatesInLayout(false);
  backdrop->setZIndex(100);
  backdrop->setVisible(false);
  backdrop->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Scrim, 0.45F)}));
  m_backdrop = backdrop.get();
  m_root->addChild(std::move(backdrop));
}

void plume::EditorWindow::rebuildStrip() {
  std::vector<std::string> labels;
  labels.reserve(m_tabs.size());
  for (const auto& tab : m_tabs) {
    labels.push_back(tab->title());
  }
  const bool decorated = !m_surface.serverDecorated() && !m_surface.fullscreen();
  if (m_strip && labels == m_stripLabels && m_active == m_stripActive && decorated == m_stripDecorated) {
    return;
  }
  const float scrollOffset = m_tabScroll ? m_tabScroll->scrollOffset() : 0.0F;
  if (m_strip) {
    // A press held on the strip is lost with it.
    float stripX = 0.0F, stripY = 0.0F;
    Node::absolutePosition(m_strip, stripX, stripY);
    if (m_dispatcher.pointerCaptured() && m_pointerY < stripY + kStripHeight) {
      m_dispatcher.cancelPointerCapture();
    }
    if (m_tooltip) {
      m_tooltip->dismiss();
    }
    auto removed = m_root->removeChild(m_strip);
  }
  m_stripLabels = std::move(labels);
  m_stripActive = m_active;
  m_stripDecorated = decorated;

  auto scroll = ui::scrollView(
      {.out = &m_tabScroll,
       .orientation = ScrollOrientation::Horizontal,
       .scrollbarVisible = false,
       .viewportPaddingH = 0.0F,
       .viewportPaddingV = 0.0F,
       .fillWidth = true,
       .fillHeight = true,
       .flexGrow = 1}
  );
  auto tabs = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs, .height = kStripHeight});
  scroll->content()->setDirection(FlexDirection::Horizontal);
  auto tabPalette = Button::defaultPalette(ButtonVariant::Ghost);
  tabPalette.hover.bg = colorSpecFromRole(ColorRole::SurfaceContainerHigh);
  tabPalette.hover.label = colorSpecFromRole(ColorRole::OnSurface);
  tabPalette.pressed.bg = colorSpecFromRole(ColorRole::SurfaceContainerHighest);
  tabPalette.pressed.label = colorSpecFromRole(ColorRole::OnSurface);
  Button* selectedButton = nullptr;
  for (std::size_t index = 0; index < m_tabs.size(); ++index) {
    const std::uint64_t id = m_tabs[index]->id;
    const bool selected = index == m_active;
    auto closePalette = Button::defaultPalette(ButtonVariant::Ghost);
    closePalette.normal.label = colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurface);
    closePalette.hover.bg = colorSpecFromRole(selected ? ColorRole::PrimaryContainer : ColorRole::SurfaceContainerHighest);
    closePalette.hover.label = colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface);
    closePalette.pressed = closePalette.hover;
    Button* button = nullptr;
    auto tab = ui::button(
        {.out = &button,
         .text = m_stripLabels[index],
         .controlHeight = Style::controlHeightSm,
         .selected = selected,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .customPalette = tabPalette,
         .tooltip = m_tabs[index]->path.empty() ? std::optional<std::string>() : m_tabs[index]->path.string(),
         .minWidth = 96.0F,
         .maxWidth = 240.0F,
         .paddingV = 0.0F,
         .paddingH = Style::spaceXs,
         .gap = Style::spaceXs,
         .onClick = [this, id] { select(indexOf(id)); },
         .configure =
             [](Button& button) {
               button.label()->setUseMarkup(false);
               button.label()->setFlexGrow(1);
               button.label()->setTextAlign(TextAlign::Center);
             }}
    );
    if (selected) {
      selectedButton = button;
    }
    tab->addChild(
        ui::button(
            {.glyph = "x",
             .glyphSize = 14.0F,
             .controlHeight = 24.0F,
             .variant = ButtonVariant::Ghost,
             .customPalette = closePalette,
             .tooltip = "Close tab (Ctrl+W)",
             .minWidth = 24.0F,
             .maxWidth = 24.0F,
             .padding = 0.0F,
             .onClick = [this, id] { requestClose(indexOf(id)); },
             .configure = [](Button& button) { button.setZIndex(2); }}
        )
    );
    tabs->addChild(std::move(tab));
  }
  tabs->addChild(
      ui::button(
          {.glyph = "plus",
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .tooltip = "New tab (Ctrl+T)",
           .onClick = [this] { newTab(); }}
      )
  );
  scroll->content()->addChild(std::move(tabs));
  scroll->requestScrollToOffset(scrollOffset);
  if (selectedButton) {
    scroll->requestScrollIntoView(selectedButton);
  }
  auto strip = ui::row(
      {.out = &m_strip,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kStripHeight,
       .participatesInLayout = false},
      std::move(scroll),
      ui::button(
          {.out = &m_menuButton,
           .glyph = "menu-2",
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .tooltip = "Menu (F10)",
           .onClick = [this] {
             // The click that dismissed the open menu must not open it again.
             if (std::chrono::steady_clock::now() - m_menuClosedAt > kMenuReopenGuard) {
               showMainMenu();
             }
           }}
      )
  );
  if (decorated) {
    const auto control = [](const char* glyph, const char* tooltip, std::function<void()> click) {
      return ui::button(
          {.glyph = glyph,
           .controlHeight = Style::controlHeightSm,
           .variant = ButtonVariant::Ghost,
           .tooltip = tooltip,
           .onClick = std::move(click)}
      );
    };
    strip->addChild(control("minus", "Minimize window", [this] { m_surface.minimize(); }));
    strip->addChild(control("square", "Toggle maximized", [this] { m_surface.toggleMaximized(); }));
    strip->addChild(control("x", "Close window", [this] { m_surface.close(); }));
  }
  strip->setZIndex(50);
  m_root->addChild(std::move(strip));
}

void plume::EditorWindow::mount(std::size_t index) {
  if (m_mounted == index) {
    return;
  }
  if (m_mounted < m_tabs.size()) {
    Tab& previous = *m_tabs[m_mounted];
    previous.detached.reset(static_cast<TextView*>(m_root->removeChild(previous.view).release()));
  }
  Tab& tab = *m_tabs[index];
  m_root->addChild(std::move(tab.detached));
  m_mounted = index;
}

void plume::EditorWindow::prepare() {
  if (!m_surface.isRunning() || m_tabs.empty() || !m_services.renderer.makeCurrent(m_surface.renderTarget())) {
    return;
  }
  UiPhaseScope phase(UiPhase::Layout);
  if (!m_root) {
    build();
  }
  m_active = std::min(m_active, m_tabs.size() - 1);
  rebuildStrip();
  mount(m_active);
  Tab& tab = *m_tabs[m_active];
  if (m_focusPending && !m_dialog) {
    m_dispatcher.setFocus(tab.view->inputArea());
    m_focusPending = false;
  }
  updateSearch();
  m_surface.setTitle(std::format("{} — Plume", tab.title()));

  const Selection selection = tab.view->selection();
  m_statusText->setText(m_status.empty() ? tab.path.string() : m_status);
  m_positionText->setText(
      selection.empty()
          ? std::format("Ln {}, Col {}", tab.view->cursorLine() + 1, tab.view->cursorColumn() + 1)
          : std::format(
                "Ln {}, Col {}  ({} selected)", tab.view->cursorLine() + 1, tab.view->cursorColumn() + 1,
                utf8Length(tab.view->selectedText())
            )
  );
  m_formatText->setText(
      tab.readOnly
          ? std::string("Binary, read-only")
          : std::format(
                "{}{}{}   {}   {} {}", tab.language.empty() ? std::string() : tab.language + "   ", tab.format.encoding,
                tab.format.byteOrderMark ? " BOM" : "", tab.format.lineEnding == LineEnding::CrLf ? "CRLF" : "LF",
                m_settings.editor.insertSpaces ? "Spaces" : "Tab", m_settings.editor.tabWidth
            )
  );

  auto& renderer = m_surface.renderTarget().renderer();
  const auto width = static_cast<float>(m_surface.width()), height = static_cast<float>(m_surface.height());
  const float bodyHeight = std::max(1.0F, height - kStripHeight - kStatusHeight);
  m_root->arrange(renderer, {0, 0, width, height});
  m_strip->arrange(renderer, {0, 0, width, kStripHeight});
  m_statusBar->arrange(renderer, {0, height - kStatusHeight, width, kStatusHeight});
  tab.view->arrange(renderer, {0, kStripHeight, width, bodyHeight});
  if (m_search->visible()) {
    const float cardWidth = std::min(kSearchWidth, width - 2.0F * Style::spaceLg);
    auto constraints = LayoutConstraints::available(cardWidth, bodyHeight);
    constraints.setExactWidth(cardWidth);
    const auto size = m_search->measure(renderer, constraints);
    m_search->arrange(
        renderer, {width - cardWidth - Style::spaceLg - 4.0F, kStripHeight + Style::spaceSm, cardWidth, size.height}
    );
  }
  m_backdrop->arrange(renderer, {0, 0, width, height});
  m_backdrop->children().front()->arrange(renderer, {0, 0, width, height});
  if (m_dialog) {
    const auto size = m_dialog->measure(renderer, LayoutConstraints::available(width - 2.0F * Style::spaceLg, height));
    m_dialog->arrange(
        renderer, {std::round((width - size.width) * 0.5F), std::round((height - size.height) * 0.4F), size.width, size.height}
    );
  }
  if (m_dispatcher.pointerCaptured() || m_menu.isOpen()) {
    m_tooltip->dismiss();
  } else {
    m_tooltip->sync(renderer, *m_root, m_pointerX, m_pointerY, {width, height});
  }
}

// ── Input ────────────────────────────────────────────────────────────────────

void plume::EditorWindow::modifiers(std::uint32_t modifiers) {
  m_modifiers = modifiers;
  for (const auto& tab : m_tabs) {
    tab->view->setModifiers(modifiers);
  }
}

void plume::EditorWindow::focus(wl_surface* surface, bool entered) {
  if (surface != m_surface.wlSurface()) {
    return;
  }
  if (entered) {
    m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);
    checkDisk();
  } else if (m_tooltip) {
    m_tooltip->dismiss();
  }
  requestUpdate();
}

void plume::EditorWindow::keyboard(const KeyboardEvent& event) {
  if (ContextMenuPopup::dispatchKeyboardEvent(event)) {
    return;
  }
  if (event.pressed && m_tooltip) {
    m_tooltip->dismiss();
  }
  const auto forward = [this, &event] {
    m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
    requestUpdate();
  };
  if (!event.pressed || event.preedit) {
    forward();
    return;
  }
  const std::uint32_t sym = lower(event.sym);
  if (m_dialog) {
    if (KeySymbol::isEscape(sym)) {
      closeDialog();
    } else if (KeySymbol::isTab(sym) || sym == XKB_KEY_ISO_Left_Tab) {
      m_dispatcher.cycleTabFocusInSubtree(m_dialog, event.modifiers == KeyMod::Shift);
      requestUpdate();
    } else if (KeySymbol::isLeft(sym) || KeySymbol::isRight(sym)) {
      m_dispatcher.cycleTabFocusInSubtree(m_dialog, KeySymbol::isLeft(sym));
      requestUpdate();
    } else {
      forward();
    }
    return;
  }
  const bool ctrl = event.modifiers == KeyMod::Ctrl;
  const bool ctrlShift = event.modifiers == (KeyMod::Ctrl | KeyMod::Shift);
  const bool plain = event.modifiers == 0;
  const bool shift = event.modifiers == KeyMod::Shift;
  std::optional<Command> command;
  if (ctrl) {
    switch (sym) {
    case XKB_KEY_s:
      command = Command::Save;
      break;
    case XKB_KEY_o:
      command = Command::Open;
      break;
    case XKB_KEY_t:
    case XKB_KEY_n:
      command = Command::NewTab;
      break;
    case XKB_KEY_w:
      command = Command::CloseTab;
      break;
    case XKB_KEY_q:
      command = Command::Quit;
      break;
    case XKB_KEY_f:
      command = Command::Find;
      break;
    case XKB_KEY_h:
      command = Command::Replace;
      break;
    case XKB_KEY_g:
      stepSearch(true);
      return;
    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
      command = Command::ZoomIn;
      break;
    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      command = Command::ZoomOut;
      break;
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
      command = Command::ZoomReset;
      break;
    case XKB_KEY_Page_Down:
    case XKB_KEY_Tab:
      select((m_active + 1) % m_tabs.size());
      return;
    case XKB_KEY_Page_Up:
      select((m_active + m_tabs.size() - 1) % m_tabs.size());
      return;
    default:
      break;
    }
  } else if (ctrlShift) {
    switch (sym) {
    case XKB_KEY_s:
      command = Command::SaveAs;
      break;
    case XKB_KEY_g:
      stepSearch(false);
      return;
    case XKB_KEY_ISO_Left_Tab:
    case XKB_KEY_Tab:
      select((m_active + m_tabs.size() - 1) % m_tabs.size());
      return;
    default:
      break;
    }
  } else if (event.modifiers == KeyMod::Alt && sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
    select(std::min<std::size_t>(sym - XKB_KEY_1, m_tabs.size() - 1));
    return;
  } else if (plain || shift) {
    switch (sym) {
    case XKB_KEY_Escape:
      if (plain && m_search && m_search->visible()) {
        hideSearch();
        return;
      }
      break;
    case XKB_KEY_F3:
      stepSearch(plain);
      return;
    case XKB_KEY_F7:
      command = Command::LineNumbers;
      break;
    case XKB_KEY_F8:
      command = Command::Wrap;
      break;
    case XKB_KEY_F9:
      command = plain ? Command::SortAscending : Command::SortDescending;
      break;
    case XKB_KEY_F10:
      showMainMenu();
      return;
    default:
      break;
    }
  }
  if (command) {
    run(*command);
    return;
  }
  forward();
}

void plume::EditorWindow::pointer(const PointerEvent& event) {
  if (event.type == PointerEvent::Type::Button && !event.pressed && std::exchange(m_swallowRelease, false)) {
    return;
  }
  if (m_menu.isOpen()) {
    // A right click anywhere, or any click beside the menu, puts it away without choosing from it.
    if (event.type == PointerEvent::Type::Button
        && event.pressed
        && (event.button == BTN_RIGHT || event.surface != m_menu.wlSurface())) {
      m_menu.close();
      m_swallowRelease = true;
      return;
    }
    if (m_menu.onPointerEvent(event)) {
      return;
    }
  }
  if (event.surface != m_surface.wlSurface()) {
    return;
  }
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
  if (m_tooltip) {
    if (!event.touch && (event.type == PointerEvent::Type::Enter || event.type == PointerEvent::Type::Motion)) {
      m_tooltip->resume();
    } else {
      m_tooltip->dismiss();
    }
  }
  m_pointerX = x;
  m_pointerY = y;
  switch (event.type) {
  case PointerEvent::Type::Enter:
    m_dispatcher.pointerEnter(x, y, event.serial);
    break;
  case PointerEvent::Type::Leave:
    m_dispatcher.pointerLeave();
    break;
  case PointerEvent::Type::Motion:
    m_dispatcher.pointerMotion(x, y, event.serial);
    break;
  case PointerEvent::Type::Button: {
    const bool taken = m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    // Without server decorations the empty part of the strip moves the window.
    if (!taken && event.pressed && event.button == BTN_LEFT && m_stripDecorated && y < kStripHeight && !m_dialog) {
      m_surface.beginMove(event.serial);
    }
    // A click into the text takes the keyboard back from the search card.
    if (event.pressed && m_search && m_search->visible() && !m_dialog) {
      if (Tab* tab = active(); tab && tab->view->containsScenePoint(x, y) && !m_search->containsScenePoint(x, y)) {
        m_dispatcher.setFocus(tab->view->inputArea());
      }
    }
    break;
  }
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
  requestUpdate();
}

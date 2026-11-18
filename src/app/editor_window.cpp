#include "app/editor_window.h"

#include "core/input/key_modifiers.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "wayland/text_input_service.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr float kStripHeight = Style::controlHeightSm + 2.0F * Style::spaceXs;
  constexpr float kStatusHeight = 28.0F;
  constexpr float kDefaultFontSize = 13.0F;
} // namespace

std::string plume::EditorWindow::Tab::title() const {
  std::string name = path.empty() ? std::string("Untitled") : path.filename().string();
  return document && document->modified() ? "• " + name : name;
}

plume::EditorWindow::EditorWindow(Services services) : m_services(services), m_surface(services.connection) {
  m_options.fontSize = kDefaultFontSize;
  m_surface.setRenderContext(&m_services.renderer);
  m_surface.setAnimationManager(&m_services.animations);
  m_surface.setPrepareFrameCallback([this](bool, bool) { prepare(); });
  m_surface.setUpdateCallback([] {});
  m_surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) { m_surface.requestLayout(); });
  m_dispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
    m_services.connection.extension<DesktopIntegration>().setCursorShape(serial, shape);
  });
  if (!m_surface.initialize(
          nullptr,
          {.width = 960, .height = 720, .minWidth = 360, .minHeight = 240, .title = "Plume", .appId = "de.flk.Plume"}
      )) {
    throw std::runtime_error("cannot create the window");
  }
  m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);
}

plume::EditorWindow::~EditorWindow() = default;

void plume::EditorWindow::requestUpdate() { m_surface.requestLayout(); }

void plume::EditorWindow::status(std::string message) {
  m_status = std::move(message);
  requestUpdate();
}

plume::EditorWindow::Tab& plume::EditorWindow::addTab() {
  auto tab = std::make_unique<Tab>();
  tab->id = m_nextTab++;
  tab->document = std::make_unique<Document>();
  tab->detached = std::make_unique<TextView>();
  tab->view = tab->detached.get();
  tab->view->setParticipatesInLayout(false);
  tab->view->setOptions(m_options);
  tab->view->setDocument(tab->document.get());
  tab->view->setOnChange([this] { requestUpdate(); });
  tab->view->setOnCursorMoved([this] { requestUpdate(); });
  m_tabs.push_back(std::move(tab));
  return *m_tabs.back();
}

void plume::EditorWindow::newTab() {
  addTab();
  select(m_tabs.size() - 1);
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
  // A lone untouched empty tab is replaced instead of left behind.
  const bool reuse = m_tabs.size() == 1
      && m_tabs[0]->path.empty()
      && !m_tabs[0]->document->modified()
      && m_tabs[0]->document->buffer().empty();
  Tab& tab = reuse ? *m_tabs[0] : addTab();
  tab.path = absolute;
  if (std::filesystem::exists(absolute, error)) {
    auto loaded = loadFile(absolute);
    if (loaded) {
      tab.format = loaded->format;
      tab.document->setText(std::move(loaded->text));
      tab.view->documentReplaced();
    } else {
      status(std::format("{}: {}", absolute.filename().string(), loaded.error()));
    }
  }
  select(reuse ? 0 : m_tabs.size() - 1);
}

void plume::EditorWindow::select(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  m_active = index;
  m_focusPending = true;
  requestUpdate();
}

void plume::EditorWindow::closeTab(std::size_t index) {
  if (index >= m_tabs.size()) {
    return;
  }
  if (m_mounted == index) {
    m_tabs[index]->detached.reset(static_cast<TextView*>(m_root->removeChild(m_tabs[index]->view).release()));
  }
  m_mounted = static_cast<std::size_t>(-1);
  m_tabs.erase(m_tabs.begin() + static_cast<std::ptrdiff_t>(index));
  if (m_tabs.empty()) {
    m_surface.close();
    return;
  }
  m_active = std::min(index, m_tabs.size() - 1);
  m_focusPending = true;
  requestUpdate();
}

void plume::EditorWindow::save() {
  Tab* tab = active();
  if (!tab || tab->path.empty()) {
    status("Save As is not available yet");
    return;
  }
  const auto result = saveFile(tab->path, tab->document->buffer().text(), tab->format);
  if (!result) {
    status(std::format("{}: {}", tab->path.filename().string(), result.error()));
    return;
  }
  tab->document->markSaved();
  status(std::format("Saved {}", tab->path.string()));
}

void plume::EditorWindow::applyOptions() {
  for (const auto& tab : m_tabs) {
    tab->view->setOptions(m_options);
  }
  requestUpdate();
}

void plume::EditorWindow::zoom(float delta) {
  m_options.fontSize = delta == 0.0F ? kDefaultFontSize : std::clamp(m_options.fontSize + delta, 6.0F, 48.0F);
  applyOptions();
}

void plume::EditorWindow::build() {
  m_root = ui::column({.align = FlexAlign::Stretch, .fill = colorSpecFromRole(ColorRole::Surface)});
  m_root->setAnimationManager(&m_services.animations);
  m_surface.setSceneRoot(m_root.get());
  m_dispatcher.setSceneRoot(m_root.get());
  auto bar = ui::row(
      {.out = &m_statusBar,
       .align = FlexAlign::Center,
       .gap = Style::spaceMd,
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
           .flexGrow = 1}
      ),
      ui::label(
          {.out = &m_formatText, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
      ),
      ui::label(
          {.out = &m_positionText, .fontSize = Style::fontSizeCaption, .color = colorSpecFromRole(ColorRole::OnSurfaceVariant)}
      )
  );
  bar->setZIndex(40);
  m_root->addChild(std::move(bar));
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
  if (m_dispatcher.pointerCaptured()) {
    m_dispatcher.cancelPointerCapture();
  }
  if (m_strip) {
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
  for (std::size_t index = 0; index < m_tabs.size(); ++index) {
    const std::uint64_t id = m_tabs[index]->id;
    const auto find = [this, id]() -> std::size_t {
      for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i]->id == id) {
          return i;
        }
      }
      return m_tabs.size();
    };
    const bool selected = index == m_active;
    auto closePalette = Button::defaultPalette(ButtonVariant::Ghost);
    closePalette.normal.label = colorSpecFromRole(selected ? ColorRole::OnPrimary : ColorRole::OnSurface);
    closePalette.hover.bg = colorSpecFromRole(selected ? ColorRole::PrimaryContainer : ColorRole::SurfaceContainerHighest);
    closePalette.hover.label = colorSpecFromRole(selected ? ColorRole::OnPrimaryContainer : ColorRole::OnSurface);
    closePalette.pressed = closePalette.hover;
    auto tab = ui::button(
        {.text = m_stripLabels[index],
         .controlHeight = Style::controlHeightSm,
         .selected = selected,
         .contentAlign = ButtonContentAlign::Start,
         .variant = ButtonVariant::Ghost,
         .customPalette = tabPalette,
         .minWidth = 96.0F,
         .maxWidth = 240.0F,
         .paddingV = 0.0F,
         .paddingH = Style::spaceXs,
         .gap = Style::spaceXs,
         .onClick = [this, find] { select(find()); },
         .configure =
             [](Button& button) {
               button.label()->setUseMarkup(false);
               button.label()->setFlexGrow(1);
               button.label()->setTextAlign(TextAlign::Center);
             }}
    );
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
             .onClick = [this, find] { closeTab(find()); },
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
  auto strip = ui::row(
      {.out = &m_strip,
       .align = FlexAlign::Center,
       .gap = Style::spaceXs,
       .paddingH = Style::spaceSm,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerLow),
       .height = kStripHeight,
       .participatesInLayout = false},
      std::move(scroll)
  );
  if (decorated) {
    strip->addChild(
        ui::button(
            {.glyph = "minus",
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Minimize window",
             .onClick = [this] { m_surface.minimize(); }}
        )
    );
    strip->addChild(
        ui::button(
            {.glyph = "square",
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Toggle maximized",
             .onClick = [this] { m_surface.toggleMaximized(); }}
        )
    );
    strip->addChild(
        ui::button(
            {.glyph = "x",
             .controlHeight = Style::controlHeightSm,
             .variant = ButtonVariant::Ghost,
             .tooltip = "Close window",
             .onClick = [this] { m_surface.close(); }}
        )
    );
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
  rebuildStrip();
  mount(m_active);
  Tab& tab = *m_tabs[m_active];
  if (m_focusPending) {
    m_dispatcher.setFocus(tab.view->inputArea());
    m_focusPending = false;
  }
  m_surface.setTitle(std::format("{} — Plume", tab.title()));

  m_statusText->setText(m_status.empty() ? tab.path.string() : m_status);
  m_positionText->setText(std::format("Ln {}, Col {}", tab.view->cursorLine() + 1, tab.view->cursorColumn() + 1));
  m_formatText->setText(
      std::format("{}  {}", tab.format.encoding, tab.format.lineEnding == LineEnding::CrLf ? "CRLF" : "LF")
  );

  auto& renderer = m_surface.renderTarget().renderer();
  const auto width = static_cast<float>(m_surface.width()), height = static_cast<float>(m_surface.height());
  m_root->arrange(renderer, {0, 0, width, height});
  m_strip->arrange(renderer, {0, 0, width, kStripHeight});
  m_statusBar->arrange(renderer, {0, height - kStatusHeight, width, kStatusHeight});
  tab.view->arrange(renderer, {0, kStripHeight, width, std::max(1.0F, height - kStripHeight - kStatusHeight)});
}

void plume::EditorWindow::focus(bool entered) {
  if (entered) {
    m_dispatcher.setTextInputContext(m_surface.wlSurface(), &m_services.textInput);
  }
  requestUpdate();
}

void plume::EditorWindow::keyboard(const KeyboardEvent& event) {
  if (event.pressed && !event.preedit) {
    m_status.clear();
    const bool ctrl = event.modifiers == KeyMod::Ctrl;
    const bool ctrlShift = event.modifiers == (KeyMod::Ctrl | KeyMod::Shift);
    Tab* tab = active();
    if (ctrl) {
      switch (event.sym) {
      case XKB_KEY_s:
        save();
        return;
      case XKB_KEY_t:
      case XKB_KEY_n:
        newTab();
        return;
      case XKB_KEY_w:
        closeTab(m_active);
        return;
      case XKB_KEY_plus:
      case XKB_KEY_equal:
      case XKB_KEY_KP_Add:
        zoom(1.0F);
        return;
      case XKB_KEY_minus:
      case XKB_KEY_KP_Subtract:
        zoom(-1.0F);
        return;
      case XKB_KEY_0:
      case XKB_KEY_KP_0:
        zoom(0.0F);
        return;
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
    }
    if (ctrlShift && (event.sym == XKB_KEY_ISO_Left_Tab || event.sym == XKB_KEY_Tab)) {
      select((m_active + m_tabs.size() - 1) % m_tabs.size());
      return;
    }
    if (event.modifiers == 0 && tab) {
      switch (event.sym) {
      case XKB_KEY_F9:
        tab->view->sortLines();
        return;
      case XKB_KEY_F7:
        m_options.lineNumbers = !m_options.lineNumbers;
        applyOptions();
        return;
      case XKB_KEY_F8:
        m_options.wrap = !m_options.wrap;
        applyOptions();
        return;
      default:
        break;
      }
    }
  }
  m_dispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  requestUpdate();
}

void plume::EditorWindow::pointer(const PointerEvent& event) {
  const auto x = static_cast<float>(event.sx), y = static_cast<float>(event.sy);
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
  case PointerEvent::Type::Button:
    m_dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
    break;
  case PointerEvent::Type::Axis:
    m_dispatcher.pointerAxis(
        x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    break;
  }
  requestUpdate();
}

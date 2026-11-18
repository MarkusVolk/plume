#include "ui/search_bar.h"

#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
  constexpr float kIconButton = 28.0F;
}

plume::SearchBar::SearchBar(Callbacks callbacks) : m_callbacks(std::move(callbacks)) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceXs);
  setPadding(Style::spaceSm);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh));
  setStyleRadius(Style::radiusLg);
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setParticipatesInLayout(false);
  setZIndex(30);
  setVisible(false);

  const auto icon = [](Button** out, const char* glyph, const char* tooltip, std::function<void()> click) {
    return ui::button(
        {.out = out,
         .glyph = glyph,
         .glyphSize = 16.0F,
         .controlHeight = kIconButton,
         .variant = ButtonVariant::Ghost,
         .tooltip = tooltip,
         .minWidth = kIconButton,
         .maxWidth = kIconButton,
         .padding = 0.0F,
         .onClick = std::move(click)}
    );
  };
  // Keeps the fields of the lower rows in line with the pattern above them.
  const auto indent = [] {
    auto gap = std::make_unique<Node>();
    gap->setSize(kIconButton, 1.0F);
    return gap;
  };
  // Keys both fields share: Escape closes, Shift+Enter searches backwards.
  const auto keys = [this](std::uint32_t sym, std::uint32_t modifiers) {
    if (KeySymbol::isEscape(sym)) {
      m_callbacks.close();
      return true;
    }
    if (KeySymbol::isEnter(sym) && modifiers == KeyMod::Shift) {
      m_callbacks.step(false);
      return true;
    }
    return false;
  };

  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs},
          icon(
              &m_replaceToggle, "replace", "Replace (Ctrl+H)", [this] { setReplaceVisible(!m_replaceVisible); }
          ),
          ui::input(
              {.out = &m_find,
               .placeholder = "Find",
               .fontSize = Style::fontSizeCaption,
               .controlHeight = kIconButton,
               .flexGrow = 1,
               .onChange = [this](const std::string&) { m_callbacks.changed(); },
               .onSubmit = [this](const std::string&) { m_callbacks.step(true); },
               .onKeyEvent = keys}
          ),
          ui::label(
              {.out = &m_status,
               .fontSize = Style::fontSizeMini,
               .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
               .minWidth = 64.0F,
               .maxLines = 1,
               .textAlign = TextAlign::End}
          ),
          icon(nullptr, "chevron-up", "Previous (Shift+Enter)", [this] { m_callbacks.step(false); }),
          icon(nullptr, "chevron-down", "Next (Enter)", [this] { m_callbacks.step(true); }),
          icon(nullptr, "x", "Close (Escape)", [this] { m_callbacks.close(); })
      )
  );
  addChild(
      ui::row(
          {.out = &m_replaceRow, .align = FlexAlign::Center, .gap = Style::spaceXs, .visible = false, .participatesInLayout = false},
          indent(),
          ui::input(
              {.out = &m_replace,
               .placeholder = "Replace with",
               .fontSize = Style::fontSizeCaption,
               .controlHeight = kIconButton,
               .flexGrow = 1,
               .onSubmit = [this](const std::string&) { m_callbacks.replace(); },
               .onKeyEvent = keys}
          ),
          ui::button(
              {.text = "Replace",
               .fontSize = Style::fontSizeCaption,
               .controlHeight = kIconButton,
               .variant = ButtonVariant::Outline,
               .onClick = [this] { m_callbacks.replace(); }}
          ),
          ui::button(
              {.text = "All",
               .fontSize = Style::fontSizeCaption,
               .controlHeight = kIconButton,
               .variant = ButtonVariant::Outline,
               .tooltip = "Replace every match",
               .onClick = [this] { m_callbacks.replaceAll(); }}
          )
      )
  );
  addChild(
      ui::row(
          {.align = FlexAlign::Center, .gap = Style::spaceXs},
          indent(),
          icon(&m_caseButton, "letter-case", "Match case", [this] { toggle(&SearchOptions::caseSensitive, m_caseButton); }),
          icon(&m_wordButton, "abc", "Whole words", [this] { toggle(&SearchOptions::wholeWord, m_wordButton); }),
          icon(&m_regexButton, "regex", "Regular expression", [this] { toggle(&SearchOptions::regex, m_regexButton); })
      )
  );
}

void plume::SearchBar::toggle(bool SearchOptions::* flag, Button* button) {
  m_options.*flag = !(m_options.*flag);
  button->setSelected(m_options.*flag);
  m_callbacks.changed();
}

const std::string& plume::SearchBar::query() const { return m_find->value(); }

const std::string& plume::SearchBar::replacement() const { return m_replace->value(); }

void plume::SearchBar::setQuery(const std::string& query) { m_find->setValue(query); }

void plume::SearchBar::setReplaceVisible(bool visible) {
  m_replaceVisible = visible;
  m_replaceRow->setVisible(visible);
  m_replaceRow->setParticipatesInLayout(visible);
  m_replaceToggle->setSelected(visible);
  markLayoutDirty();
}

void plume::SearchBar::setStatus(const std::string& status, bool invalid) {
  m_status->setText(status);
  m_status->setColor(colorSpecFromRole(invalid ? ColorRole::Error : ColorRole::OnSurfaceVariant));
  m_find->setInvalid(invalid);
}

InputArea* plume::SearchBar::findArea() const { return m_find->inputArea(); }

InputArea* plume::SearchBar::replaceArea() const { return m_replace->inputArea(); }

void plume::SearchBar::selectQuery() { m_find->selectRange(0, m_find->value().size()); }

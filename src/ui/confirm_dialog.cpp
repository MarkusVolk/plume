#include "ui/confirm_dialog.h"

#include "ui/builders.h"
#include "ui/style.h"

plume::ConfirmDialog::ConfirmDialog(std::string title, std::string message, std::vector<Answer> answers) {
  setDirection(FlexDirection::Vertical);
  setAlign(FlexAlign::Stretch);
  setGap(Style::spaceMd);
  setPadding(Style::spaceLg);
  setFill(colorSpecFromRole(ColorRole::SurfaceContainerHigh));
  setStyleRadius(Style::radiusXl);
  setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
  setParticipatesInLayout(false);
  setZIndex(110);

  const auto literal = [](Label& label) { label.setUseMarkup(false); };
  addChild(
      ui::label(
          {.text = std::move(title),
           .fontSize = Style::fontSizeTitle,
           .fontWeight = FontWeight::SemiBold,
           .color = colorSpecFromRole(ColorRole::OnSurface),
           .maxWidth = 420.0F,
           .maxLines = 2,
           .configure = literal}
      )
  );
  addChild(
      ui::label(
          {.text = std::move(message),
           .fontSize = Style::fontSizeBody,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = 420.0F,
           .maxLines = 4,
           .configure = literal}
      )
  );
  auto row = ui::row({.align = FlexAlign::Center, .justify = FlexJustify::End, .gap = Style::spaceSm});
  for (auto& answer : answers) {
    Button* button = nullptr;
    row->addChild(
        ui::button(
            {.out = &button,
             .text = answer.label,
             .controlHeight = Style::controlHeightSm,
             .variant = answer.primary       ? ButtonVariant::Primary
                 : answer.destructive ? ButtonVariant::Destructive
                                      : ButtonVariant::Outline,
             .minWidth = 88.0F,
             .onClick = std::move(answer.chosen)}
        )
    );
    if (answer.primary || !m_default) {
      m_default = button->inputArea();
    }
  }
  addChild(std::move(row));
}

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Config/Graphics/RemixWidget.h"

#include <span>
#include <variant>

#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QStringList>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"
#include "Core/Config/RemixSettings.h"
#include "Core/Core.h"
#include "Core/System.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/Config/ConfigControls/ConfigInteger.h"
#include "DolphinQt/Config/ConfigControls/ConfigText.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/Settings.h"

using Group = Config::RemixSettingMeta::Group;
using Liveness = Config::RemixSettingMeta::Liveness;
using Maturity = Config::RemixSettingMeta::Maturity;

namespace
{
// The README's sections, in the order the tab shows them.
struct GroupTitle
{
  Group group;
  const char* title;
};

constexpr GroupTitle GROUP_TITLES[] = {
    {Group::RuntimeScale, QT_TR_NOOP("Runtime and Scale")},
    {Group::CameraRecovery, QT_TR_NOOP("Camera Recovery")},
    {Group::Sky, QT_TR_NOOP("Sky")},
    {Group::GxSemantics, QT_TR_NOOP("GX Semantics")},
    {Group::ProjectionViewport, QT_TR_NOOP("Projection and Viewport")},
    {Group::UiOverlay, QT_TR_NOOP("UI Overlay")},
    {Group::Efb, QT_TR_NOOP("EFB Emulation")},
    {Group::Diagnostics, QT_TR_NOOP("Diagnostics")},
};

// The label text IS the INI key, deliberately: it is an identifier, it is what
// GFX.ini and every log line call the option, and taking it from the Location
// means a renamed key can never leave a stale label behind. For the same reason
// it is never run through tr().
QString SettingKey(const Config::RemixSettingMeta& meta)
{
  return std::visit([](const auto* info) { return QString::fromStdString(info->GetLocation().key); },
                    meta.setting);
}

// Tooltips are untranslated too - they live in Core, which has no access to
// Qt's translation pipeline, and translating half a tab would be worse than
// translating none of it. BalloonTip renders rich text, hence <br>.
QString SettingTooltip(const Config::RemixSettingMeta& meta)
{
  QString text = QString::fromUtf8(meta.tooltip);
  if (meta.maturity == Maturity::Experimental)
    text.prepend(QStringLiteral("Experimental. "));
  if (meta.liveness == Liveness::RequiresRestart)
    text += QStringLiteral("<br><br>Takes effect when a game starts - restart the game to apply.");
  return text;
}
}  // namespace

RemixWidget::RemixWidget(GraphicsPane* gfx_pane) : m_game_layer{gfx_pane->GetConfigLayer()}
{
  CreateWidgets();

  connect(gfx_pane, &GraphicsPane::BackendChanged, this, &RemixWidget::OnBackendChanged);
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this, [this](Core::State state) {
    OnEmulationStateChanged(state != Core::State::Uninitialized);
  });

  OnBackendChanged(QString::fromStdString(Config::Get(Config::MAIN_GFX_BACKEND)));
  OnEmulationStateChanged(!Core::IsUninitialized(Core::System::GetInstance()));
}

void RemixWidget::CreateWidgets()
{
  auto* const main_layout = new QVBoxLayout;

  auto* const status = new QLabel(
      tr("Options for the Remix video backend. Every label is the exact GFX.ini key name. "
         "Right-click a per-game value to clear it. Greyed-out options are read once when the "
         "backend starts, so they apply after the game is restarted."));
  status->setWordWrap(true);
  main_layout->addWidget(status);

  m_filter = new QLineEdit;
  m_filter->setPlaceholderText(tr("Filter options..."));
  m_filter->setClearButtonEnabled(true);
  connect(m_filter, &QLineEdit::textChanged, this, &RemixWidget::OnFilterChanged);

  // Everything below the status line lives in one container, so the whole tab
  // can be disabled in one call when another backend is selected.
  m_content = new QWidget;
  auto* const content_layout = new QVBoxLayout{m_content};
  content_layout->setContentsMargins(0, 0, 0, 0);
  content_layout->addWidget(m_filter);

  const std::span<const Config::RemixSettingMeta> settings = Config::GetRemixSettingsMetadata();

  for (const GroupTitle& group_title : GROUP_TITLES)
  {
    auto* const box = new QGroupBox(tr(group_title.title));
    auto* const form = new QFormLayout;

    const bool collapsible = group_title.group == Group::Diagnostics;
    if (collapsible)
    {
      // No collapsible-group helper exists in DolphinQt, and the diagnostics are
      // noise for anyone not debugging the backend, so the group's contents go
      // in a child widget that the group box's own check state shows and hides.
      m_diagnostics_box = box;
      m_diagnostics_contents = new QWidget;
      m_diagnostics_contents->setLayout(form);
      form->setContentsMargins(0, 0, 0, 0);
      auto* const box_layout = new QVBoxLayout{box};
      box_layout->addWidget(m_diagnostics_contents);
    }
    else
    {
      box->setLayout(form);
    }

    for (const Config::RemixSettingMeta& meta : settings)
    {
      if (meta.group == group_title.group)
        AddSetting(box, form, meta);
    }

    if (collapsible)
    {
      box->setCheckable(true);
      box->setChecked(false);
      m_diagnostics_contents->setVisible(false);
      connect(box, &QGroupBox::toggled, this, [this](bool on) {
        m_diagnostics_contents->setVisible(on);
        // A checkable QGroupBox re-enables all of its children when it is
        // ticked, which would undo the liveness greying below it.
        ApplyLiveness();
      });
    }

    m_boxes.push_back(box);
    content_layout->addWidget(box);
  }

  main_layout->addWidget(m_content);
  main_layout->addStretch();

  setLayout(main_layout);
}

void RemixWidget::AddSetting(QGroupBox* box, QFormLayout* form,
                             const Config::RemixSettingMeta& meta)
{
  const QString key = SettingKey(meta);
  const QString tooltip = SettingTooltip(meta);

  QWidget* label = nullptr;
  QWidget* control = nullptr;

  if (const auto* const* bool_setting = std::get_if<const Config::Info<bool>*>(&meta.setting))
  {
    // The checkbox carries the key name itself, so no separate label row.
    auto* const check = new ConfigBool(key, **bool_setting, m_game_layer);
    check->SetTitle(key);
    check->SetDescription(tooltip);
    control = check;
    form->addRow(check);
  }
  else if (const auto* const* int_setting = std::get_if<const Config::Info<int>*>(&meta.setting))
  {
    if (!meta.choices.empty())
    {
      QStringList options;
      for (const char* const option : meta.choices)
        options << QString::fromUtf8(option);

      auto* const combo = new ConfigChoice(options, **int_setting, m_game_layer);
      combo->SetTitle(key);
      combo->SetDescription(tooltip);
      label = new QLabel(key);
      control = combo;
    }
    else
    {
      auto* const spin = new ConfigInteger(static_cast<int>(meta.min), static_cast<int>(meta.max),
                                           **int_setting, m_game_layer);
      spin->SetTitle(key);
      spin->SetDescription(tooltip);
      label = new ConfigIntegerLabel(key, spin);
      control = spin;
    }
  }
  else if (const auto* const* float_setting =
               std::get_if<const Config::Info<float>*>(&meta.setting))
  {
    auto* const slider =
        new ConfigFloatSlider(meta.min, meta.max, **float_setting, meta.step, m_game_layer);
    slider->SetTitle(key);
    slider->SetDescription(tooltip);
    label = new ConfigFloatLabel(key, slider);
    control = slider;
  }
  else if (const auto* const* string_setting =
               std::get_if<const Config::Info<std::string>*>(&meta.setting))
  {
    // ConfigText is a QLineEdit rather than a ToolTipWidget, so it gets an
    // ordinary tooltip instead of a balloon.
    auto* const edit = new ConfigText(**string_setting, m_game_layer);
    edit->setToolTip(tooltip);
    label = new QLabel(key);
    control = edit;
  }
  else
  {
    // Unreachable: the variant has exactly these four alternatives and the unit
    // test asserts none of them is null.
    return;
  }

  if (label != nullptr)
  {
    label->setToolTip(tooltip);
    form->addRow(label, control);
  }

  m_rows.push_back({form, box, label, control, &meta, (key + tooltip).toLower(), true});
}

void RemixWidget::OnBackendChanged(const QString& backend_name)
{
  // The signal carries the backend's CONFIG name, not its display name - it is
  // MAIN_GFX_BACKEND / the backend combo's item data, both of which are
  // GetConfigName(). The literal is Remix::VideoBackend::CONFIG_NAME
  // (VideoBackends/Remix/VideoBackend.h), spelled out here rather than included
  // because that backend is built on Windows only and DolphinQt is not.
  m_content->setEnabled(backend_name == QStringLiteral("Remix"));
}

void RemixWidget::OnEmulationStateChanged(bool running)
{
  m_emulation_running = running;
  ApplyLiveness();
}

void RemixWidget::ApplyLiveness()
{
  // Only in the global dialog. A per-game edit lands in the game's INI, and the
  // GameINI layers are installed before the video backend is initialized, so
  // even a read-once knob honours the profile at the game's next boot - there
  // is nothing to grey out there.
  if (m_game_layer != nullptr)
    return;

  for (const Row& row : m_rows)
  {
    const bool enabled =
        !m_emulation_running || row.meta->liveness == Liveness::Live;
    row.control->setEnabled(enabled);
    if (row.label != nullptr)
      row.label->setEnabled(enabled);
  }
}

void RemixWidget::OnFilterChanged()
{
  const QString needle = m_filter->text().trimmed().toLower();

  for (Row& row : m_rows)
  {
    row.visible = needle.isEmpty() || row.haystack.contains(needle);
    row.form->setRowVisible(row.control, row.visible);
  }

  bool diagnostics_has_match = false;
  for (QGroupBox* const box : m_boxes)
  {
    bool any = false;
    for (const Row& row : m_rows)
      any = any || (row.box == box && row.visible);

    box->setVisible(any);
    if (box == m_diagnostics_box)
      diagnostics_has_match = any;
  }

  // Collapsed by default, but a filter that matches something inside it would
  // otherwise look like it matched nothing.
  if (m_diagnostics_box != nullptr)
    m_diagnostics_box->setChecked(!needle.isEmpty() && diagnostics_has_match);
}

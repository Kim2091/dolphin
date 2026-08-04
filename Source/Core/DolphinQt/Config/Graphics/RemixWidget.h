// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include <QString>
#include <QWidget>

class GraphicsPane;
class QFormLayout;
class QGroupBox;
class QLineEdit;

namespace Config
{
class Layer;
struct RemixSettingMeta;
}  // namespace Config

// The Graphics dialog's Remix tab. Every knob the Remix backend has is emitted
// from the metadata table in Core/Config/RemixSettings.cpp, so this file knows
// nothing about individual settings and cannot fall out of step with them.
//
// The same widget serves both scopes: GraphicsPane is constructed with a null
// layer for the global dialog and with the game's INI layer for Properties ->
// Game Config -> Graphics, and the ConfigControl base class does the rest
// (reading through the layer, bolding a locally-set value, right-click to
// clear).
class RemixWidget final : public QWidget
{
  Q_OBJECT
public:
  explicit RemixWidget(GraphicsPane* gfx_pane);

private:
  // One editable knob, plus what the filter and the greying need to know about
  // it. `label` is null for checkboxes, which carry their own text.
  struct Row
  {
    QFormLayout* form;
    QGroupBox* box;
    QWidget* label;
    QWidget* control;
    const Config::RemixSettingMeta* meta;
    // Key name and tooltip, lowercased, i.e. what the filter searches.
    QString haystack;
    bool visible;
  };

  void CreateWidgets();
  void AddSetting(QGroupBox* box, QFormLayout* form, const Config::RemixSettingMeta& meta);

  void OnBackendChanged(const QString& backend_name);
  void OnEmulationStateChanged(bool running);
  void OnFilterChanged();
  // Greys out the knobs the running backend has already baked in. Re-applied
  // rather than set once, because QGroupBox re-enables its children whenever a
  // checkable box is ticked.
  void ApplyLiveness();

  Config::Layer* m_game_layer = nullptr;
  QWidget* m_content = nullptr;
  QLineEdit* m_filter = nullptr;
  QGroupBox* m_diagnostics_box = nullptr;
  QWidget* m_diagnostics_contents = nullptr;
  std::vector<QGroupBox*> m_boxes;
  std::vector<Row> m_rows;
  bool m_emulation_running = false;
};

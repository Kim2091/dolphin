// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QWidget>

#include <string>

class MainWindow;

namespace Config
{
class Layer;
}  // namespace Config

class GraphicsPane final : public QWidget
{
  Q_OBJECT
public:
  // game_id is set only when the pane is the per-game one in a game's Properties
  // dialog; empty in the global Graphics window.
  explicit GraphicsPane(MainWindow* main_window, Config::Layer* config_layer,
                        std::string game_id = {});

  Config::Layer* GetConfigLayer();
  const std::string& GetGameId() const { return m_game_id; }

signals:
  void BackendChanged(const QString& backend);
  void UseFastTextureSamplingChanged();
  void UpdateGPUTextureDecoding();

private:
  void CreateMainLayout();
  void OnBackendChanged(const QString& backend);

  MainWindow* const m_main_window;
  Config::Layer* const m_config_layer;
  const std::string m_game_id;
};

/*
 * This file is part of KSmoothDock.
 * Copyright (C) 2017 Viet Dang (dangvd@gmail.com)
 *
 * KSmoothDock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KSmoothDock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KSmoothDock.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KSMOOTHDOCK_DOCK_PANEL_H_
#define KSMOOTHDOCK_DOCK_PANEL_H_

#include <memory>
#include <vector>

#include <QAction>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <KAboutApplicationDialog>
#include <KWindowSystem>
#include <kactivities/consumer.h>

#include "add_panel_dialog.h"
#include "application_menu_settings_dialog.h"
#include "appearance_settings_dialog.h"
#include "dock_item.h"
#include "edit_launchers_dialog.h"
#include "task_manager_settings_dialog.h"
#include "tooltip.h"
#include "wallpaper_settings_dialog.h"
#include "utils/task_helper.h"

namespace ksmoothdock {

class MultiDockView;

// A dock panel. The user can have multiple dock panels at the same time.
class DockPanel : public QWidget {
  Q_OBJECT

 public:
  // No pointer ownership.
  DockPanel(MultiDockView* parent, MultiDockModel* model, int dockId);
  virtual ~DockPanel() = default;

  void resize(int w, int h);

  int dockId() const { return dockId_; }
  PanelPosition position() const { return position_; }

  QRect screenGeometry() { return screenGeometry_; }

  // Gets the position to show the application menu.
  QPoint applicationMenuPosition(const QSize& menuSize);
  // Gets the position to show the application menu's sub-menus.
  QPoint applicationSubMenuPosition(const QSize& menuSize,
                                       const QRect& subMenuGeometry);
  void addPanelSettings(QMenu* menu);

 public slots:
  // Reloads the items and updates the dock.
  void reload();

  // Checks that the items are still valid, removes an invalid one and updates the dock.
  // Should be called after a program with no task is unpinned.
  // Will return as soon as an invalid one is found.
  void refresh();
  void delayedRefresh();

  void onCurrentDesktopChanged();
  void onCurrentActivityChanged();

  void onDockLaunchersChanged(int dockId) {
    if (dockId_ == dockId) {
      reload();
    }
  }

  void setStrut();
  void setStrutForApplicationMenu();

  void updatePosition(PanelPosition position) {
    setPosition(position);
    reload();
    saveDockConfig();
  }

  void updateVisibility(PanelVisibility visibility) {
    setVisibility(visibility);
    reload();
    saveDockConfig();
  }

  void toggleApplicationMenu() {
    showApplicationMenu_ = !showApplicationMenu_;
    reload();
    saveDockConfig();
  }

  void togglePager();

  void updatePager() {
    if (showPager_) {
      reload();
    }
  }

  void toggleTaskManager() {
    model_->setShowTaskManager(dockId_, taskManagerAction_->isChecked());
    reload();
    saveDockConfig();
  }

  void toggleClock() {
    showClock_ = !showClock_;
    reload();
    saveDockConfig();
  }

  // Sets the dock on a specific screen given screen index.
  // Thus 0 is screen 1 and so on.
  // This doesn't refresh the dock.
  void setScreen(int screen);

  // Slot to update zoom animation.
  void updateAnimation();

  void showWaitCursor();
  void resetCursor();

  void showOnlineDocumentation();

  void about();

  // These are for global appearance settings.
  // Dock-specific settings are activated from menu items on the context menu
  // directly.
  void showAppearanceSettingsDialog();
  void showEditLaunchersDialog();
  void showApplicationMenuSettingsDialog();
  void showWallpaperSettingsDialog(int desktop);
  void showTaskManagerSettingsDialog();

  void addDock();
  void cloneDock();
  void removeDock();

  void onWindowAdded(WId wId);
  void onWindowRemoved(WId wId);
  void onWindowChanged(WId wId, NET::Properties properties,
                       NET::Properties2 properties2);

 protected:
  virtual void paintEvent(QPaintEvent* e) override;
  virtual void mouseMoveEvent(QMouseEvent* e) override;
  virtual void mousePressEvent(QMouseEvent* e) override;
  virtual void enterEvent(QEvent* e) override;
  virtual void leaveEvent(QEvent* e) override;

 private:
  // The space between the tooltip and the dock.
  static constexpr int kTooltipSpacing = 10;

  // Width/height of the panel in Auto Hide mode.
  static constexpr int kAutoHideSize = 1;

  bool isHorizontal() { return orientation_ == Qt::Horizontal; }

  bool autoHide() { return visibility_ == PanelVisibility::AutoHide; }
  bool windowsCanCover() {
    return visibility_ == PanelVisibility::WindowsCanCover;
  }

  void setPosition(PanelPosition position);

  void setVisibility(PanelVisibility visibility);

  int itemCount() const { return static_cast<int>(items_.size()); }

  int applicationMenuItemCount() const { return showApplicationMenu_ ? 1 : 0; }

  int launcherItemCount() const {
    return model_->dockLauncherConfigs(dockId_).size();
  }

  int pagerItemCount() const {
    return showPager_ ? KX11Extras::numberOfDesktops() : 0;
  }

  int clockItemCount() const {
    return showClock_ ? 1 : 0;
  }

  bool showTaskManager() { return model_->showTaskManager(dockId_); }

  void initUi();

  void createMenu();

  void loadDockConfig();
  void saveDockConfig();
  void loadAppearanceConfig();

  void initLaunchers();
  void initApplicationMenu();
  void initPager();
  void initTasks();
  void reloadTasks();
  void addTask(const TaskInfo& task);
  void addTask(WId wId) { addTask(taskHelper_.getTaskInfo(wId)); }
  void removeTask(WId wId);
  void updateTask(WId wId);
  void initClock();

  void initLayoutVars();

  // Updates width, height, items's size and position when the mouse is outside
  // the dock.
  void updateLayout();

  // Updates width, height, items's size and position given the mouse position.
  void updateLayout(int x, int y);

  // Resizes the task manager part of the panel. This needs to not interfere
  // with the zooming.
  void resizeTaskManager();

  void setStrut(int width);

  // Finds the active item given the mouse position.
  int findActiveItem(int x, int y);

  // Shows the appropriate tooltip given the mouse position.
  void showTooltip(int x, int y);
  // Shows tool tip for the item at the specified index.
  void showTooltip(int i);

  // Returns the size given the distance to the mouse.
  int parabolic(int x);

  MultiDockView* parent_;

  // The model.
  MultiDockModel* model_;
  int dockId_;

  // Config variables.

  PanelPosition position_;
  int screen_;  // the screen (as screen index) that the dock is on.
  PanelVisibility visibility_;
  bool showApplicationMenu_;
  bool showPager_;
  bool showClock_;
  int minSize_;
  int maxSize_;
  float spacingFactor_;  // item spacing as ratio of minSize, in (0, 1) range.
  QColor backgroundColor_;  // including alpha.
  bool showBorder_;
  QColor borderColor_;  // no alpha.
  int tooltipFontSize_;

  // Non-config variables.

  int itemSpacing_;
  int minX_;  // X-coordinate when minimized.
  int minY_;  // Y-coordinate when minimized.
  int minWidth_;
  int maxWidth_;
  int minHeight_;
  int maxHeight_;
  int parabolicMaxX_;
  QRect screenGeometry_;  // the geometry of the screen that the dock is on.

  // Number of animation steps when zooming in and out.
  int numAnimationSteps_;
  // Animation speed, between 0 and 31. The higher the faster.
  int animationSpeed_;

  Qt::Orientation orientation_;

  // The list of all dock items.
  std::vector<std::unique_ptr<DockItem>> items_;

  // Context (right-click) menu.
  QMenu menu_;
  QAction* positionTop_;
  QAction* positionBottom_;
  QAction* positionLeft_;
  QAction* positionRight_;
  QAction* visibilityAlwaysVisibleAction_;
  QAction* visibilityAutoHideAction_;
  QAction* visibilityWindowsCanCoverAction_;
  QAction* visibilityWindowsCanCoverQuietAction_;
  QAction* visibilityWindowsGoBelowAction_;
  QAction* applicationMenuAction_;
  QAction* pagerAction_;
  QAction* taskManagerAction_;
  QAction* clockAction_;
  // Actions to set the dock on a specific screen.
  std::vector<QAction*> screenActions_;

  KAboutApplicationDialog aboutDialog_;
  AddPanelDialog addPanelDialog_;
  AppearanceSettingsDialog appearanceSettingsDialog_;
  EditLaunchersDialog editLaunchersDialog_;
  ApplicationMenuSettingsDialog applicationMenuSettingsDialog_;
  WallpaperSettingsDialog wallpaperSettingsDialog_;
  TaskManagerSettingsDialog taskManagerSettingsDialog_;

  TaskHelper taskHelper_;
  KActivities::Consumer activityManager_;

  // The tooltip object to show tooltip for the active item.
  Tooltip tooltip_;

  bool isMinimized_;
  bool isResizing_;
  bool isEntering_;
  bool isLeaving_;
  bool isAnimationActive_;
  std::unique_ptr<QTimer> animationTimer_;
  int currentAnimationStep_;
  int backgroundWidth_;
  int startBackgroundWidth_;
  int endBackgroundWidth_;
  int backgroundHeight_;
  int startBackgroundHeight_;
  int endBackgroundHeight_;

  // For recording the mouse position before doing entering animation
  // so that we can show the correct tooltip at the end of it.
  int mouseX_;
  int mouseY_;

  friend class Program;  // for leaveEvent.
  friend class DockPanelTest;
  friend class ConfigDialogTest;
  friend class EditLaunchersDialogTest;
};

}  // namespace ksmoothdock

#endif  // KSMOOTHDOCK_DOCK_PANEL_H_

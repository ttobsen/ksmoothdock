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

#include "dock_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

#include <QColor>
#include <QCursor>
#include <QGuiApplication>
#include <QIcon>
#include <QListWidgetItem>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QSize>
#include <QStringList>
#include <QVariant>
#include <Qt>

#include <KAboutData>
#include <KLocalizedString>
#include <KMessageBox>
#include <netwm_def.h>

#include "add_panel_dialog.h"
#include "application_menu.h"
#include "clock.h"
#include "desktop_selector.h"
#include "multi_dock_view.h"
#include "program.h"
#include "separator.h"
#include <utils/command_utils.h>
#include <utils/task_helper.h>

namespace ksmoothdock {

const int DockPanel::kTooltipSpacing;
const int DockPanel::kAutoHideSize;

DockPanel::DockPanel(MultiDockView* parent, MultiDockModel* model, int dockId)
    : QWidget(),
      parent_(parent),
      model_(model),
      dockId_(dockId),
      visibility_(PanelVisibility::AlwaysVisible),
      showPager_(false),
      showClock_(false),
      showBorder_(true),
      aboutDialog_(KAboutData::applicationData(), this),
      addPanelDialog_(this, model, dockId),
      appearanceSettingsDialog_(this, model),
      editLaunchersDialog_(this, model, dockId),
      applicationMenuSettingsDialog_(this, model),
      wallpaperSettingsDialog_(this, model),
      taskManagerSettingsDialog_(this, model),
      isMinimized_(true),
      isResizing_(false),
      isEntering_(false),
      isLeaving_(false),
      isAnimationActive_(false),
      animationTimer_(std::make_unique<QTimer>(this)) {
  setAttribute(Qt::WA_TranslucentBackground);
  KWindowSystem::setType(winId(), NET::Dock);
  KX11Extras::setOnAllDesktops(winId(), true);
  setMouseTracking(true);
  createMenu();
  loadDockConfig();
  loadAppearanceConfig();
  initUi();

  connect(animationTimer_.get(), SIGNAL(timeout()), this,
      SLOT(updateAnimation()));
  connect(KWindowSystem::self(), SIGNAL(numberOfDesktopsChanged(int)),
      this, SLOT(updatePager()));
  connect(KWindowSystem::self(), SIGNAL(currentDesktopChanged(int)),
          this, SLOT(onCurrentDesktopChanged()));
  connect(KWindowSystem::self(), SIGNAL(activeWindowChanged(WId)),
          this, SLOT(update()));
  connect(KWindowSystem::self(), SIGNAL(windowAdded(WId)),
          this, SLOT(onWindowAdded(WId)));
  connect(KWindowSystem::self(), SIGNAL(windowRemoved(WId)),
          this, SLOT(onWindowRemoved(WId)));
  connect(KWindowSystem::self(),
          SIGNAL(windowChanged(WId, NET::Properties, NET::Properties2)),
          this,
          SLOT(onWindowChanged(WId, NET::Properties, NET::Properties2)));
  connect(&activityManager_, &KActivities::Consumer::currentActivityChanged,
          this, &DockPanel::onCurrentActivityChanged);
  connect(model_, SIGNAL(appearanceOutdated()), this, SLOT(update()));
  connect(model_, SIGNAL(appearanceChanged()), this, SLOT(reload()));
  connect(model_, SIGNAL(dockLaunchersChanged(int)),
          this, SLOT(onDockLaunchersChanged(int)));
}

void DockPanel::resize(int w, int h) {
  isResizing_ = true;
  QWidget::resize(w, h);
  int x, y;
  if (position_ == PanelPosition::Top) {
    x = (screenGeometry_.width() - w) / 2;
    y = 0;
  } else if (position_ == PanelPosition::Bottom) {
    x = (screenGeometry_.width() - w) / 2;
    y = screenGeometry_.height() - h;
  } else if (position_ == PanelPosition::Left) {
    x = 0;
    y = (screenGeometry_.height() - h) / 2;
  } else {  // Right
    x = screenGeometry_.width() - w;
    y = (screenGeometry_.height() - h) / 2;
  }
  if (isMinimized_) {
    move(x + screenGeometry_.x(), y + screenGeometry_.y());
  }
  if (w == minWidth_ && h == minHeight_) {
    minX_ = x + screenGeometry_.x();
    minY_ = y + screenGeometry_.y();
  }
  // This is to fix the bug that if launched from Plasma Quicklaunch,
  // KSmoothDock still doesn't show on all desktops even though
  // we've already called this in the constructor.
  KX11Extras::setOnAllDesktops(winId(), true);
  isResizing_ = false;
}

QPoint DockPanel::applicationMenuPosition(const QSize& menuSize) {
  if (position_ == PanelPosition::Top) {
    return QPoint(minX_, minY_ + minHeight_);
  } else if (position_ == PanelPosition::Bottom) {
    return QPoint(minX_, minY_ - menuSize.height());
  } else if (position_ == PanelPosition::Left) {
    return QPoint(minX_ + minWidth_, minY_);
  } else {  // Right
    return QPoint(minX_ - menuSize.width(), minY_);
  }
}

QPoint DockPanel::applicationSubMenuPosition(
    const QSize& menuSize, const QRect& subMenuGeometry) {
  if (position_ == PanelPosition::Top) {
    return QPoint(subMenuGeometry.x(),
                  std::min(subMenuGeometry.y(),
                           std::max(minY_ + minHeight_, minY_ + minHeight_ +
                               menuSize.height() - subMenuGeometry.height())));
  } else if (position_ == PanelPosition::Bottom) {
    return QPoint(subMenuGeometry.x(),
                  std::min(subMenuGeometry.y(),
                           minY_ - subMenuGeometry.height()));
  }
  // Left, Right.
  return QPoint(subMenuGeometry.x(), subMenuGeometry.y());  // no change.
}

void DockPanel::reload() {
  loadAppearanceConfig();
  items_.clear();
  initUi();
  update();
}

void DockPanel::refresh() {
  for (int i = 0; i < itemCount(); ++i) {
    if (items_[i]->shouldBeRemoved()) {
      items_.erase(items_.begin() + i);
      resizeTaskManager();
      return;
    }
  }
}

void DockPanel::delayedRefresh() {
  QTimer::singleShot(100 /* msecs */, this, SLOT(refresh()));
}

void DockPanel::onCurrentDesktopChanged() {
  reloadTasks();
}

void DockPanel::onCurrentActivityChanged() {
  reloadTasks();
}

void DockPanel::setStrut() {
  switch(visibility_) {
    case PanelVisibility::AlwaysVisible:
      setStrut(isHorizontal() ? minHeight_ : minWidth_);
      break;
    case PanelVisibility::AutoHide:  // fall through
    case PanelVisibility::WindowsCanCover:
      setStrut(1);
      break;
    case PanelVisibility::WindowsGoBelow:  // fall through
    case PanelVisibility::WindowsCanCover_Quiet:
    default:
      setStrut(0);
      break;
  }
}

void DockPanel::setStrutForApplicationMenu() {
  ApplicationMenu* applicationMenu = dynamic_cast<ApplicationMenu*>(
      items_[0].get());
  if (applicationMenu && model_->applicationMenuStrut()) {
    QSize menuSize = applicationMenu->getMenuSize();
    // For vertical positions, we reserve some space for the sub-menu as well.
    setStrut(isHorizontal() ? minHeight_ + menuSize.height()
                            : minWidth_ + 2 * menuSize.width());
  }
}

void DockPanel::togglePager() {
  showPager_ = !showPager_;
  reload();
  saveDockConfig();
}

void DockPanel::setScreen(int screen) {
  screen_ = screen;
  for (int i = 0; i < static_cast<int>(screenActions_.size()); ++i) {
    screenActions_[i]->setChecked(i == screen);
  }
  screenGeometry_ = QGuiApplication::screens()[screen]->geometry();
}

void DockPanel::updateAnimation() {
  for (const auto& item : items_) {
    item->nextAnimationStep();
  }
  ++currentAnimationStep_;
  backgroundWidth_ = startBackgroundWidth_
      + (endBackgroundWidth_ - startBackgroundWidth_)
          * currentAnimationStep_ / numAnimationSteps_;
  backgroundHeight_ = startBackgroundHeight_
      + (endBackgroundHeight_ - startBackgroundHeight_)
          * currentAnimationStep_ / numAnimationSteps_;
  if (currentAnimationStep_ == numAnimationSteps_) {
    animationTimer_->stop();
    isAnimationActive_ = false;
    if (isLeaving_) {
      isLeaving_ = false;
      updateLayout();
    } else {
      showTooltip(mouseX_, mouseY_);
    }
  }
  repaint();
}

void DockPanel::resetCursor() {
  setCursor(QCursor(Qt::ArrowCursor));
}

void DockPanel::showOnlineDocumentation() {
  Program::launch(
      "xdg-open https://github.com/dangvd/ksmoothdock/wiki/Documentation");
}

void DockPanel::about() {
  aboutDialog_.show();
  KX11Extras::forceActiveWindow(aboutDialog_.winId());
}

void DockPanel::showAppearanceSettingsDialog() {
  appearanceSettingsDialog_.reload();
  appearanceSettingsDialog_.show();
  KX11Extras::forceActiveWindow(appearanceSettingsDialog_.winId());
}

void DockPanel::showEditLaunchersDialog() {
  editLaunchersDialog_.reload();
  editLaunchersDialog_.show();
  KX11Extras::forceActiveWindow(editLaunchersDialog_.winId());
}

void DockPanel::showApplicationMenuSettingsDialog() {
  applicationMenuSettingsDialog_.reload();
  applicationMenuSettingsDialog_.show();
  KX11Extras::forceActiveWindow(applicationMenuSettingsDialog_.winId());
}

void DockPanel::showWallpaperSettingsDialog(int desktop) {
  wallpaperSettingsDialog_.setFor(desktop, screen_);
  wallpaperSettingsDialog_.show();
  KX11Extras::forceActiveWindow(wallpaperSettingsDialog_.winId());
}

void DockPanel::showTaskManagerSettingsDialog() {
  taskManagerSettingsDialog_.show();
  KX11Extras::forceActiveWindow(taskManagerSettingsDialog_.winId());
}

void DockPanel::addDock() {
  addPanelDialog_.setMode(AddPanelDialog::Mode::Add);
  addPanelDialog_.show();
}

void DockPanel::cloneDock() {
  addPanelDialog_.setMode(AddPanelDialog::Mode::Clone);
  addPanelDialog_.show();
}

void DockPanel::removeDock() {
  if (model_->dockCount() == 1) {
    KMessageBox::information(
        nullptr,
        i18n("The last panel cannot be removed."),
        i18n("Remove Panel"));
    return;
  }

  if (KMessageBox::questionYesNo(
        nullptr,
        i18n("Do you really want to remove this panel?"),
        i18n("Remove Panel"),
        KStandardGuiItem::yes(),
        KStandardGuiItem::no(),
        "confirmRemoveDock") == KMessageBox::Yes) {
    deleteLater();
    model_->removeDock(dockId_);
  }
}

void DockPanel::onWindowAdded(WId wId) {
  if (!showTaskManager()) {
    return;
  }

  if (taskHelper_.isValidTask(wId, screen_)) {
    // Now inserts it.
    addTask(wId);
    resizeTaskManager();
  }
}

void DockPanel::onWindowRemoved(WId wId) {
  if (!showTaskManager()) {
    return;
  }

  removeTask(wId);
}

void DockPanel::onWindowChanged(WId wId, NET::Properties properties,
                                NET::Properties2 properties2) {
  if (!showTaskManager()) {
    return;
  }

  if (wId != winId() && wId != tooltip_.winId() &&
      taskHelper_.isValidTask(wId)) {
    auto screen = model_->currentScreenTasksOnly() ? screen_ : -1;
    if (properties & NET::WMDesktop || properties & NET::WMGeometry) {
      if (taskHelper_.isValidTask(wId, screen, model_->currentDesktopTasksOnly())) {
        addTask(wId);
        resizeTaskManager();
      } else {
        removeTask(wId);
      }
    } else if (properties & NET::WMState) {
      updateTask(wId);
    }
  }
}

void DockPanel::paintEvent(QPaintEvent* e) {
  if (isResizing_) {
    return;  // to avoid potential flicker.
  }

  QPainter painter(this);

  if (isHorizontal()) {
    const int y = (position_ == PanelPosition::Top)
                  ? 0 : height() - backgroundHeight_;
    painter.fillRect((width() - backgroundWidth_) / 2, y,
                     backgroundWidth_, backgroundHeight_, backgroundColor_);

    if (showBorder_) {
      painter.setPen(borderColor_);
      painter.drawRect((width() - backgroundWidth_) / 2, y,
                       backgroundWidth_ - 1, backgroundHeight_ - 1);
    }
  } else {  // Vertical
    const int x =  (position_ == PanelPosition::Left)
                   ? 0 : width() - backgroundWidth_;
    painter.fillRect(x, (height() - backgroundHeight_) / 2,
                     backgroundWidth_, backgroundHeight_, backgroundColor_);

    if (showBorder_) {
      painter.setPen(borderColor_);
      painter.drawRect(x, (height() - backgroundHeight_) / 2,
                       backgroundWidth_ - 1, backgroundHeight_ - 1);
    }
  }

  // Draw the items from the end to avoid zoomed items getting clipped by
  // non-zoomed items.
  for (int i = itemCount() - 1; i >= 0; --i) {
    items_[i]->draw(&painter);
  }
}

void DockPanel::mouseMoveEvent(QMouseEvent* e) {
  if (isEntering_ && !autoHide()) {
    // Don't do the parabolic zooming if the mouse is near the border.
    // Quite often the user was just scrolling a window etc.
    if ((position_ == PanelPosition::Bottom && e->y() < itemSpacing_ / 2) ||
        (position_ == PanelPosition::Top && e->y() > height() - itemSpacing_ / 2) ||
        (position_ == PanelPosition::Left && e->x() > width() - itemSpacing_ / 2) ||
        (position_ == PanelPosition::Right && e->x() < itemSpacing_ / 2)) {
      return;
    }
  }

  if (isAnimationActive_) {
    return;
  }

  if (!isEntering_) {
    showTooltip(e->x(), e->y());
  }
  updateLayout(e->x(), e->y());
}

void DockPanel::mousePressEvent(QMouseEvent* e) {
  if (isAnimationActive_) {
    return;
  }

  int i = findActiveItem(e->x(), e->y());
  if (i < 0 || i >= itemCount()) {
    return;
  }

  items_[i]->mousePressEvent(e);
}

void DockPanel::enterEvent (QEvent* e) {
  isEntering_ = true;
  if (windowsCanCover()) {
    KWindowSystem::setState(winId(), NET::KeepAbove);
  }
}

void DockPanel::leaveEvent(QEvent* e) {
  if (windowsCanCover()) {
    KWindowSystem::setState(winId(), NET::KeepBelow);
  }

  if (isMinimized_) {
    return;
  }

  isLeaving_ = true;
  updateLayout();
  tooltip_.hide();
}

void DockPanel::initUi() {
  initApplicationMenu();
  initPager();
  initLaunchers();
  initTasks();
  initClock();
  initLayoutVars();
  updateLayout();
  setStrut();
}

void DockPanel::addPanelSettings(QMenu* menu) {
  QAction* action = menu->addMenu(&menu_);
  action->setText("&Panel Settings");
  action->setIcon(QIcon::fromTheme("configure"));
}

void DockPanel::createMenu() {
  menu_.addAction(QIcon::fromTheme("list-add"), i18n("&Add Panel"),
      this, SLOT(addDock()));
  menu_.addAction(QIcon::fromTheme("edit-copy"), i18n("&Clone Panel"),
      this, SLOT(cloneDock()));
  menu_.addAction(QIcon::fromTheme("edit-delete"), i18n("&Remove Panel"),
      this, SLOT(removeDock()));
  menu_.addSeparator();

  menu_.addAction(
      QIcon::fromTheme("configure"), i18n("Appearance &Settings"), this,
      SLOT(showAppearanceSettingsDialog()));

  QMenu* position = menu_.addMenu(i18n("&Position"));
  positionTop_ = position->addAction(i18n("&Top"), this,
      [this]() { updatePosition(PanelPosition::Top); });
  positionTop_->setCheckable(true);
  positionBottom_ = position->addAction(i18n("&Bottom"), this,
      [this]() { updatePosition(PanelPosition::Bottom); });
  positionBottom_->setCheckable(true);
  positionLeft_ = position->addAction(i18n("&Left"), this,
      [this]() { updatePosition(PanelPosition::Left); });
  positionLeft_->setCheckable(true);
  positionRight_ = position->addAction(i18n("&Right"), this,
      [this]() { updatePosition(PanelPosition::Right); });
  positionRight_->setCheckable(true);

  const int numScreens = QGuiApplication::screens().size();
  if (numScreens > 1) {
    QMenu* screen = menu_.addMenu(i18n("Scr&een"));
    for (int i = 0; i < numScreens; ++i) {
      QAction* action = screen->addAction(
          "Screen " + QString::number(i + 1), this,
          [this, i]() {
            setScreen(i);
            reload();
            saveDockConfig();
          });
      action->setCheckable(true);
      screenActions_.push_back(action);
    }
  }

  QMenu* visibility = menu_.addMenu(i18n("&Visibility"));
  visibilityAlwaysVisibleAction_ = visibility->addAction(
      i18n("Always &Visible"), this,
      [this]() { updateVisibility(PanelVisibility::AlwaysVisible); });
  visibilityAlwaysVisibleAction_->setCheckable(true);
  visibilityAutoHideAction_ = visibility->addAction(
      i18n("Auto &Hide"), this,
      [this]() { updateVisibility(PanelVisibility::AutoHide); });
  visibilityAutoHideAction_->setCheckable(true);
  visibilityWindowsCanCoverAction_ = visibility->addAction(
      i18n("Windows Can &Cover"), this,
      [this]() { updateVisibility(PanelVisibility::WindowsCanCover); });
  visibilityWindowsCanCoverAction_->setCheckable(true);
  visibilityWindowsCanCoverQuietAction_ = visibility->addAction(
      i18n("Windows Can Cover (&Quiet)"), this,
      [this]() { updateVisibility(PanelVisibility::WindowsCanCover_Quiet); });
  visibilityWindowsCanCoverQuietAction_->setCheckable(true);
  visibilityWindowsGoBelowAction_ = visibility->addAction(
      i18n("Windows Go &Below"), this,
      [this]() { updateVisibility(PanelVisibility::WindowsGoBelow); });
  visibilityWindowsGoBelowAction_->setCheckable(true);

  QMenu* extraComponents = menu_.addMenu(i18n("&Optional Features"));
  applicationMenuAction_ = extraComponents->addAction(i18n("Application Menu"), this,
      SLOT(toggleApplicationMenu()));
  applicationMenuAction_->setCheckable(true);
  pagerAction_ = extraComponents->addAction(i18n("Pager"), this,
      SLOT(togglePager()));
  pagerAction_->setCheckable(true);
  clockAction_ = extraComponents->addAction(i18n("Clock"), this,
      SLOT(toggleClock()));
  clockAction_->setCheckable(true);
  taskManagerAction_ = extraComponents->addAction(i18n("Show Running Tasks"), this,
      SLOT(toggleTaskManager()));
  taskManagerAction_->setCheckable(true);

  menu_.addSeparator();
  menu_.addAction(QIcon::fromTheme("help-contents"),
                  i18n("Online &Documentation"),
                  this, SLOT(showOnlineDocumentation()));
  menu_.addAction(QIcon::fromTheme("help-about"), i18n("A&bout KSmoothDock"),
      this, SLOT(about()));
  menu_.addSeparator();
  menu_.addAction(i18n("E&xit"), parent_, SLOT(exit()));
}

void DockPanel::setPosition(PanelPosition position) {
  position_ = position;
  orientation_ = (position_ == PanelPosition::Top ||
      position_ == PanelPosition::Bottom)
      ? Qt::Horizontal : Qt::Vertical;
  positionTop_->setChecked(position == PanelPosition::Top);
  positionBottom_->setChecked(position == PanelPosition::Bottom);
  positionLeft_->setChecked(position == PanelPosition::Left);
  positionRight_->setChecked(position == PanelPosition::Right);
}

void DockPanel::setVisibility(PanelVisibility visibility) {
  visibility_ = visibility;
  switch(visibility_) {
    case PanelVisibility::AlwaysVisible:  // fall through
    case PanelVisibility::AutoHide:  // fall through
    case PanelVisibility::WindowsGoBelow:
      KWindowSystem::setState(winId(), NET::KeepAbove);
      break;
    case PanelVisibility::WindowsCanCover:  // fall through
    case PanelVisibility::WindowsCanCover_Quiet:
      KWindowSystem::setState(winId(), NET::KeepBelow);
      break;
    default:
      break;
  }

  visibilityAlwaysVisibleAction_->setChecked(
      visibility_ == PanelVisibility::AlwaysVisible);
  visibilityAutoHideAction_->setChecked(
      visibility_ == PanelVisibility::AutoHide);
  visibilityWindowsCanCoverAction_->setChecked(
      visibility_ == PanelVisibility::WindowsCanCover);
  visibilityWindowsCanCoverQuietAction_->setChecked(
      visibility_ == PanelVisibility::WindowsCanCover_Quiet);
  visibilityWindowsGoBelowAction_->setChecked(
      visibility_ == PanelVisibility::WindowsGoBelow);
}

void DockPanel::loadDockConfig() {
  setPosition(model_->panelPosition(dockId_));
  setScreen(model_->screen(dockId_));
  setVisibility(model_->visibility(dockId_));

  showApplicationMenu_ = model_->showApplicationMenu(dockId_);
  applicationMenuAction_->setChecked(showApplicationMenu_);

  showPager_ = model_->showPager(dockId_);
  pagerAction_->setChecked(showPager_);

  taskManagerAction_->setChecked(model_->showTaskManager(dockId_));

  showClock_ = model_->showClock(dockId_);
  clockAction_->setChecked(showClock_);
}

void DockPanel::saveDockConfig() {
  model_->setPanelPosition(dockId_, position_);
  model_->setScreen(dockId_, screen_);
  model_->setVisibility(dockId_, visibility_);
  model_->setShowApplicationMenu(dockId_, showApplicationMenu_);
  model_->setShowPager(dockId_, showPager_);
  model_->setShowTaskManager(dockId_, taskManagerAction_->isChecked());
  model_->setShowClock(dockId_, showClock_);
  model_->saveDockConfig(dockId_);
}

void DockPanel::loadAppearanceConfig() {
  minSize_ = model_->minIconSize();
  maxSize_ = model_->maxIconSize();
  spacingFactor_ = model_->spacingFactor();
  backgroundColor_ = model_->backgroundColor();
  showBorder_ = model_->showBorder();
  borderColor_ = model_->borderColor();
  tooltipFontSize_ = model_->tooltipFontSize();
}

void DockPanel::initApplicationMenu() {
  if (showApplicationMenu_) {
    items_.push_back(std::make_unique<ApplicationMenu>(
        this, model_, orientation_, minSize_, maxSize_));
  }
}

void DockPanel::initLaunchers() {
  for (const auto& launcherConfig : model_->dockLauncherConfigs(dockId_)) {
    if (launcherConfig.command == "SEPARATOR") {
      items_.push_back(std::make_unique<Separator>(this, model_, orientation_, minSize_, maxSize_));
    } else {
      items_.push_back(std::make_unique<Program>(
          this, model_, launcherConfig.name, orientation_, launcherConfig.icon, minSize_,
          maxSize_, launcherConfig.command, launcherConfig.taskCommand, /*pinned=*/true));
    }
  }
}

void DockPanel::initPager() {
  if (showPager_) {
    for (int desktop = 1; desktop <= KX11Extras::numberOfDesktops();
         ++desktop) {
      items_.push_back(std::make_unique<DesktopSelector>(
          this, model_, orientation_, minSize_, maxSize_, desktop, screen_));
    }
  }
}

void DockPanel::initTasks() {
  if (!showTaskManager()) {
    return;
  }

  auto screen = model_->currentScreenTasksOnly() ? screen_ : -1;
  for (const auto& task : taskHelper_.loadTasks(screen, model_->currentDesktopTasksOnly())) {
    addTask(task);
  }
}

void DockPanel::reloadTasks() {
  if (!showTaskManager()) {
    return;
  }

  const int itemsToKeep = applicationMenuItemCount() + pagerItemCount();
  items_.resize(itemsToKeep);
  initLaunchers();
  initTasks();
  initClock();
  resizeTaskManager();
}

void DockPanel::addTask(const TaskInfo& task) {
  // Checks is the task already exists.
  for (auto& item : items_) {
    if (item->hasTask(task.wId)) {
      return;
    }
  }

  // Tries adding the task to existing programs.
  for (auto& item : items_) {
    if (item->addTask(task)) {
      return;
    }
  }

  // Adds a new program.
  auto app = model_->findApplication(task.command);
  const QString& command = app ? app->taskCommand : task.command;
  int i = 0;
  for (; i < itemCount() && items_[i]->beforeTask(command); ++i);
  if (app) {
    items_.insert(items_.begin() + i, std::make_unique<Program>(
        this, model_, app->name, orientation_, app->icon, minSize_,
        maxSize_, app->command, app->taskCommand, /*pinned=*/false));
  } else {
    items_.insert(items_.begin() + i, std::make_unique<Program>(
        this, model_, task.program, orientation_, "xapp", minSize_,
        maxSize_, task.command, task.command, /*pinned=*/false));
  }
  items_[i]->addTask(task);
}

void DockPanel::removeTask(WId wId) {
  for (int i = 0; i < itemCount(); ++i) {
    if (items_[i]->removeTask(wId)) {
      if (items_[i]->shouldBeRemoved()) {
        items_.erase(items_.begin() + i);
        resizeTaskManager();
      }
      return;
    }
  }
}

void DockPanel::updateTask(WId wId) {
  const TaskInfo& task = taskHelper_.getTaskInfo(wId);
  for (auto& item : items_) {
    if (item->updateTask(task)) {
      return;
    }
  }
}

void DockPanel::initClock() {
  if (showClock_) {
    items_.push_back(std::make_unique<Clock>(
        this, model_, orientation_, minSize_, maxSize_));
  }
}

void DockPanel::initLayoutVars() {
  itemSpacing_ = static_cast<int>(minSize_ * spacingFactor_);
  parabolicMaxX_ = static_cast<int>(2.5 * (minSize_ + itemSpacing_));
  numAnimationSteps_ = 20;
  animationSpeed_ = 16;

  tooltip_.setFontSize(tooltipFontSize_);
  tooltip_.setFontBold(true);
  tooltip_.setFontColor(Qt::white);
  tooltip_.setBackgroundColor(Qt::black);

  const int distance = minSize_ + itemSpacing_;
  // The difference between minWidth_ and maxWidth_
  // (horizontal mode) or between minHeight_ and
  // maxHeight_ (vertical mode).
  int delta = 0;
  if (itemCount() >= 5) {
    delta = parabolic(0) + 2 * parabolic(distance) +
        2 * parabolic(2 * distance) - 5 * minSize_;
  } else if (itemCount() == 4) {
    delta = parabolic(0) + 2 * parabolic(distance) +
        parabolic(2 * distance) - 4 * minSize_;
  } else if (itemCount() == 3) {
    delta = parabolic(0) + 2 * parabolic(distance) - 3 * minSize_;
  } else if (itemCount() == 2) {
    delta = parabolic(0) + parabolic(distance) - 2 * minSize_;
  } else if (itemCount() == 1) {
    delta = parabolic(0) - minSize_;
  }

  if (orientation_ == Qt::Horizontal) {
    minWidth_ = 0;
    for (const auto& item : items_) {
      minWidth_ += (item->getMinWidth() + itemSpacing_);
    }
    minHeight_ = autoHide() ? kAutoHideSize : distance;
    maxWidth_ = minWidth_ + delta;
    maxHeight_ = itemSpacing_ + maxSize_;
  } else {  // Vertical
    minHeight_ = 0;
    for (const auto& item : items_) {
      minHeight_ += (item->getMinHeight() + itemSpacing_);
    }
    minWidth_ = autoHide() ? kAutoHideSize : distance;
    maxHeight_ = minHeight_ + delta;
    maxWidth_ = itemSpacing_ + maxSize_;
  }
}

void DockPanel::updateLayout() {
  const int distance = minSize_ + itemSpacing_;
  if (isLeaving_) {
    for (const auto& item : items_) {
      item->setAnimationStartAsCurrent();
      if (isHorizontal()) {
        startBackgroundWidth_ = backgroundWidth_;
        startBackgroundHeight_ = distance;
      } else {  // Vertical
        startBackgroundHeight_ = backgroundHeight_;
        startBackgroundWidth_ = distance;
      }
    }
  }

  for (int i = 0; i < itemCount(); ++i) {
    items_[i]->size_ = minSize_;
    if (isHorizontal()) {
      items_[i]->left_ = (i == 0) ? itemSpacing_ / 2
          : items_[i - 1]->left_ + items_[i - 1]->getMinWidth() + itemSpacing_;
      items_[i]->top_ = itemSpacing_ / 2;
      items_[i]->minCenter_ = items_[i]->left_ + items_[i]->getMinWidth() / 2;
    } else {  // Vertical
      items_[i]->left_ = itemSpacing_ / 2;
      items_[i]->top_ = (i == 0) ? itemSpacing_ / 2
          : items_[i - 1]->top_ + items_[i - 1]->getMinHeight() + itemSpacing_;
      items_[i]->minCenter_ = items_[i]->top_ + items_[i]->getMinHeight() / 2;
    }
  }
  if (isHorizontal()) {
    backgroundWidth_ = minWidth_;
    backgroundHeight_ = distance;
  } else {  // Vertical
    backgroundHeight_ = minHeight_;
    backgroundWidth_ = distance;
  }

  if (isLeaving_) {
    for (const auto& item : items_) {
      item->endSize_ = item->size_;
      if (isHorizontal()) {
        item->endLeft_ = item->left_ + (screenGeometry_.width() - minWidth_) / 2
            - x() + screenGeometry_.x();
        if (position_ == PanelPosition::Top) {
          item->endTop_ = item->top_ + minHeight_ - distance;
        } else {  // Bottom
          item->endTop_ = item->top_ + (maxHeight_ - minHeight_);
        }
      } else {  // Vertical
        item->endTop_ = item->top_ + (screenGeometry_.height() - minHeight_) / 2
            - y() + screenGeometry_.y();
        if (position_ == PanelPosition::Left) {
          item->endLeft_ = item->left_ + minWidth_ - distance;
        } else {  // Right
          item->endLeft_ = item->left_ + (maxWidth_ - minWidth_);
        }
      }
      item->startAnimation(numAnimationSteps_);
    }
    if (isHorizontal()) {
      endBackgroundWidth_ = minWidth_;
      backgroundWidth_ = startBackgroundWidth_;
      endBackgroundHeight_ = autoHide() ? kAutoHideSize : distance;
      backgroundHeight_ = startBackgroundHeight_;
    } else {  // Vertical
      endBackgroundHeight_ = minHeight_;
      backgroundHeight_ = startBackgroundHeight_;
      endBackgroundWidth_ = autoHide() ? kAutoHideSize : distance;
      backgroundWidth_ = startBackgroundWidth_;
    }
    currentAnimationStep_ = 0;
    isAnimationActive_ = true;
    animationTimer_->start(32 - animationSpeed_);
  } else {
    isMinimized_ = true;
    resize(minWidth_, minHeight_);
    update();
  }
}

void DockPanel::updateLayout(int x, int y) {
  const int distance = minSize_ + itemSpacing_;
  if (isEntering_) {
    for (const auto& item : items_) {
      item->startSize_ = item->size_;
      if (isHorizontal()) {
        item->startLeft_ = item->left_ + (maxWidth_ - minWidth_) / 2;
        if (position_ == PanelPosition::Top) {
          item->startTop_ = item->top_ + minHeight_ - distance;
        } else {  // Bottom
          item->startTop_ = item->top_ + (maxHeight_ - minHeight_);
        }
      } else {  // Vertical
        item->startTop_ = item->top_ + (maxHeight_ - minHeight_) / 2;
        if (position_ == PanelPosition::Left) {
          item->startLeft_ = item->left_ + minWidth_ - distance;
        } else {  // Right
          item->startLeft_ = item->left_ + (maxWidth_ - minWidth_);
        }
      }
    }
    if (isHorizontal()) {
      startBackgroundWidth_ = minWidth_;
      startBackgroundHeight_ = autoHide() ? kAutoHideSize : distance;
    } else {  // Vertical
      startBackgroundHeight_ = minHeight_;
      startBackgroundWidth_ = autoHide() ? kAutoHideSize : distance;
    }
  }

  int first_update_index = -1;
  int last_update_index = 0;
  if (isHorizontal()) {
    items_[0]->left_ = itemSpacing_ / 2;
  } else {  // Vertical
    items_[0]->top_ = itemSpacing_ / 2;
  }
  for (int i = 0; i < itemCount(); ++i) {
    int delta;
    if (isHorizontal()) {
      delta = std::abs(items_[i]->minCenter_ - x + (width() - minWidth_) / 2);
    } else {  // Vertical
      delta = std::abs(items_[i]->minCenter_ - y + (height() - minHeight_) / 2);
    }
    if (delta < parabolicMaxX_) {
      if (first_update_index == -1) {
        first_update_index = i;
      }
      last_update_index = i;
    }
    items_[i]->size_ = parabolic(delta);
    if (position_ == PanelPosition::Top) {
      items_[i]->top_ = itemSpacing_ / 2;
    } else if (position_ == PanelPosition::Bottom) {
      items_[i]->top_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getHeight();
    } else if (position_ == PanelPosition::Left) {
      items_[i]->left_ = itemSpacing_ / 2;
    } else {  // Right
      items_[i]->left_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getWidth();
    }
    if (i > 0) {
      if (isHorizontal()) {
        items_[i]->left_ = items_[i - 1]->left_ + items_[i - 1]->getWidth()
            + itemSpacing_;
      } else {  // Vertical
        items_[i]->top_ = items_[i - 1]->top_ + items_[i - 1]->getHeight()
            + itemSpacing_;
      }
    }
  }
  for (int i = itemCount() - 1; i >= last_update_index + 1; --i) {
    if (isHorizontal()) {
      items_[i]->left_ = (i == itemCount() - 1)
          ? maxWidth_ - itemSpacing_ / 2 - items_[i]->getMinWidth()
          : items_[i + 1]->left_ - items_[i]->getMinWidth() - itemSpacing_;
    } else {  // Vertical
      items_[i]->top_ = (i == itemCount() - 1)
          ? maxHeight_ - itemSpacing_ / 2 - items_[i]->getMinHeight()
          : items_[i + 1]->top_ - items_[i]->getMinHeight() - itemSpacing_;
    }
  }
  if (first_update_index == 0 && last_update_index < itemCount() - 1) {
    for (int i = last_update_index; i >= first_update_index; --i) {
      if (isHorizontal()) {
        items_[i]->left_ = items_[i + 1]->left_ - items_[i]->getWidth()
            - itemSpacing_;
      } else {  // Vertical
        items_[i]->top_ = items_[i + 1]->top_ - items_[i]->getHeight()
            - itemSpacing_;
      }
    }
  }

  if (isEntering_) {
    for (const auto& item : items_) {
      item->setAnimationEndAsCurrent();
      item->startAnimation(numAnimationSteps_);
    }
    if (isHorizontal()) {
      endBackgroundWidth_ = maxWidth_;
      backgroundWidth_ = startBackgroundWidth_;
      endBackgroundHeight_ = distance;
      backgroundHeight_ = startBackgroundHeight_;
      mouseX_ = x + (maxWidth_ - minWidth_) / 2;
    } else {  // Vertical
      endBackgroundHeight_ = maxHeight_;
      backgroundHeight_ = startBackgroundHeight_;
      endBackgroundWidth_ = distance;
      backgroundWidth_ = startBackgroundWidth_;
      mouseY_ = y + (maxHeight_ - minHeight_) / 2;
    }

    currentAnimationStep_ = 0;
    isAnimationActive_ = true;
    isEntering_ = false;
    animationTimer_->start(32 - animationSpeed_);
  } else {
    mouseX_ = x;
    mouseY_ = y;
  }

  resize(maxWidth_, maxHeight_);
  isMinimized_ = false;
  update();
}

void DockPanel::resizeTaskManager() {
  // Re-calculate panel's size.
  initLayoutVars();

  if (isMinimized_) {
    updateLayout();
    return;
  } else {
    // Need to call QWidget::resize(), not DockPanel::resize(), in order not to
    // mess up the zooming.
    QWidget::resize(maxWidth_, maxHeight_);
    if (isHorizontal()) {
      backgroundWidth_ = maxWidth_;
    } else {
      backgroundHeight_ = maxHeight_;
    }
  }

  const int itemsToKeep = (showApplicationMenu_ ? 1 : 0) +
      (showPager_ ? KX11Extras::numberOfDesktops() : 0);
  int left = 0;
  int top = 0;
  for (int i = 0; i < itemCount(); ++i) {
    if (isHorizontal()) {
      left = (i == 0) ? itemSpacing_ / 2
                      : left + items_[i - 1]->getMinWidth() + itemSpacing_;
      if (i >= itemsToKeep) {
        items_[i]->minCenter_ = left + items_[i]->getMinWidth() / 2;
      }
    } else {  // Vertical
      top = (i == 0) ? itemSpacing_ / 2
                     : top + items_[i - 1]->getMinHeight() + itemSpacing_;
      if (i >= itemsToKeep) {
        items_[i]->minCenter_ = top + items_[i]->getMinHeight() / 2;
      }
    }
  }

  int last_update_index = 0;
  for (int i = itemsToKeep; i < itemCount(); ++i) {
    int delta;
    if (isHorizontal()) {
      delta = std::abs(items_[i]->minCenter_ - mouseX_ +
                       (width() - minWidth_) / 2);
    } else {  // Vertical
      delta = std::abs(items_[i]->minCenter_ - mouseY_ +
                       (height() - minHeight_) / 2);
    }
    if (delta < parabolicMaxX_) {
      last_update_index = i;
    }
    items_[i]->size_ = parabolic(delta);
    if (position_ == PanelPosition::Top) {
      items_[i]->top_ = itemSpacing_ / 2;
    } else if (position_ == PanelPosition::Bottom) {
      items_[i]->top_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getHeight();
    } else if (position_ == PanelPosition::Left) {
      items_[i]->left_ = itemSpacing_ / 2;
    } else {  // Right
      items_[i]->left_ = itemSpacing_ / 2 + maxSize_ - items_[i]->getWidth();
    }
    if (i > 0) {
      if (isHorizontal()) {
        items_[i]->left_ = items_[i - 1]->left_ + items_[i - 1]->getWidth()
            + itemSpacing_;
      } else {  // Vertical
        items_[i]->top_ = items_[i - 1]->top_ + items_[i - 1]->getHeight()
            + itemSpacing_;
      }
    }
  }

  for (int i = itemCount() - 1;
       i >= std::max(itemsToKeep, last_update_index + 1); --i) {
    if (isHorizontal()) {
      items_[i]->left_ = (i == itemCount() - 1)
          ? maxWidth_ - itemSpacing_ / 2 - items_[i]->getMinWidth()
          : items_[i + 1]->left_ - items_[i]->getMinWidth() - itemSpacing_;
    } else {  // Vertical
      items_[i]->top_ = (i == itemCount() - 1)
          ? maxHeight_ - itemSpacing_ / 2 - items_[i]->getMinHeight()
          : items_[i + 1]->top_ - items_[i]->getMinHeight() - itemSpacing_;
    }
  }

  update();
}

void DockPanel::setStrut(int width) {
  // Somehow if we use setExtendedStrut() as below when screen_ is 0,
  // the strut extends the whole combined desktop instead of just the first
  // screen.
  if (screen_ == 0) {
    if (position_ == PanelPosition::Top) {
      KX11Extras::setStrut(winId(), 0, 0, width, 0);
    } else if (position_ == PanelPosition::Bottom) {
      KX11Extras::setStrut(winId(), 0, 0, 0, width);
    } else if (position_ == PanelPosition::Left) {
      KX11Extras::setStrut(winId(), width, 0, 0, 0);
    } else {  // Right
      KX11Extras::setStrut(winId(), 0, width, 0, 0);
    }
    return;
  }

  if (position_ == PanelPosition::Top) {
    KX11Extras::setExtendedStrut(
        winId(),
        0, 0, 0,
        0, 0, 0,
        width,
        screenGeometry_.x(),
        screenGeometry_.x() + screenGeometry_.width(),
        0, 0, 0);
  } else if (position_ == PanelPosition::Bottom) {
    KX11Extras::setExtendedStrut(
        winId(),
        0, 0, 0,
        0, 0, 0,
        0, 0, 0,
        width,
        screenGeometry_.x(),
        screenGeometry_.x() + screenGeometry_.width());
  } else if (position_ == PanelPosition::Left) {
    KX11Extras::setExtendedStrut(
        winId(),
        width,
        screenGeometry_.y(),
        screenGeometry_.y() + screenGeometry_.height(),
        0, 0, 0,
        0, 0, 0,
        0, 0, 0);
  } else {  // Right
    KX11Extras::setExtendedStrut(
        winId(),
        0, 0, 0,
        width,
        screenGeometry_.y(),
        screenGeometry_.y() + screenGeometry_.height(),
        0, 0, 0,
        0, 0, 0);
  }
}

int DockPanel::findActiveItem(int x, int y) {
  int i = 0;
  while (i < itemCount() &&
      ((orientation_ == Qt::Horizontal && items_[i]->left_ < x) ||
      (orientation_ == Qt::Vertical && items_[i]->top_ < y))) {
    ++i;
  }
  return i - 1;
}

void DockPanel::showTooltip(int x, int y) {
  int i = findActiveItem(x, y);
  if (i < 0 || i >= itemCount()) {
    tooltip_.hide();
  } else {
    showTooltip(i);
    // Somehow we have to set this property again when re-showing.
    KX11Extras::setOnAllDesktops(tooltip_.winId(), true);
  }
}

void DockPanel::showTooltip(int i) {
  tooltip_.setText(items_[i]->getLabel());
  int x, y;
  if (position_ == PanelPosition::Top) {
    x = geometry().x() + items_[i]->left_
        - tooltip_.width() / 2 + items_[i]->getWidth() / 2;
    y = geometry().y() + maxHeight_ + kTooltipSpacing;
  } else if (position_ == PanelPosition::Bottom) {
    x = geometry().x() + items_[i]->left_
        - tooltip_.width() / 2 + items_[i]->getWidth() / 2;
    // No need for additional tooltip spacing in this position.
    y = geometry().y() - tooltip_.height();
  } else if (position_ == PanelPosition::Left) {
    x = geometry().x() + maxWidth_ + kTooltipSpacing;
    y = geometry().y() + items_[i]->top_
        - tooltip_.height() / 2 + items_[i]->getHeight() / 2;
  } else {  // Right
    x = geometry().x() - tooltip_.width() - kTooltipSpacing;
    y = geometry().y() + items_[i]->top_
        - tooltip_.height() / 2 + items_[i]->getHeight() / 2;
  }
  tooltip_.move(x, y);
  tooltip_.show();
}

void DockPanel::showWaitCursor() {
  setCursor(QCursor(Qt::WaitCursor));
  QTimer::singleShot(1000 /* msecs */, this, SLOT(resetCursor()));
}

int DockPanel::parabolic(int x) {
  // Assume x >= 0.
  if (x > parabolicMaxX_) {
    return minSize_;
  } else {
    return maxSize_ -
        (x * x * (maxSize_ - minSize_)) / (parabolicMaxX_ * parabolicMaxX_);
  }
}

}  // namespace ksmoothdock

#pragma once
#include "mainwindow.h"
#include "home.h"
#include "home_status_presenter.h"
#include "configmanager.h"
#include "loggingmanager.h"
#include "trackingtypes.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QJsonObject>
#include <QKeyEvent>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>
#include <functional>
#include <memory>
#include <vector>

inline void installGuiSmoke(QApplication& app, MainWindow& window, const QString& output) {
    QDir().mkpath(output);
    auto phase = std::make_shared<size_t>(0);
    auto steps = std::make_shared<std::vector<std::function<void()>>>();
    auto screenshots = std::make_shared<QJsonArray>();
    auto maintenancePages = std::make_shared<QJsonArray>();
    auto maintenanceSubPages = std::make_shared<QJsonArray>();
    auto ipcMaintenancePages = std::make_shared<QJsonArray>();
    auto mapEditorPages = std::make_shared<QJsonArray>();
    auto cameraControlLayouts = std::make_shared<QJsonArray>();
    auto maintenanceLeaveChecks = std::make_shared<QJsonObject>();
    auto helpPages = std::make_shared<QJsonArray>();
    auto* timer = new QTimer(&window);
    auto click = [&window](const char* name) {
        auto* button = window.findChild<QPushButton*>(name);
        if (!button) qFatal("Missing navigation button");
        button->click();
    };
    auto capture = [&window, output, screenshots](const QString& name) {
        if (!window.grab().save(QDir(output).filePath(name + ".png"))) qFatal("Screenshot failed");
        screenshots->append(name);
    };
    const auto checkFullyVisible = [&window](QWidget *control) {
        if (!control || !control->isVisible()) qFatal("Required action or guidance is hidden");
        for (auto *parent = control->parentWidget(); parent; parent = parent->parentWidget()) {
            if (!parent->rect().contains(QRect(control->mapTo(parent, QPoint()), control->size())))
                qFatal("Required action or guidance is clipped by its page or window");
            if (parent == &window) break;
        }
    };
    auto checkConfigVisibility = [&window](bool expected) {
        int regionCount = 0;
        for (auto *region : window.findChildren<QWidget *>()) {
            if (!region->property("maintenanceConfigRegion").toBool()) continue;
            ++regionCount;
            if (region->isVisible() != expected) qFatal("Maintenance configuration visibility mismatch");
        }
        if (regionCount != 2) qFatal("Missing maintenance configuration regions");
    };
    auto scrollTo = [](QWidget *page, double fraction) {
        QScrollArea *scroll = qobject_cast<QScrollArea *>(page);
        if (!scroll) {
            for (auto *candidate : page->findChildren<QScrollArea *>()) {
                if (candidate->isVisible()) { scroll = candidate; break; }
            }
        }
        if (!scroll) qFatal("Missing scroll area for visible page");
        auto *bar = scroll->verticalScrollBar();
        bar->setValue(qRound(bar->maximum() * fraction));
    };
    auto *tabs = window.findChild<QTabWidget*>("maintenanceTabWidget");
    auto *maintenanceNav = window.findChild<QListWidget*>("maintenanceNavList");
    auto *helpNav = window.findChild<QListWidget*>("helpNavList");
    auto *helpRoot = window.findChild<QWidget*>("helpRoot");
    auto *aboutRoot = window.findChild<QWidget*>("aboutRoot");
    auto *ipcTabs = window.findChild<QTabWidget*>("ipcMaintenanceTabs");
    auto *taskTabs = window.findChild<QTabWidget*>("mapTaskTabWidget");
    auto *editorTabs = window.findChild<QTabWidget*>("mapEditorTabs");
    auto *taskPanel = window.findChild<QWidget*>("mapTaskExecutionPanel");
    if (!tabs || !maintenanceNav || !helpNav || !helpRoot || !aboutRoot)
        qFatal("Missing maintenance/help/about navigation");
    if (maintenanceNav->count() != tabs->count()) qFatal("Maintenance navigation page count mismatch");
    const QStringList ipcTitles{QStringLiteral("服务管理"), QStringLiteral("融合与导航参数"),
        QStringLiteral("配置备份"), QStringLiteral("相机采集"), QStringLiteral("车辆与原点 · 只读")};
    if (!ipcTabs || ipcTabs->count() != ipcTitles.size()) qFatal("Missing IPC maintenance pages");
    for (int i = 0; i < ipcTitles.size(); ++i)
        if (ipcTabs->tabText(i) != ipcTitles[i]) qFatal("IPC maintenance page title mismatch");
    if (!taskTabs || taskTabs->count() != 2 || taskTabs->tabText(0) != QStringLiteral("常规任务") ||
        taskTabs->tabText(1) != QStringLiteral("直线任务") || !taskPanel || taskTabs->isAncestorOf(taskPanel))
        qFatal("Map must offer two planning pages with one fixed execution panel");
    if (window.findChildren<QWidget *>("mapTaskExecutionPanel").size() != 1 ||
        window.findChild<QWidget *>("mapRowWorkModeTabWidget") || window.findChild<QWidget *>("mapRowWorkMissionPage") ||
        window.findChild<QWidget *>("mapRowMissionStepTable")) qFatal("Retired mission UI or duplicate execution panels remain");
    for (const char *name : {"mapTrackingStatusLabel", "mapTaskProgressLabel", "mapTaskNoticeLabel"})
        if (!taskPanel->findChild<QLabel *>(name)) qFatal("Missing centralized task execution feedback");
    const QStringList editorTitles{QStringLiteral("点位"), QStringLiteral("路径"), QStringLiteral("地图")};
    if (!editorTabs || editorTabs->count() != editorTitles.size()) qFatal("Missing separate map editor pages");
    for (int i = 0; i < editorTitles.size(); ++i)
        if (editorTabs->tabText(i) != editorTitles[i]) qFatal("Map editor title mismatch");
    if (window.findChild<QMenu *>("mapTaskAdvancedMenu") ||
        window.findChild<QAction *>("mapOnlyUploadAction"))
        qFatal("Retired advanced task menu remains");
    for (auto *button : taskPanel->findChildren<QPushButton *>())
        if (button->text().contains(QStringLiteral("高级工具"))) qFatal("Retired advanced task button remains");
    auto *frameBinding = window.findChild<QPushButton *>("mapFrameBindingButton");
    auto *photoReview = taskPanel->findChild<QPushButton *>("mapCheckpointReviewButton");
    if (!frameBinding || !editorTabs->widget(2)->isAncestorOf(frameBinding))
        qFatal("Map coordinate adaptation must be under map properties");
    if (!photoReview || !photoReview->isHidden()) qFatal("Photo confirmation must stay hidden without an unknown event");
    const auto checkTaskPanel = [taskPanel, taskTabs, &window, checkFullyVisible] {
        if (!taskPanel->isVisible() || window.findChild<QWidget *>("mapTaskExecutionPanel") != taskPanel)
            qFatal("Changing task page or resizing hid the fixed execution panel");
        checkFullyVisible(taskPanel);
        const auto prefix = taskTabs->currentIndex() == 0 ? QStringLiteral("mapRoute") : QStringLiteral("mapRowWork");
        auto *startAction = taskTabs->currentWidget()->findChild<QPushButton *>(prefix + "StartButton");
        auto *startHint = taskTabs->currentWidget()->findChild<QLabel *>(prefix + "StartHintLabel");
        checkFullyVisible(startAction);
        checkFullyVisible(startHint);
        if (startAction->isEnabled() || startHint->text().trimmed().isEmpty())
            qFatal("Offline task action must be disabled with visible guidance");
        if (taskTabs->currentIndex() != 1) return;
        auto *page = taskTabs->currentWidget();
        auto *start = page->findChild<QPushButton *>("mapRowWorkStartButton");
        auto *scroll = page->findChild<QScrollArea *>("mapRowWorkPrimitiveScrollArea");
        if (!start || !scroll || !start->isVisibleTo(page))
            qFatal("Straight task execution action is missing or hidden");
        for (auto *parent = start->parentWidget(); parent && parent != page; parent = parent->parentWidget())
            if (qobject_cast<QScrollArea *>(parent))
                qFatal("Straight task execution action must stay outside scrolling content");
        const QRect startBounds(start->mapTo(page, QPoint()), start->size());
        if (!page->rect().contains(startBounds))
            qFatal("Straight task execution action is clipped by its page");
        if (scroll->horizontalScrollBar()->maximum() != 0)
            qFatal("Straight task form requires horizontal scrolling");
    };
    for (int i=0; i<tabs->count(); ++i)
        if (maintenanceNav->item(i)->text() != tabs->tabText(i))
            qFatal("Maintenance navigation title mismatch");
    if (!aboutRoot->findChild<QScrollArea*>("aboutScrollArea")) qFatal("Missing about scroll area");
    const auto *logDirectory = aboutRoot->findChild<QLabel *>("aboutLogDirectory");
    if (!logDirectory || logDirectory->text() != LoggingManager::logDirectoryPath())
        qFatal("About must show the actual executable-relative log directory");
    for (auto *widget : window.findChildren<QWidget *>()) {
        const auto key = widget->property("maintenanceConfigKey").toString();
        if (key.startsWith("taskDefaults.") || key.startsWith("geo.") || key.startsWith("vehicle."))
            qFatal("Retired IPC parameters must not have local editors");
    }
    QComboBox *videoBackend = nullptr;
    QSpinBox *localCameraWidth = nullptr;
    QLineEdit *cameraServiceAddress = nullptr;
    QLineEdit *genericStreamAddress = nullptr;
    for (auto *widget : window.findChildren<QWidget *>()) {
        const auto key = widget->property("maintenanceConfigKey").toString();
        if (key == "video.backend") videoBackend = qobject_cast<QComboBox *>(widget);
        if (key == "video.previewWidth") localCameraWidth = qobject_cast<QSpinBox *>(widget);
        if (key == "video.controlBaseUrl") cameraServiceAddress = qobject_cast<QLineEdit *>(widget);
        if (key == "video.streamUrl") genericStreamAddress = qobject_cast<QLineEdit *>(widget);
    }
    if (!videoBackend || !localCameraWidth || !cameraServiceAddress || !genericStreamAddress)
        qFatal("Missing camera backend or address controls");
    auto *localCameraRow = localCameraWidth->parentWidget();
    auto *serviceAddressRow = cameraServiceAddress->parentWidget();
    auto *genericStreamRow = genericStreamAddress->parentWidget();
    const QString originalBackend = videoBackend->currentText();
    const QString originalServiceAddress = cameraServiceAddress->text();
    const QString originalGenericAddress = genericStreamAddress->text();
    videoBackend->setCurrentText("mjpeg_http");
    if (!localCameraRow->isHidden()) qFatal("Remote camera mode exposed duplicate local capture settings");
    if (serviceAddressRow->isHidden() || !genericStreamRow->isHidden())
        qFatal("Configured camera service must be the sole visible remote address");
    cameraServiceAddress->clear();
    if (serviceAddressRow->isHidden() || genericStreamRow->isHidden())
        qFatal("Generic MJPEG address must remain available without a camera service address");
    cameraServiceAddress->setText(originalServiceAddress);
    if (!genericStreamRow->isHidden()) qFatal("Generic MJPEG fallback did not hide after restoring camera service");
    videoBackend->setCurrentText("oak_depthai");
    if (localCameraRow->isHidden()) qFatal("Local camera mode did not show its capture settings");
    if (!serviceAddressRow->isHidden() || !genericStreamRow->isHidden())
        qFatal("Local OAK mode must hide remote camera addresses");
    videoBackend->setCurrentText(originalBackend);
    if (cameraServiceAddress->text() != originalServiceAddress || genericStreamAddress->text() != originalGenericAddress)
        qFatal("Changing camera backend visibility modified configured addresses");
    auto *helpStack = helpRoot->findChild<QStackedWidget*>("helpTopicStack");
    if (!helpStack) helpStack = helpRoot->findChild<QStackedWidget*>();
    if (!helpStack || helpStack->count() < helpNav->count()) qFatal("Missing help topic pages");
    auto *helpSearch = helpRoot->findChild<QLineEdit*>("helpSearchEdit");
    if (!helpSearch) qFatal("Missing help full-text search");

    auto *home = window.findChild<Home *>();
    auto *previewKind = window.findChild<QComboBox *>("cameraPreviewKind");
    auto *previewButton = window.findChild<QPushButton *>("videoPreviewButton");
    if (!previewKind || previewKind->count() != 2 || !previewButton || !previewButton->isCheckable() ||
        previewButton->isChecked() || window.findChild<QWidget *>("video_topic_name") ||
        window.findChild<QWidget *>("savePathButton"))
        qFatal("Home must offer one explicit RGB/depth preview selector and preview toggle");
    if (previewKind->itemData(0, Qt::UserRole + 1) != QStringLiteral("rgb") ||
        previewKind->itemData(1, Qt::UserRole + 1) != QStringLiteral("depth"))
        qFatal("Home preview sources must distinguish RGB and depth");
    const auto checkHomeOverview = [&window, checkFullyVisible] {
        for (const char *name : {"homeLocalizationState", "homeTaskState", "homeTaskProgress"}) {
            auto *field = window.findChild<QLineEdit *>(name);
            checkFullyVisible(field);
            if (!field->isReadOnly() || field->focusPolicy() != Qt::NoFocus || field->text().isEmpty())
                qFatal("Home overview must remain populated and read-only");
            if (field->fontMetrics().horizontalAdvance(field->text()) > field->contentsRect().width() - 16)
                qFatal("Home overview text is clipped");
        }
        if (window.findChild<QWidget *>("comboBox_Mode") || window.findChild<QWidget *>("pushButton") ||
            window.findChild<QWidget *>("lineEdit_mapname") || window.findChild<QWidget *>("lineEdit_mode"))
            qFatal("Retired home chassis mode controls remain");
    };
    const auto checkCameraControls = [&window, previewKind, previewButton, cameraControlLayouts, checkHomeOverview](const QString &sizeName) {
        checkHomeOverview();
        auto *longitude = window.findChild<QLineEdit *>("lineEdit");
        auto *latitude = window.findChild<QLineEdit *>("lineEdit_8");
        if (!longitude || !latitude || longitude->mapTo(&window, QPoint()).x() != latitude->mapTo(&window, QPoint()).x() ||
            longitude->width() != latitude->width() || longitude->height() != latitude->height())
            qFatal("Home longitude and latitude fields must share aligned edges and equal sizes");
        const QList<QWidget *> controls{window.findChild<QPushButton *>("recordButton"),
            window.findChild<QPushButton *>("captureButton"), previewButton, previewKind};
        QJsonArray sizes;
        int minWidth = 100000, maxWidth = 0, rowTop = -1;
        for (auto *control : controls) {
            if (!control || !control->isVisible() || control->height() != 64)
                qFatal("Camera controls must remain visible at the same 64 pixel height");
            const QPoint position = control->mapTo(&window, QPoint());
            if (rowTop >= 0 && rowTop != position.y()) qFatal("Camera controls no longer share one row");
            rowTop = position.y();
            for (auto *parent = control->parentWidget(); parent; parent = parent->parentWidget()) {
                if (!parent->rect().contains(QRect(control->mapTo(parent, QPoint()), control->size())))
                    qFatal("Camera control is clipped by its page or window");
                if (parent == &window) break;
            }
            minWidth = qMin(minWidth, control->width()); maxWidth = qMax(maxWidth, control->width());
            sizes.append(QJsonObject{{"name", control->objectName()}, {"width", control->width()}, {"height", control->height()}});
        }
        if (maxWidth - minWidth > 2) qFatal("Camera controls must have equal allocated widths");
        cameraControlLayouts->append(QJsonObject{{"size", sizeName}, {"controls", sizes}});
    };
    const auto selectPreviewPopupItem = [previewKind](int row) {
        auto *view = previewKind->view();
        if (!view || !view->isVisible()) qFatal("Preview selector did not open its menu");
        const auto index = view->model()->index(row, previewKind->modelColumn(), previewKind->rootModelIndex());
        if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled)) qFatal("Preview menu item is unavailable");
        view->scrollTo(index);
        const auto bounds = view->visualRect(index);
        if (bounds.isEmpty() || !view->viewport()->rect().contains(bounds.center()))
            qFatal("Preview menu item is outside the visible popup");
        const QPoint point = bounds.center();
        auto *viewport = view->viewport();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(viewport->mapToGlobal(point)),
            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(viewport->mapToGlobal(point)),
            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &press);
        QApplication::sendEvent(viewport, &release);
    };
    auto *chassisStatus = window.findChild<QPushButton *>("pushButton_10");
    if (!chassisStatus || chassisStatus->text() != QStringLiteral("状态延迟") ||
        !chassisStatus->toolTip().contains(QStringLiteral("底盘 HTTP")))
        qFatal("Initial status must identify the chassis HTTP monitor independently of IPC navigation");
    auto *originLongitude = window.findChild<QLineEdit *>("lineEdit");
    auto *originLatitude = window.findChild<QLineEdit *>("lineEdit_8");
    if (!home || !originLongitude || !originLatitude || !originLongitude->isReadOnly() || !originLatitude->isReadOnly())
        qFatal("Home origin must only display authoritative IPC readback");
    const QJsonObject savedOrigin{{"originRevision", "smoke-saved"}, {"latitude", 30.25}, {"longitude", 120.5}, {"altitude", 0.0}};
    home->setSavedOriginSnapshot({{"saved", savedOrigin}, {"consistent", true}});
    if (originLongitude->text() != "120.50000000" || originLatitude->text() != "30.25000000" ||
        !originLongitude->toolTip().contains(QStringLiteral("尚未确认导航运行生效")))
        qFatal("Saved origin readback swapped coordinates or claimed runtime activation");
    home->setActiveOriginSnapshot({{"originRevision", "smoke-active"}, {"latitude", 31.0}, {"longitude", 121.0}, {"altitude", 0.0}});
    if (originLongitude->text() != "121.00000000" || originLatitude->text() != "31.00000000" ||
        !originLongitude->toolTip().contains(QStringLiteral("已保存不同原点")))
        qFatal("Home did not distinguish active and saved origins");
    home->setNavigationStatusCurrent(false);
    if (originLongitude->text() != "121.00000000" || originLatitude->text() != "31.00000000" ||
        !originLongitude->toolTip().contains(QStringLiteral("状态延迟")))
        qFatal("Wireless delay erased cached origin or failed to label it as last known");
    home->setNavigationStatusCurrent(true);
    home->setActiveOriginSnapshot({});
    if (originLongitude->text() != "120.50000000") qFatal("Stopped navigation did not fall back to saved origin");
    home->setSavedOriginSnapshot({});
    if (!originLongitude->text().isEmpty() || !originLatitude->text().isEmpty())
        qFatal("Offline origin display retained stale coordinates");
    for (auto *label : window.findChildren<QLabel *>())
        if (label->text() == QStringLiteral("地图基准纬度") || label->text() == QStringLiteral("地图基准经度"))
            qFatal("Legacy local origin editor must be removed");

    steps->push_back([&window, click] { window.resize(1280,900); click("homeButton"); });
    steps->push_back([click] { click("pushButton_9"); });
    steps->push_back([tabs, maintenanceNav, click] {
        if (!tabs->currentWidget()->property("maintenanceRemotePage").toBool() || maintenanceNav->currentRow() != tabs->currentIndex())
            qFatal("Home origin action must navigate to IPC maintenance without writing coordinates");
        tabs->setCurrentIndex(0);
        click("homeButton");
    });
    steps->push_back([previewKind, checkCameraControls] {
        checkCameraControls(QStringLiteral("1280x900"));
        const int selected = previewKind->currentIndex();
        previewKind->setFocus(Qt::MouseFocusReason);
        QWheelEvent wheel(QPointF(previewKind->rect().center()),
            QPointF(previewKind->mapToGlobal(previewKind->rect().center())),
            QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(previewKind, &wheel);
        if (previewKind->currentIndex() != selected)
            qFatal("Scrolling over preview selector changed the selected camera source");
        previewKind->showPopup();
    });
    steps->push_back([&window, previewKind, output, screenshots, selectPreviewPopupItem] {
        if (!previewKind->view()->isVisible()) qFatal("Preview selector menu is hidden after opening");
        auto *popup = previewKind->view()->window();
        const QRect windowBounds(window.mapToGlobal(QPoint()), window.size());
        const QRect popupBounds(popup->mapToGlobal(QPoint()), popup->size());
        const QRect bounds = windowBounds.united(popupBounds);
        const auto windowImage = window.grab();
        QPixmap screenshot(bounds.size() * windowImage.devicePixelRatio());
        screenshot.setDevicePixelRatio(windowImage.devicePixelRatio());
        screenshot.fill(QColor("#f4f7f5"));
        {
            QPainter painter(&screenshot);
            painter.drawPixmap(windowBounds.topLeft() - bounds.topLeft(), windowImage);
            painter.drawPixmap(popupBounds.topLeft() - bounds.topLeft(), popup->grab());
        }
        if (!screenshot.save(QDir(output).filePath("home-preview-menu.png"))) qFatal("Preview menu screenshot failed");
        screenshots->append(QStringLiteral("home-preview-menu"));
        selectPreviewPopupItem(1);
    });
    steps->push_back([previewKind, previewButton, capture] {
        if (previewKind->currentData(Qt::UserRole + 1) != QStringLiteral("depth") || previewKind->view()->isVisible())
            qFatal("Selecting depth in the open menu did not apply or dismiss the menu");
        if (previewButton->isChecked()) qFatal("Selecting a preview source must not start a disabled preview");
        capture("home-depth");
        previewKind->showPopup();
    });
    steps->push_back([selectPreviewPopupItem] { selectPreviewPopupItem(0); });
    steps->push_back([previewKind] {
        previewKind->showPopup();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(previewKind->view(), &escape);
        if (previewKind->view()->isVisible() || previewKind->currentData(Qt::UserRole + 1) != QStringLiteral("rgb"))
            qFatal("Cancelling preview selection changed the source or left its menu open");
        previewKind->showPopup();
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(previewKind->view(), &down);
        QApplication::sendEvent(previewKind->view(), &enter);
        if (previewKind->view()->isVisible() || previewKind->currentData(Qt::UserRole + 1) != QStringLiteral("depth"))
            qFatal("Keyboard preview selection did not apply and close its menu");
    });
    steps->push_back([previewKind] { previewKind->showPopup(); });
    steps->push_back([selectPreviewPopupItem] { selectPreviewPopupItem(0); });
    steps->push_back([&window, previewKind, previewButton, capture] {
        if (previewKind->currentData(Qt::UserRole + 1) != QStringLiteral("rgb") || previewKind->view()->isVisible() ||
            previewButton->isChecked()) qFatal("Selecting RGB in the open menu did not restore a disabled RGB preview");
        capture("home");
        window.resize(1200, 780);
    });
    steps->push_back([&window, checkCameraControls, capture] {
        if (window.size() != QSize(1200, 780)) qFatal("Home cannot fit the requested small window");
        checkCameraControls(QStringLiteral("1200x780"));
        capture("home-small");
        window.resize(1280, 900);
    });
    steps->push_back([&window] {
        auto *button = window.findChild<QPushButton *>("leftButton");
        const auto local = button->rect().center();
        QHelpEvent hover(QEvent::ToolTip, local, button->mapToGlobal(local));
        QApplication::sendEvent(button, &hover);
    });
    steps->push_back([&window, output, screenshots] {
        QLabel *tooltip = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (widget->isVisible() && widget->windowType() == Qt::ToolTip)
                tooltip = qobject_cast<QLabel *>(widget);
        if (!tooltip || !tooltip->text().contains(QStringLiteral("按住点动")))
            qFatal("Manual button hover must show readable operating guidance");
        if (!tooltip->grab().save(QDir(output).filePath("home-hover-tooltip.png")))
            qFatal("Tooltip screenshot failed");
        screenshots->append("home-hover-tooltip");
        const QImage rendered = tooltip->grab().toImage();
        int lightPixels = 0, darkPixels = 0;
        for (int y = 0; y < rendered.height(); ++y)
            for (int x = 0; x < rendered.width(); ++x) {
                const auto color = rendered.pixelColor(x, y);
                lightPixels += color.alpha() > 240 && color.lightness() > 200;
                darkPixels += color.alpha() > 240 && color.lightness() < 120;
            }
        if (lightPixels < rendered.width() * rendered.height() / 2 || darkPixels < 20)
            qFatal("Hover guidance must render an opaque light background and visible dark text");
        if (window.findChild<QWidget *>("line_3"))
            qFatal("Retired trailing home divider is still present");
        QToolTip::hideText();
    });
    const auto samplePose = [] {
        ControlPoseSnapshot pose;
        pose.bootId = "offline-smoke-fusion";
        pose.validForControl = true;
        pose.stateAgeMs = 18;
        pose.position = QPointF(-12.345, -6.789);
        pose.yaw = -1.571;
        pose.reasons = {"uncertainty_unknown"};
        return pose;
    };
    const auto sampleTask = [] {
        TrackingSnapshot task;
        task.bootId = "offline-smoke-tracking";
        task.taskId = "offline-smoke-task";
        task.state = "Tracking";
        task.stepIndex = 2;
        task.progressMeters = 1.25;
        task.lengthMeters = 5.0;
        return task;
    };
    steps->push_back([&window, home, samplePose, sampleTask] {
        home->setControlPoseStatus(samplePose());
        HomeStatusPresenter::showControlPose(window.findChild<QLineEdit *>("lineEdit_Position"), samplePose());
        home->setTaskStatus(sampleTask(), true);
    });
    steps->push_back([&window, capture, checkHomeOverview] {
        auto *position = window.findChild<QLineEdit *>("lineEdit_Position");
        if (position->fontMetrics().horizontalAdvance(position->text()) > position->contentsRect().width() - 16)
            qFatal("Current X, Y, theta values are clipped");
        checkHomeOverview(); capture("home-overview-live"); window.resize(1200, 780);
    });
    steps->push_back([&window, home, sampleTask, capture, checkHomeOverview] {
        auto *position = window.findChild<QLineEdit *>("lineEdit_Position");
        if (position->fontMetrics().horizontalAdvance(position->text()) > position->contentsRect().width() - 16)
            qFatal("Current X, Y, theta values are clipped in the small window");
        checkHomeOverview(); capture("home-overview-live-small");
        home->setPoseMonitoringAvailability(false, QStringLiteral("离线界面验证：监视更新延迟"));
        home->setTaskStatus(sampleTask(), false);
    });
    steps->push_back([&window, home, samplePose, sampleTask, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-delayed");
        auto *state = window.findChild<QLineEdit *>("homeTaskState");
        if (!state->text().contains(QStringLiteral("显示延迟")) ||
            !window.findChild<QLineEdit *>("homeTaskProgress")->text().startsWith(QStringLiteral("上次")))
            qFatal("Delayed overview must label retained task data");
        home->setControlPoseStatus(samplePose());
        auto task = sampleTask();
        task.state = "Paused"; task.pauseReason = "goal_position_outside_tolerance";
        home->setTaskStatus(task, true);
    });
    steps->push_back([home, sampleTask, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-paused");
        auto task = sampleTask();
        task.state = "Paused"; task.pauseReason = "pose_sample_invalid"; task.autoResumePending = true;
        home->setTaskStatus(task, true);
    });
    steps->push_back([home, sampleTask, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-recovering");
        auto task = sampleTask();
        task.state = "Paused"; task.pauseReason = "pose_sample_invalid"; task.autoResumePending = true;
        home->setTaskStatus(task, false);
    });
    steps->push_back([home, sampleTask, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-recovering-delayed");
        auto task = sampleTask();
        task.state = "Turning"; task.remainingAngleRad = 0.7853981633974483;
        home->setTaskStatus(task, true);
    });
    steps->push_back([home, sampleTask, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-turning");
        auto task = sampleTask();
        task.state = "Waiting"; task.waitingEventId = "offline-smoke-checkpoint";
        home->setTaskStatus(task, true);
    });
    steps->push_back([&window, home, capture, checkHomeOverview] {
        checkHomeOverview(); capture("home-overview-waiting");
        home->setControlPoseStatus({});
        home->setTaskStatus({}, false);
        window.resize(1280, 900);
    });
    steps->push_back([click, checkCameraControls] { checkCameraControls(QStringLiteral("1280x900-restored")); click("mapButton"); });
    steps->push_back([capture, checkTaskPanel, &window] {
        checkTaskPanel(); capture("map-route"); window.resize(1200,780);
    });
    steps->push_back([capture, checkTaskPanel, &window] {
        checkTaskPanel(); capture("map-route-small"); window.resize(1280,900);
    });
    for (int i = 0; i < editorTabs->count(); ++i) {
        const QString prefix = QStringLiteral("map-editor-%1").arg(i);
        mapEditorPages->append(QJsonObject{{"index", i}, {"title", editorTabs->tabText(i)},
            {"top", prefix + "-top"}, {"bottom", prefix + "-bottom"}});
        steps->push_back([editorTabs, i, scrollTo, checkTaskPanel] {
            editorTabs->setCurrentIndex(i);
            scrollTo(editorTabs->currentWidget(), 0);
            checkTaskPanel();
        });
        steps->push_back([editorTabs, prefix, capture, scrollTo] {
            capture(prefix + "-top");
            scrollTo(editorTabs->currentWidget(), 1);
        });
        steps->push_back([prefix, capture] { capture(prefix + "-bottom"); });
    }
    steps->push_back([taskTabs, editorTabs] { editorTabs->setCurrentIndex(0); taskTabs->setCurrentIndex(1); });
    steps->push_back([taskTabs, capture, scrollTo, checkTaskPanel] {
        checkTaskPanel(); capture("map-row"); scrollTo(taskTabs->currentWidget(), 1);
    });
    steps->push_back([&window, taskTabs, capture, scrollTo, checkTaskPanel] {
        checkTaskPanel(); capture("map-row-bottom"); scrollTo(taskTabs->currentWidget(), 0); window.resize(1200,780);
    });
    steps->push_back([capture, click, checkTaskPanel] { checkTaskPanel(); capture("map-small"); click("maintenanceButton"); });
    steps->push_back([tabs, maintenanceNav, checkConfigVisibility] {
        if (tabs->tabText(0) != QStringLiteral("状态总览") || tabs->currentIndex() != 0 || maintenanceNav->currentRow() != 0)
            qFatal("Navigation status must be the default first maintenance tab");
        checkConfigVisibility(false);
        for (int i=0; i<tabs->count(); ++i)
            if (tabs->widget(i)->objectName() == "maintenanceLocalConnectionTabs") tabs->setCurrentIndex(i);
        if (tabs->currentIndex() == 0) qFatal("Missing maintenance communication tab");
        if (maintenanceNav->currentRow() != tabs->currentIndex())
            qFatal("Maintenance tab did not synchronize navigation selection");
        checkConfigVisibility(true);
    });
    steps->push_back([tabs] {
        auto *subtabs = qobject_cast<QTabWidget *>(tabs->currentWidget());
        if (!subtabs) qFatal("Missing local connection sub-tabs");
        auto *page = subtabs->currentWidget();
        auto *toggle = page->findChild<QCheckBox*>("maintenanceDescriptionsToggle");
        const auto hints = page->findChildren<QLabel*>("maintenanceFieldHint");
        if (!toggle || hints.isEmpty()) qFatal("Missing maintenance parameter descriptions");
        if (toggle->isChecked()) qFatal("Maintenance descriptions must default to collapsed");
        for (auto *hint : hints)
            if (!hint->isHidden()) qFatal("Maintenance parameter description was not initially hidden");
        toggle->click();
        if (!toggle->isChecked()) qFatal("Maintenance descriptions did not expand");
        for (auto *hint : hints)
            if (hint->isHidden()) qFatal("Maintenance parameter description did not become visible");
        toggle->click();
        if (toggle->isChecked()) qFatal("Maintenance descriptions did not collapse");
        for (auto *hint : hints)
            if (!hint->isHidden()) qFatal("Maintenance parameter description did not return to hidden");
    });
    steps->push_back([tabs, capture] {
        QSpinBox *field = nullptr;
        for (auto *spin : tabs->currentWidget()->findChildren<QSpinBox *>())
            if (spin->property("maintenanceConfigKey") == "network.statusPollIntervalMs") field = spin;
        if (!field) qFatal("Missing wheel-safety regression field");
        QScrollArea *scroll = nullptr;
        for (auto *parent = field->parentWidget(); parent; parent = parent->parentWidget())
            if ((scroll = qobject_cast<QScrollArea *>(parent))) break;
        if (!scroll) qFatal("Maintenance parameter editor has no scroll container");
        const int value = field->value();
        field->setFocus(Qt::MouseFocusReason);
        auto *bar = scroll->verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        const int position = bar->value();
        QWidget *target = field->findChild<QLineEdit *>();
        if (!target) target = field;
        QWheelEvent wheel(QPointF(target->rect().center()), QPointF(target->mapToGlobal(target->rect().center())),
            QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(target, &wheel);
        if (field->value() != value) qFatal("Scrolling a focused parameter changed its saved value");
        if (bar->maximum() > position && bar->value() <= position)
            qFatal("Parameter wheel guard swallowed page scrolling");
        capture("maintenance-scroll-safe");
        bar->setValue(0);
    });
    steps->push_back([&window, timer, tabs, click, capture, maintenanceLeaveChecks] {
        // These modal dialogs spin a nested event loop; keep later smoke steps
        // from firing until the entire cancel/discard interaction has returned.
        timer->stop();
        auto &config = ConfigManager::instance();
        if (config.network().websocketUrl != "ws://127.0.0.1:1")
            qFatal("Unsaved maintenance regression requires isolated endpoints");
        const auto fileBytes = [&config] {
            QFile file(config.configFilePath());
            if (!file.open(QIODevice::ReadOnly)) qFatal("Cannot read isolated configuration");
            return file.readAll();
        };
        const auto before = fileBytes();
        const auto diskRevision = config.diskRevision();
        const auto runtimeRevision = config.runtimeRevision();
        auto *stack = window.findChild<QStackedWidget *>("stackedWidget");
        auto *dirty = window.findChild<QLabel *>("maintenanceDirtyState");
        QSpinBox *field = nullptr;
        for (auto *spin : tabs->currentWidget()->findChildren<QSpinBox *>())
            if (spin->property("maintenanceConfigKey") == "network.statusPollIntervalMs") field = spin;
        if (!stack || !dirty || !field || stack->currentIndex() != 2)
            qFatal("Unsaved maintenance regression is not on its actual form");
        const int original = field->value();
        const int edited = original == 5000 ? 4950 : original + 50;
        field->setValue(edited);
        if (!dirty->text().contains(QStringLiteral("有未保存修改")))
            qFatal("Maintenance edit did not become dirty");
        const auto chooseDialog = [](const QString &text, QMessageBox::ButtonRole role) {
            QTimer::singleShot(30, [text, role] {
                auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
                if (!dialog || dialog->windowTitle() != QStringLiteral("是否保存本次修改"))
                    qFatal("Missing unsaved maintenance confirmation");
                for (auto *button : dialog->buttons()) {
                    if (button->text() == text && dialog->buttonRole(button) == role) {
                        button->click();
                        return;
                    }
                }
                qFatal("Unsaved maintenance confirmation has wrong action semantics");
            });
        };
        const auto checkRetained = [&] {
            if (stack->currentIndex() != 2 || field->value() != edited ||
                !dirty->text().contains(QStringLiteral("有未保存修改")) || !window.isVisible() ||
                !window.isEnabled() || fileBytes() != before || config.diskRevision() != diskRevision ||
                config.runtimeRevision() != runtimeRevision)
                qFatal("Cancelling navigation or close discarded maintenance edits or changed configuration");
        };
        chooseDialog(QStringLiteral("取消"), QMessageBox::RejectRole);
        click("homeButton");
        checkRetained();
        (*maintenanceLeaveChecks)["navigationCancelRetainsEdits"] = true;
        capture("maintenance-leave-cancel");
        QTimer::singleShot(30, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog) qFatal("Missing maintenance confirmation for Escape");
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(dialog, &escape);
        });
        click("homeButton");
        checkRetained();
        (*maintenanceLeaveChecks)["escapeRetainsEdits"] = true;
        chooseDialog(QStringLiteral("取消"), QMessageBox::RejectRole);
        window.close();
        checkRetained();
        (*maintenanceLeaveChecks)["closeCancelRetainsEdits"] = true;
        chooseDialog(QStringLiteral("放弃修改"), QMessageBox::DestructiveRole);
        click("homeButton");
        if (stack->currentIndex() != 0 || field->value() != original ||
            dirty->text().contains(QStringLiteral("有未保存修改")) || fileBytes() != before ||
            config.diskRevision() != diskRevision || config.runtimeRevision() != runtimeRevision)
            qFatal("Explicit discard did not restore the form without changing saved configuration");
        (*maintenanceLeaveChecks)["discardRestoresFormWithoutSaving"] = true;
        (*maintenanceLeaveChecks)["fileBytesUnchanged"] = true;
        click("maintenanceButton");
        capture("maintenance-leave-discard");
        timer->start(350);
    });
    steps->push_back([&window, tabs] {
        // run_gui_smoke.py supplies a disposable config and loopback endpoints.
        auto &config = ConfigManager::instance();
        if (config.network().websocketUrl != "ws://127.0.0.1:1") qFatal("Configuration GUI regression requires isolated endpoints");
        const int running = config.network().statusPollIntervalMs;
        QSpinBox *field = nullptr;
        for (auto *spin : tabs->currentWidget()->findChildren<QSpinBox *>())
            if (spin->property("maintenanceConfigKey") == "network.statusPollIntervalMs") field = spin;
        if (!field) qFatal("Missing local configuration editor");
        field->setValue(running == 5000 ? 4950 : running + 50);
        QPushButton *save = nullptr;
        for (auto *button : window.findChildren<QPushButton *>()) if (button->text() == QStringLiteral("保存本机")) save = button;
        if (!save || !save->isEnabled()) qFatal("Local save action unavailable after editing");
        QTimer::singleShot(30, [] { if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->accept(); });
        save->click();
        if (config.network().statusPollIntervalMs != running || config.diskRevision() == config.runtimeRevision())
            qFatal("Save-only changed runtime state or did not persist");
        QPushButton *apply = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("应用已保存配置到本机")) apply = button;
        auto *state = window.findChild<QLabel *>("maintenanceDirtyState");
        if (!apply || !apply->isEnabled() || apply->text() != QStringLiteral("应用已保存配置到本机") ||
            !state || state->text() != QStringLiteral("已保存待应用")) qFatal("Saved local configuration cannot be applied directly");
        const auto savedRevision = config.diskRevision();
        QTimer::singleShot(30, [] { if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->accept(); });
        apply->click();
        if (config.network().statusPollIntervalMs != field->value() || config.runtimeRevision() != savedRevision || apply->isEnabled())
            qFatal("Explicit apply did not use saved values or left a duplicate apply action");
    });
    steps->push_back([&window, tabs, maintenanceNav, capture, checkConfigVisibility] {
        checkConfigVisibility(true);
        capture("maintenance");
        tabs->setCurrentIndex(0);
        if (maintenanceNav->currentRow() != 0) qFatal("Maintenance navigation did not return to status");
        checkConfigVisibility(false);
        window.resize(1280,900);
    });
    steps->push_back([capture] { capture("maintenance-navigation"); });

    // Traverse every page and both ends of long forms without editing or invoking actions.
    for (int i=0; i<tabs->count(); ++i) {
        const QString prefix = QStringLiteral("maintenance-tab-%1").arg(i, 2, 10, QChar('0'));
        maintenancePages->append(QJsonObject{{"index", i}, {"title", tabs->tabText(i)},
            {"top", prefix + "-top"}, {"bottom", prefix + "-bottom"}});
        steps->push_back([tabs, maintenanceNav, i, scrollTo, checkConfigVisibility] {
            maintenanceNav->setCurrentRow(i);
            if (tabs->currentIndex() != i) qFatal("Maintenance navigation did not select matching page");
            checkConfigVisibility(i != 0 && !tabs->currentWidget()->property("maintenanceRemotePage").toBool());
            scrollTo(tabs->currentWidget(), 0);
        });
        steps->push_back([tabs, prefix, capture, scrollTo] {
            capture(prefix + "-top");
            scrollTo(tabs->currentWidget(), 1);
        });
        steps->push_back([prefix, capture] { capture(prefix + "-bottom"); });
        auto *subtabs = qobject_cast<QTabWidget *>(tabs->widget(i));
        if (subtabs) for (int j=0; j<subtabs->count(); ++j) {
            const QString subprefix = prefix + QStringLiteral("-sub-%1").arg(j, 2, 10, QChar('0'));
            maintenanceSubPages->append(QJsonObject{{"parentIndex", i}, {"index", j}, {"title", subtabs->tabText(j)},
                {"top", subprefix + "-top"}, {"bottom", subprefix + "-bottom"}});
            steps->push_back([subtabs, j, scrollTo] {
                subtabs->setCurrentIndex(j);
                scrollTo(subtabs->currentWidget(), 0);
            });
            steps->push_back([subtabs, subprefix, capture, scrollTo] {
                capture(subprefix + "-top"); scrollTo(subtabs->currentWidget(), 1);
            });
            steps->push_back([subprefix, capture] { capture(subprefix + "-bottom"); });
        }
    }
    int ipcParentIndex = -1;
    for (int i = 0; i < tabs->count(); ++i)
        if (tabs->widget(i)->property("maintenanceRemotePage").toBool()) ipcParentIndex = i;
    if (ipcParentIndex < 0) qFatal("Missing authoritative IPC maintenance entry");
    for (int i = 0; i < ipcTabs->count(); ++i) {
        const QString prefix = QStringLiteral("maintenance-ipc-%1").arg(i, 2, 10, QChar('0'));
        ipcMaintenancePages->append(QJsonObject{{"index", i}, {"title", ipcTabs->tabText(i)},
            {"top", prefix + "-top"}, {"bottom", prefix + "-bottom"}});
        steps->push_back([tabs, maintenanceNav, ipcTabs, ipcParentIndex, i, scrollTo, checkConfigVisibility] {
            maintenanceNav->setCurrentRow(ipcParentIndex);
            if (tabs->currentIndex() != ipcParentIndex) qFatal("IPC maintenance entry did not become current");
            checkConfigVisibility(false);
            ipcTabs->setCurrentIndex(i);
            scrollTo(ipcTabs->currentWidget(), 0);
        });
        steps->push_back([ipcTabs, prefix, capture, scrollTo] {
            capture(prefix + "-top");
            scrollTo(ipcTabs->currentWidget(), 1);
        });
        steps->push_back([prefix, capture] { capture(prefix + "-bottom"); });
    }
    steps->push_back([&window, ipcTabs, scrollTo] {
        ipcTabs->setCurrentIndex(1); window.resize(1200,780); scrollTo(ipcTabs->currentWidget(), 1);
    });
    steps->push_back([&window, ipcTabs, capture, checkFullyVisible] {
        auto *actions = window.findChild<QWidget *>("ipcParameterActions");
        checkFullyVisible(actions);
        for (const auto *name : {"ipcParametersSave", "ipcParametersSaveApply", "ipcParametersApplySaved"})
            checkFullyVisible(actions->findChild<QPushButton *>(name));
        checkFullyVisible(actions->findChild<QLabel *>("ipcParameterEditState"));
        capture("maintenance-ipc-parameters-small"); window.resize(1280,900);
    });
    steps->push_back([click, helpNav] { click("helpButton"); helpNav->setCurrentRow(0); });
    for (int i=0; i<helpNav->count(); ++i) {
        const QString prefix = QStringLiteral("help-topic-%1").arg(i, 2, 10, QChar('0'));
        helpPages->append(QJsonObject{{"index", i}, {"title", helpNav->item(i)->text()},
            {"top", prefix + "-top"}, {"bottom", prefix + "-bottom"}});
        steps->push_back([helpNav, helpStack, i, scrollTo] {
            helpNav->setCurrentRow(i);
            helpNav->scrollToItem(helpNav->item(i));
            if (helpStack->currentIndex() != i) qFatal("Help navigation did not select matching topic");
            scrollTo(helpStack->currentWidget(), 0);
        });
        steps->push_back([helpStack, prefix, i, capture, scrollTo] {
            capture(prefix + "-top");
            if (i == 0) capture("help");
            scrollTo(helpStack->currentWidget(), 1);
        });
        steps->push_back([prefix, capture] { capture(prefix + "-bottom"); });
    }
    {
        auto *search = helpSearch;
        steps->push_back([search] { search->setText("W/A/S/D"); });
        steps->push_back([helpNav, helpStack, capture] {
            for (int i=0; i<helpNav->count(); ++i)
                if (helpNav->item(i)->isHidden() != (i != 10))
                    qFatal("Help full-text search did not isolate keyboard topic");
            if (helpNav->currentRow() != 10 || helpStack->currentIndex() != 10)
                qFatal("Help full-text search did not select matching topic");
            auto *scroll = helpStack->currentWidget()->findChild<QScrollArea*>();
            if (!scroll || scroll->verticalScrollBar()->value() != 0)
                qFatal("Help full-text search did not scroll matching topic to top");
            capture("help-search-match");
        });
        steps->push_back([search] { search->setText("gui-smoke-no-matching-topic-5e3b7"); });
        steps->push_back([helpNav, helpStack, capture] {
            for (int i=0; i<helpNav->count(); ++i)
                if (!helpNav->item(i)->isHidden()) qFatal("Unmatched help search left visible topics");
            if (helpNav->currentRow() != -1 || helpStack->currentIndex() < helpNav->count())
                qFatal("Unmatched help search did not select empty results page");
            capture("help-search-empty");
        });
        steps->push_back([search] { search->clear(); });
        steps->push_back([helpNav, helpStack] {
            for (int i=0; i<helpNav->count(); ++i)
                if (helpNav->item(i)->isHidden()) qFatal("Clearing help search did not restore topics");
            if (helpNav->currentRow() != 0 || helpStack->currentIndex() != 0)
                qFatal("Clearing unmatched help search did not restore default topic");
        });
    }
    steps->push_back([click, aboutRoot, scrollTo] { click("aboutButton"); scrollTo(aboutRoot, 0); });
    steps->push_back([capture, aboutRoot, scrollTo] { capture("about"); capture("about-top"); scrollTo(aboutRoot, .5); });
    steps->push_back([capture, aboutRoot, scrollTo] { capture("about-middle"); scrollTo(aboutRoot, 1); });
    steps->push_back([capture] { capture("about-bottom"); });
    steps->push_back([&app, timer, output, screenshots, maintenancePages, maintenanceSubPages, ipcMaintenancePages,
                      mapEditorPages, cameraControlLayouts, maintenanceLeaveChecks, helpPages, tabs, helpNav, taskTabs, ipcTabs, editorTabs] {
        QFile manifest(QDir(output).filePath("coverage.json"));
        if (!manifest.open(QIODevice::WriteOnly)) qFatal("Cannot write GUI coverage manifest");
        manifest.write(QJsonDocument(QJsonObject{{"maintenanceTabCount", tabs->count()},
            {"helpTopicCount", helpNav->count()}, {"maintenance", *maintenancePages}, {"help", *helpPages},
            {"maintenanceSubPages", *maintenanceSubPages},
            {"ipcMaintenanceTabCount", ipcTabs->count()}, {"ipcMaintenance", *ipcMaintenancePages},
            {"mapEditorTabCount", editorTabs->count()}, {"mapEditors", *mapEditorPages},
            {"cameraControlLayouts", *cameraControlLayouts},
            {"maintenanceLeaveChecks", *maintenanceLeaveChecks},
            {"taskPages", QJsonArray{taskTabs->tabText(0), taskTabs->tabText(1)}},
            {"taskExecutionPanelCount", 1},
            {"taskWorkflowSimplified", true},
            {"screenshots", *screenshots}, {"aboutScrollPositions", QJsonArray{"top", "middle", "bottom"}}}).toJson());
        manifest.close();
        timer->stop();
        app.quit();
    });
    QObject::connect(timer, &QTimer::timeout, &window, [phase, steps] {
        if (*phase >= steps->size()) qFatal("GUI smoke sequence exceeded its steps");
        (*steps)[(*phase)++]();
    });
    timer->start(350);
}

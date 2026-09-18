#include "mainwindow.h" // 主窗口类声明
#include "ui_mainwindow.h" // Qt Designer 生成的 UI 头文件
#include "controlsessioncoordinator.h"
#include "externaleventcoordinator.h"
#include <QStandardPaths>
#include <QStatusBar>
#include "configmanager.h"
#include "home.h" // 首页业务逻辑封装
#include "map.h" // 地图页业务逻辑封装
#include "maintenance.h" // 维护页业务逻辑封装
#include "help.h" // 帮助页业务逻辑封装
#include "about.h" // 关于页业务逻辑封装

#include <QIcon> //  按钮图标所需
#include <QApplication>
#include <QInputDialog>
#include <QMessageBox>
#include <QPoint> //
#include <QRect> // 自定义拖动区域判断 右键菜单槽函数参数类型
#include <QSize> // 控件尺寸定义
#include <QTextStream> // 读取样式文件时使用
#include <QEvent>
#include <QWidget>
#include <QCloseEvent>
#include <QKeyEvent> // 捕获键盘事件
#include <QFile> // 文件操作
#include <QMouseEvent>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

MainWindow::MainWindow(QWidget *parent) // 主窗口构造函数
    : QMainWindow(parent) // 初始化基类
    , ui(new Ui::MainWindow) // 创建 UI 对象
{

    ui->setupUi(this); // 将 UI 绑定到当前窗口
    initUIComponents(); // 调用样式初始化函数
   /*导航栏 */
    //按钮指针
    homeButton = ui->homeButton; // 首页按钮指针
    mapButton = ui->mapButton; // 地图按钮指针
    maintenanceButton = ui->maintenanceButton; // 维护按钮指针
    helpButton = ui->helpButton; // 帮助按钮指针
    aboutButton = ui->aboutButton; // 关于按钮指针

    // 按钮图标
    homeButton->setIcon(QIcon(":/image/imagesnav/shouye.png")); //  首页按钮图标
    mapButton->setIcon(QIcon(":/image/imagesnav/ditu.png")); //  地图按钮图标
    maintenanceButton->setIcon(QIcon(":/image/imagesnav/weihu.png")); //  维护按钮图标
    helpButton->setIcon(QIcon(":/image/imagesnav/bangzhu.png")); //  帮助按钮图标
    aboutButton->setIcon(QIcon(":/image/imagesnav/guanyu.png")); //  关于按钮图标

    // 图标尺寸
    const QSize iconSize(24, 24); // 定义统一的图标尺寸
    homeButton->setIconSize(iconSize); // 应用图标尺寸到首页按钮
    mapButton->setIconSize(iconSize); // 应用图标尺寸到地图按钮
    maintenanceButton->setIconSize(iconSize); // 应用图标尺寸到维护按钮
    helpButton->setIconSize(iconSize); // 应用图标尺寸到帮助按钮
    aboutButton->setIconSize(iconSize); // 应用图标尺寸到关于按钮

    stackedWidget = ui->stackedWidget; // 缓存堆叠窗口指针

    //导航按钮互斥
    homeButton->setAutoExclusive(true); //  首页按钮互斥
    mapButton->setAutoExclusive(true); //  地图按钮互斥
    maintenanceButton->setAutoExclusive(true); //  维护按钮互斥
    helpButton->setAutoExclusive(true); //  帮助按钮互斥
    aboutButton->setAutoExclusive(true); //  关于按钮互斥

    homeButton->setFocusPolicy(Qt::NoFocus);
    mapButton->setFocusPolicy(Qt::NoFocus);
    maintenanceButton->setFocusPolicy(Qt::NoFocus);
    helpButton->setFocusPolicy(Qt::NoFocus);
    aboutButton->setFocusPolicy(Qt::NoFocus);

    homeButton->setChecked(true); // 默认选中首页按钮

    homePage = new Home(ui, this); // 初始化首页控制对象
    mapPage = new Map(ui, this); // 初始化地图页控制对象
    maintenancePage = new Maintenance(ui, this); // 初始化维护页控制对象
    helpPage = new Help(ui, this); // 初始化帮助页控制对象
    aboutPage = new About(ui, this); // 初始化关于页控制对象

    qApp->installEventFilter(this);

    poseClient = new PoseClient(this);
    trackingClient = new TrackingClient(this);
    connect(trackingClient, &TrackingClient::configurationChanged, maintenancePage, &Maintenance::setIpcConfiguration);
    connect(trackingClient, &TrackingClient::availabilityChanged, this, [this](bool ready, const QString&) {
        maintenancePage->setIpcConfiguration(ready ? trackingClient->configuration() : QJsonObject{});
    });
    controlCoordinator = new ControlSessionCoordinator(trackingClient, poseClient, this);
    mapPage->setControlCoordinator(controlCoordinator);
    externalEvents = new ExternalEventCoordinator(trackingClient, this);
    connect(externalEvents, &ExternalEventCoordinator::message, this, [this](const QString& text) { statusBar()->showMessage(text, 15000); });
    connect(mapPage, &Map::externalActionResolved, this, [this](const QString &eventId, bool success) {
        if (!externalEvents->resolveCurrent(eventId, success)) statusBar()->showMessage(tr("当前检查点动作无法确认，请检查会话、事件及拍摄状态"), 5000);
    });
    connect(homePage, &Home::originUpdateRequested, this, [this](double lat, double lon) {
        const auto config = trackingClient->configuration();
        const auto active = config["origin"].toObject()["active"].toObject();
        if (!trackingClient->fresh() || !trackingClient->hasSession() || active.isEmpty()) {
            QMessageBox::warning(this, tr("更新原点"), tr("请先连接任务服务并获取操作权")); return;
        }
        bool ok = false;
        const double altitude = QInputDialog::getDouble(this, tr("更新融合原点"), tr("WGS84 海拔（m）"), active["altitude"].toDouble(), -1000, 10000, 3, &ok);
        if (!ok) return;
        QJsonObject origin{{"schemaVersion", 1}, {"originRevision", TrackingJson::newId()}, {"latitude", lat},
            {"longitude", lon}, {"altitude", altitude}, {"datum", "WGS84"}, {"frameId", "map"}};
        if (!trackingClient->updateOrigin(origin, active["originRevision"].toString()))
            QMessageBox::warning(this, tr("更新原点"), tr("原点更新未提交，请确认停稳、任务状态和操作会话"));
    });
    connect(trackingClient, &TrackingClient::originChanged, this, [this](const QJsonObject& origin) {
        QMessageBox::information(this, tr("更新原点"), origin["restartRequired"].toBool() ?
            tr("新原点已保存为待生效；请在停机维护时重启融合服务并重新确认地图绑定") : tr("原点已生效"));
    });
    connect(poseClient, &PoseClient::poseChanged, this, [this](const ControlPoseSnapshot& pose) {
        ui->lineEdit_Position->setText(tr("ENU X=%1, Y=%2, yaw=%3 rad · %4 ms")
            .arg(pose.position.x(), 0, 'f', 3).arg(pose.position.y(), 0, 'f', 3)
            .arg(pose.yaw, 0, 'f', 3).arg(pose.positionAgeMs, 0, 'f', 0));
    });
    connect(poseClient, &PoseClient::availabilityChanged, this, [this](bool valid, const QString& reason) {
        if (!valid) ui->lineEdit_Position->setText(reason);
    });
    connect(&ConfigManager::instance(), &ConfigManager::configChanged, this, &MainWindow::configureIpcClients);
    configureIpcClients();

    //四个界面控制按钮
    zhidingDefaultIcon = ui->zhiding->icon();
    if (zhidingDefaultIcon.isNull()) {
        zhidingDefaultIcon = QIcon(":/image/imagesnav/weizhiding.png");
        ui->zhiding->setIcon(zhidingDefaultIcon);
    }
    zhidingPinnedIcon = QIcon(":/image/imagesnav/zhiding.png");

    fangdaDefaultIcon = ui->fangda->icon();
    if (fangdaDefaultIcon.isNull()) {
        fangdaDefaultIcon = QIcon(":/image/imagesnav/fangda.png");
        ui->fangda->setIcon(fangdaDefaultIcon);
    }
    fangdaRestoreIcon = QIcon(":/image/imagesnav/huifu.png");

    ui->zhiding->setToolTip(tr("置顶"));
    ui->suoxiao->setToolTip(tr("最小化"));
    ui->fangda->setToolTip(tr("最大化"));
    ui->guanbi->setToolTip(tr("关闭"));


}

MainWindow::~MainWindow() // 析构函数
{
    if (controlCoordinator) controlCoordinator->shutdown();
    if (trackingClient) trackingClient->stop();
    if (poseClient) poseClient->stop();
    delete mapPage; mapPage = nullptr;
    delete homePage; homePage = nullptr;
    delete maintenancePage; maintenancePage = nullptr;
    delete helpPage; helpPage = nullptr;
    delete aboutPage; aboutPage = nullptr;
    delete ui; // 页面服务先退出，再释放 UI
}

// 首页按钮槽函数
void MainWindow::on_homeButton_clicked()
{
    switchToPage(0, homeButton); // 切换到首页页面
}

// 地图按钮槽函数
void MainWindow::on_mapButton_clicked()
{
    if (switchToPage(1, mapButton) && mapPage) { // 切换到地图页面
        mapPage->handleModuleActivated();
    }
}

// 维护按钮槽函数
void MainWindow::on_maintenanceButton_clicked()
{
    switchToPage(2, maintenanceButton); // 切换到维护页面
}

 // 帮助按钮槽函数
void MainWindow::on_helpButton_clicked()
{
    switchToPage(3, helpButton); // 切换到帮助页面
}

// 关于按钮槽函数
void MainWindow::on_aboutButton_clicked()
{
    switchToPage(4, aboutButton); // 切换到关于页面
}

 // 关于页右键菜单槽
void MainWindow::on_aboutPage_customContextMenuRequested(const QPoint &pos)
{
    Q_UNUSED(pos); // 避免未使用参数警告
    // TODO: 可在此实现右键菜单逻辑
}


 //四个界面控制按钮
void MainWindow::on_zhiding_clicked() //置顶
{
    topMostEnabled = !topMostEnabled;
    if (!zhidingPinnedIcon.isNull() && topMostEnabled) {
        ui->zhiding->setIcon(zhidingPinnedIcon);
    } else if (!zhidingDefaultIcon.isNull()) {
        ui->zhiding->setIcon(zhidingDefaultIcon);
    }
    applyTopMost(topMostEnabled);
}

void MainWindow::on_suoxiao_clicked() //最小化
{
    setWindowState(windowState() | Qt::WindowMinimized);
}

void MainWindow::on_fangda_clicked()  //最大化
{
    if (isMaximized() || isFullScreen()) {
        ui->fangda->setToolTip(tr("最大化"));
        showNormal();
        if (!fangdaDefaultIcon.isNull()) {
            ui->fangda->setIcon(fangdaDefaultIcon);
        }
    } else {
        ui->fangda->setToolTip(tr("恢复大小"));
        showMaximized();
        if (!fangdaRestoreIcon.isNull()) {
            ui->fangda->setIcon(fangdaRestoreIcon);

        }
    }
}

void MainWindow::on_guanbi_clicked()
{
    close();
}

void MainWindow::applyTopMost(bool enabled) //单独封装置顶
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
#else
    const Qt::WindowStates previousState = windowState();
    setWindowFlag(Qt::WindowStaysOnTopHint, enabled);
    setWindowState(previousState);
#endif
}

bool MainWindow::switchToPage(int pageIndex, QPushButton *button)
{
    if (!stackedWidget) {
        return false;
    }

    const int currentIndex = stackedWidget->currentIndex();
    if (currentIndex == pageIndex) {
        if (button) {
            button->setChecked(true);
        }
        return true;
    }

    if (currentIndex == 2 && maintenancePage && !maintenancePage->confirmLeaveIfDirty(this)) {
        if (maintenanceButton) {
            maintenanceButton->setChecked(true);
        }
        return false;
    }

    stackedWidget->setCurrentIndex(pageIndex);
    if (button) {
        button->setChecked(true);
    }
    return true;
}

bool MainWindow::isInNavBarDragArea(const QPoint &globalPos) const //判断拖拽区域是否在导航栏
{
    if (!ui || !ui->navBar || !ui->navBar->isVisible()) {
        return false;
    }
    const QPoint navTopLeft = ui->navBar->mapToGlobal(QPoint());
    const QRect navRect(navTopLeft, ui->navBar->size());
    return navRect.contains(globalPos);
}

void MainWindow::changeEvent(QEvent *event) //监听窗口状态变化 同步改变四个按钮图标 目的是避免快捷键操作导致图标不变化
{
    QMainWindow::changeEvent(event);
    if (!ui) {
        return;
    }

    if (event->type() == QEvent::WindowStateChange) {
        const Qt::WindowStates state = windowState();
        if (state.testFlag(Qt::WindowMinimized)) {
            if (!fangdaDefaultIcon.isNull()) {
                ui->fangda->setIcon(fangdaDefaultIcon);
            }
            if (homePage) {
                homePage->stopMotionForSafety(QStringLiteral("window_minimized"));
            }
        } else if (state.testFlag(Qt::WindowMaximized) || state.testFlag(Qt::WindowFullScreen)) {
            if (!fangdaRestoreIcon.isNull()) {
                ui->fangda->setIcon(fangdaRestoreIcon);
            }
        } else {
            if (!fangdaDefaultIcon.isNull()) {
                ui->fangda->setIcon(fangdaDefaultIcon);
            }
        }
    }

    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && homePage) {
        homePage->stopMotionForSafety(QStringLiteral("window_deactivated"));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closeDrained) { QMainWindow::closeEvent(event); return; }
    if (m_closing) { event->ignore(); return; }
    if (maintenancePage && maintenancePage->hasUnsavedChanges()) {
        if (!maintenancePage->confirmLeaveIfDirty(this)) {
            event->ignore();
            return;
        }
    }

    if (mapPage && !mapPage->confirmClose()) {
        event->ignore(); return;
    }
    // Finish local cleanup; the IPC continues executing its accepted task.
    m_closing = true; setEnabled(false); event->ignore();
    if (controlCoordinator) controlCoordinator->shutdown();
    QTimer::singleShot(0, this, [this] { m_closeDrained = true; close(); });
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (!homePage || !stackedWidget || stackedWidget->currentIndex() != 0) {
        return QMainWindow::eventFilter(watched, event);
    }

    auto *watchedWidget = qobject_cast<QWidget *>(watched);
    if (!watchedWidget) {
        return QMainWindow::eventFilter(watched, event);
    }
    if (watchedWidget->window() != this) {
        return QMainWindow::eventFilter(watched, event);
    }

    if (event->type() != QEvent::ShortcutOverride &&
        event->type() != QEvent::KeyPress &&
        event->type() != QEvent::KeyRelease) {
        return QMainWindow::eventFilter(watched, event);
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    const int key = keyEvent->key();
    const bool isGimbalKey =
        key == Qt::Key_Up ||
        key == Qt::Key_Down ||
        key == Qt::Key_Left ||
        key == Qt::Key_Right ||
        key == Qt::Key_Control;
    if (!isGimbalKey) {
        return QMainWindow::eventFilter(watched, event);
    }

    if (event->type() == QEvent::ShortcutOverride) {
        keyEvent->accept();
        return true;
    }

    if (event->type() == QEvent::KeyPress) {
        homePage->handleKeyPress(keyEvent->key(), keyEvent->modifiers(), keyEvent->isAutoRepeat());
    } else if (event->type() == QEvent::KeyRelease) {
        homePage->handleKeyRelease(keyEvent->key(), keyEvent->modifiers(), keyEvent->isAutoRepeat());
    }

    keyEvent->accept();
    return true;
}

void MainWindow::mousePressEvent(QMouseEvent *event) //鼠标点击事件
{
    if (event->button() == Qt::LeftButton) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint globalPos = event->globalPosition().toPoint();
#else
        const QPoint globalPos = event->globalPos();
#endif
        if (isInNavBarDragArea(globalPos)) {
            draggingWindow = true;
            dragOffset = globalPos - frameGeometry().topLeft();
            event->accept();
            return;
        }
    }
    draggingWindow = false;
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent *event) //鼠标移动事件
{
    if (draggingWindow && (event->buttons() & Qt::LeftButton)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        move(event->globalPosition().toPoint() - dragOffset);
#else
        move(event->globalPos() - dragOffset);
#endif
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event) //鼠标释放事件
{
    if (event->button() == Qt::LeftButton && draggingWindow) {
        draggingWindow = false;
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event) //键盘按下事件
{
    if (homePage && stackedWidget && stackedWidget->currentIndex() == 0) {
        if (homePage->handleKeyPress(event->key(), event->modifiers(), event->isAutoRepeat())) {
            event->accept();
            return;
        }
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event) //键盘松开事件
{
    if (homePage && stackedWidget && stackedWidget->currentIndex() == 0) {
        if (homePage->handleKeyRelease(event->key(), event->modifiers(), event->isAutoRepeat())) {
            event->accept();
            return;
        }
    }

    QMainWindow::keyReleaseEvent(event);
}













void MainWindow::configureIpcClients()
{
    const auto& config = ConfigManager::instance();
    const auto& pose = config.poseSource(); const auto& tracking = config.tracking();
    if (controlCoordinator) controlCoordinator->shutdown();
    poseClient->configure(QUrl(pose.baseUrl), config.network().authToken, pose.requestTimeoutMs);
    trackingClient->configure(QUrl(tracking.baseUrl), config.network().authToken, tracking.requestTimeoutMs);
    if (pose.enabled) poseClient->start(); else poseClient->stop();
    if (tracking.enabled) trackingClient->start(); else trackingClient->stop();
    if (externalEvents) externalEvents->configure(QUrl(config.video().controlBaseUrl), config.network().authToken,
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/checkpoint-actions", config.video().cameraRequestTimeoutMs);
}

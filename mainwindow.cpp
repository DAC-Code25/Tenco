#include "mainwindow.h" // 主窗口类声明
#include "ui_mainwindow.h" // Qt Designer 生成的 UI 头文件
#include "home.h" // 首页业务逻辑封装
#include "map.h" // 地图页业务逻辑封装
#include "maintenance.h" // 维护页业务逻辑封装
#include "help.h" // 帮助页业务逻辑封装
#include "about.h" // 关于页业务逻辑封装

#include <QIcon> //  按钮图标所需
#include <QPoint> //
#include <QRect> // 自定义拖动区域判断 右键菜单槽函数参数类型
#include <QSize> // 控件尺寸定义
#include <QTextStream> // 读取样式文件时使用
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

    homeButton->setChecked(true); // 默认选中首页按钮

    homePage = new Home(ui, this); // 初始化首页控制对象
    mapPage = new Map(ui, this); // 初始化地图页控制对象
    maintenancePage = new Maintenance(ui, this); // 初始化维护页控制对象
    helpPage = new Help(ui, this); // 初始化帮助页控制对象
    aboutPage = new About(ui, this); // 初始化关于页控制对象

    if (homePage && mapPage) {
        connect(homePage, &Home::vehiclePoseUpdated, mapPage, &Map::updateVehiclePose);
        connect(mapPage, &Map::routeSegmentDispatched, homePage, &Home::followRouteSegment);
        connect(mapPage, &Map::routeQueueCompletedOnce, homePage, &Home::handleRouteQueueCompleted);
        connect(mapPage, &Map::routeExecutionCancelled, homePage, &Home::cancelRouteExecution);
        connect(homePage, &Home::routeSegmentCompleted, mapPage, &Map::handleRouteSegmentCompleted);
    }

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
    delete ui; // 释放 UI 资源
}

// 首页按钮槽函数
void MainWindow::on_homeButton_clicked()
{
    stackedWidget->setCurrentIndex(0); // 切换到首页页面
    homeButton->setChecked(true); // 保持按钮选中状态
}

// 地图按钮槽函数
void MainWindow::on_mapButton_clicked()
{
    stackedWidget->setCurrentIndex(1); // 切换到地图页面
    mapButton->setChecked(true); // 保持按钮选中状态
    if (mapPage) {
        mapPage->handleModuleActivated();
    }
}

// 维护按钮槽函数
void MainWindow::on_maintenanceButton_clicked()
{
    stackedWidget->setCurrentIndex(2); // 切换到维护页面
    maintenanceButton->setChecked(true); // 保持按钮选中状态
}

 // 帮助按钮槽函数
void MainWindow::on_helpButton_clicked()
{
    stackedWidget->setCurrentIndex(3); // 切换到帮助页面
    helpButton->setChecked(true); // 保持按钮选中状态
}

// 关于按钮槽函数
void MainWindow::on_aboutButton_clicked()
{
    stackedWidget->setCurrentIndex(4); // 切换到关于页面
    aboutButton->setChecked(true); // 保持按钮选中状态
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

void MainWindow::applyTopMost(bool enabled)
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

bool MainWindow::isInNavBarDragArea(const QPoint &globalPos) const
{
    if (!ui || !ui->navBar || !ui->navBar->isVisible()) {
        return false;
    }
    const QPoint navTopLeft = ui->navBar->mapToGlobal(QPoint());
    const QRect navRect(navTopLeft, ui->navBar->size());
    return navRect.contains(globalPos);
}

void MainWindow::changeEvent(QEvent *event)
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
}

void MainWindow::mousePressEvent(QMouseEvent *event)
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

void MainWindow::mouseMoveEvent(QMouseEvent *event)
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

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && draggingWindow) {
        draggingWindow = false;
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (homePage && homePage->handleKeyPress(event->key(), event->isAutoRepeat())) {
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (homePage && homePage->handleKeyRelease(event->key(), event->isAutoRepeat())) {
        event->accept();
        return;
    }

    QMainWindow::keyReleaseEvent(event);
}













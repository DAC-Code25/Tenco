#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QIcon>
#include <QPoint> //整数坐标

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

//存在这些类 但无需提前包含头文件 前向声明
class Home;
class Map;
class Maintenance;
class Help;
class About;
class QKeyEvent;
class QMouseEvent; //用于窗口拖拽鼠标事件
class QEvent;
class QPoint;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr); //防隐式转换
    void initUIComponents();
    ~MainWindow() override; //重写基类析构逻辑

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override; //重写键盘按下事件入口
    void keyReleaseEvent(QKeyEvent *event) override; //重写键盘释放事件入口
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override; //监听窗口状态变化 同步到四个按钮图标

private slots: //槽函数声明
    void on_homeButton_clicked();
    void on_mapButton_clicked();
    void on_maintenanceButton_clicked();
    void on_helpButton_clicked();
    void on_aboutButton_clicked();
    void on_zhiding_clicked();
    void on_suoxiao_clicked();
    void on_fangda_clicked();
    void on_guanbi_clicked();

    //响应右键 在目标页面弹出菜单并执行对应动作 传入局部点击位置
    void on_aboutPage_customContextMenuRequested(const QPoint &pos); 

private:
    void applyTopMost(bool enabled); //切换窗口置顶属性并同步窗口按钮状态

    Ui::MainWindow *ui; //Qt Designer 生成的界面对象入口
    QPushButton *homeButton;
    QPushButton *mapButton;
    QPushButton *maintenanceButton;
    QPushButton *helpButton;
    QPushButton *aboutButton;
    QStackedWidget *stackedWidget;

    Home *homePage;
    Map *mapPage;
    Maintenance *maintenancePage;
    Help *helpPage;
    About *aboutPage;

    bool topMostEnabled = false; //记录是否置顶
    bool draggingWindow = false; //当前是否处于拖拽窗口状态
    QPoint dragOffset; //鼠标按下点相对窗口左上角的偏移，用于平滑拖动

    bool isInNavBarDragArea(const QPoint &globalPos) const;
    QIcon zhidingDefaultIcon;
    QIcon zhidingPinnedIcon;
    QIcon fangdaDefaultIcon;
    QIcon fangdaRestoreIcon;
};

#endif // MAINWINDOW_H






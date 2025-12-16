#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QStackedWidget>
#include <QIcon>
#include <QPoint>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class Home;
class Map;
class Maintenance;
class Help;
class About;
class QKeyEvent;
class QMouseEvent;
class QEvent;
class QPoint;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    void initUIComponents();
    ~MainWindow() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void on_homeButton_clicked();
    void on_mapButton_clicked();
    void on_maintenanceButton_clicked();
    void on_helpButton_clicked();
    void on_aboutButton_clicked();
    void on_zhiding_clicked();
    void on_suoxiao_clicked();
    void on_fangda_clicked();
    void on_guanbi_clicked();

    void on_aboutPage_customContextMenuRequested(const QPoint &pos);

private:
    void applyTopMost(bool enabled);

    Ui::MainWindow *ui;
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

    bool topMostEnabled = false;
    bool draggingWindow = false;
    QPoint dragOffset;

    bool isInNavBarDragArea(const QPoint &globalPos) const;
    QIcon zhidingDefaultIcon;
    QIcon zhidingPinnedIcon;
    QIcon fangdaDefaultIcon;
    QIcon fangdaRestoreIcon;
};

#endif // MAINWINDOW_H






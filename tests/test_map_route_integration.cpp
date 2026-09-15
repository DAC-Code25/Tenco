#include "../map.h"

#include "ui_mainwindow.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>

#include <QtTest>

#include <cmath>

class MapRouteIntegrationTest : public QObject
{
    Q_OBJECT

private:
    template<typename T>
    T *requireChild(QWidget *root, const char *name)
    {
        T *child = root->findChild<T *>(QString::fromLatin1(name));
        if (!child) {
            QTest::qFail(name, __FILE__, __LINE__);
            return nullptr;
        }
        return child;
    }

    void pumpEvents()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }

    void setComboToPointId(QComboBox *combo, int pointId)
    {
        QVERIFY(combo != nullptr);
        const int index = combo->findData(pointId);
        QVERIFY2(index >= 0, "point id not found in combo");
        combo->setCurrentIndex(index);
        pumpEvents();
    }

    void addPoint(QWidget *root, double x, double y, double theta = 0.0)
    {
        auto *xSpin = requireChild<QDoubleSpinBox>(root, "mapPointXSpin");
        auto *ySpin = requireChild<QDoubleSpinBox>(root, "mapPointYSpin");
        auto *thetaSpin = requireChild<QDoubleSpinBox>(root, "mapPointThetaSpin");
        auto *button = requireChild<QPushButton>(root, "mapAddPointButton");

        xSpin->setValue(x);
        ySpin->setValue(y);
        thetaSpin->setValue(theta);
        button->click();
        pumpEvents();
    }

    void addPath(QWidget *root, int fromId, int toId, int pathTypeIndex = 0, double sagitta = 1.0)
    {
        auto *startCombo = requireChild<QComboBox>(root, "mapPathStartCombo");
        auto *endCombo = requireChild<QComboBox>(root, "mapPathEndCombo");
        auto *typeCombo = requireChild<QComboBox>(root, "mapPathTypeCombo");
        auto *sagittaSpin = requireChild<QDoubleSpinBox>(root, "mapArcSagittaSpin");
        auto *button = requireChild<QPushButton>(root, "mapAddPathButton");

        setComboToPointId(startCombo, fromId);
        setComboToPointId(endCombo, toId);
        typeCombo->setCurrentIndex(pathTypeIndex);
        sagittaSpin->setValue(sagitta);
        button->click();
        pumpEvents();
    }

    void addRoute(QWidget *root, int fromId, int toId)
    {
        auto *startCombo = requireChild<QComboBox>(root, "mapRouteStartCombo");
        auto *endCombo = requireChild<QComboBox>(root, "mapRouteEndCombo");
        auto *button = requireChild<QPushButton>(root, "mapRouteAddButton");

        setComboToPointId(startCombo, fromId);
        setComboToPointId(endCombo, toId);
        button->click();
        pumpEvents();
    }

    void startRoute(QWidget *root)
    {
        auto *button = requireChild<QPushButton>(root, "mapRouteStartButton");
        QVERIFY(button->isEnabled());
        button->click();
        pumpEvents();
    }

    void removePath(QWidget *root, int fromId, int toId)
    {
        auto *table = requireChild<QTableWidget>(root, "mapPathTable");
        auto *button = requireChild<QPushButton>(root, "mapRemovePathButton");

        int rowToRemove = -1;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *startItem = table->item(row, 1);
            const QTableWidgetItem *endItem = table->item(row, 2);
            if (!startItem || !endItem) {
                continue;
            }
            if (startItem->text().toInt() == fromId && endItem->text().toInt() == toId) {
                rowToRemove = row;
                break;
            }
        }

        QVERIFY2(rowToRemove >= 0, "path row not found");
        table->selectRow(rowToRemove);
        button->click();
        pumpEvents();
    }


    bool polylineContains(const QList<QPointF> &polyline, const QPointF &expected) const
    {
        for (const QPointF &point : polyline) {
            if (std::abs(point.x() - expected.x()) < 1e-6
                && std::abs(point.y() - expected.y()) < 1e-6) {
                return true;
            }
        }
        return false;
    }

private slots:
    void showsTaskTabsForRouteAndRowWork();
    void showsRowWorkModeTabsForPrimitiveAndMission();
    void showsScrollableLeftEditorPanelWithStackedGroups();
};

void MapRouteIntegrationTest::showsTaskTabsForRouteAndRowWork()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *tabWidget = requireChild<QTabWidget>(mapPage, "mapTaskTabWidget");
    QVERIFY(tabWidget != nullptr);
    QCOMPARE(tabWidget->count(), 2);
    QCOMPARE(tabWidget->tabText(0), QStringLiteral("常规任务"));
    QCOMPARE(tabWidget->tabText(1), QStringLiteral("直线作业"));
}

void MapRouteIntegrationTest::showsRowWorkModeTabsForPrimitiveAndMission()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *taskTabs = requireChild<QTabWidget>(mapPage, "mapTaskTabWidget");
    QVERIFY(taskTabs != nullptr);
    taskTabs->setCurrentIndex(1);
    pumpEvents();

    auto *rowWorkModeTabs = requireChild<QTabWidget>(mapPage, "mapRowWorkModeTabWidget");
    QVERIFY(rowWorkModeTabs != nullptr);
    QCOMPARE(rowWorkModeTabs->count(), 2);
    QCOMPARE(rowWorkModeTabs->tabText(0), QStringLiteral("单垄原语"));
    QCOMPARE(rowWorkModeTabs->tabText(1), QStringLiteral("多垄任务"));
}

void MapRouteIntegrationTest::showsScrollableLeftEditorPanelWithStackedGroups()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    auto *leftScrollArea = requireChild<QScrollArea>(mapPage, "mapLeftScrollArea");
    QVERIFY(leftScrollArea != nullptr);
    QVERIFY(leftScrollArea->widgetResizable());
    QVERIFY(requireChild<QGroupBox>(mapPage, "mapGridBox") != nullptr);
    QVERIFY(requireChild<QGroupBox>(mapPage, "mapPointBox") != nullptr);
    QVERIFY(requireChild<QGroupBox>(mapPage, "mapBatchBox") != nullptr);
    QVERIFY(requireChild<QGroupBox>(mapPage, "mapPathBox") != nullptr);
}



QTEST_MAIN(MapRouteIntegrationTest)
#include "test_map_route_integration.moc"

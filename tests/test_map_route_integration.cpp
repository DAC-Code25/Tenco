#include "../map.h"

#include "ui_mainwindow.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTimer>

#include <QtTest>

#include <cmath>

class FakeRouteExecutor : public QObject
{
    Q_OBJECT

public:
    struct DispatchRecord {
        int fromId = -1;
        int toId = -1;
        QList<QPointF> polyline;
        double startTheta = 0.0;
        double endTheta = 0.0;
    };

    Map *map = nullptr;
    bool autoComplete = false;
    bool completionResult = true;
    QList<DispatchRecord> dispatches;

public slots:
    void handleDispatched(int fromId, int toId, const QList<QPointF> &polyline, double startTheta, double endTheta)
    {
        dispatches.append(DispatchRecord{fromId, toId, polyline, startTheta, endTheta});
        if (!autoComplete || !map) {
            return;
        }

        QTimer::singleShot(0, map, [this]() {
            if (!map) {
                return;
            }
            QMetaObject::invokeMethod(map,
                                      "handleRouteSegmentCompleted",
                                      Qt::QueuedConnection,
                                      Q_ARG(bool, completionResult));
        });
    }
};

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

    void completeSegment(Map &map, bool success)
    {
        const bool invoked = QMetaObject::invokeMethod(&map,
                                                       "handleRouteSegmentCompleted",
                                                       Qt::DirectConnection,
                                                       Q_ARG(bool, success));
        QVERIFY(invoked);
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
    void executesQueuedRouteEndToEnd();
    void replansRemainingRouteAfterSegmentFailure();
};

void MapRouteIntegrationTest::executesQueuedRouteEndToEnd()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    addPoint(mapPage, 0.0, 0.0, 0.0);
    addPoint(mapPage, 5.0, 0.0, 0.0);
    addPoint(mapPage, 10.0, 0.0, 0.0);

    addPath(mapPage, 1, 2);
    addPath(mapPage, 2, 3);

    addRoute(mapPage, 1, 2);
    addRoute(mapPage, 2, 3);

    map.updateVehiclePose(0.0, 0.0, 0.0);
    pumpEvents();

    FakeRouteExecutor executor;
    executor.map = &map;
    executor.autoComplete = true;
    connect(&map, &Map::routeSegmentDispatched, &executor, &FakeRouteExecutor::handleDispatched);

    QSignalSpy completedSpy(&map, &Map::routeQueueCompletedOnce);

    startRoute(mapPage);

    QTRY_COMPARE_WITH_TIMEOUT(executor.dispatches.size(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 1000);

    QCOMPARE(executor.dispatches.at(0).fromId, 1);
    QCOMPARE(executor.dispatches.at(0).toId, 2);
    QCOMPARE(executor.dispatches.at(1).fromId, 2);
    QCOMPARE(executor.dispatches.at(1).toId, 3);

    auto *queueList = requireChild<QListWidget>(mapPage, "mapRouteQueueList");
    QCOMPARE(queueList->count(), 2);
    QVERIFY(queueList->item(0)->text().contains(QStringLiteral("点1 → 点2")));
    QVERIFY(queueList->item(1)->text().contains(QStringLiteral("点2 → 点3")));
}

void MapRouteIntegrationTest::replansRemainingRouteAfterSegmentFailure()
{
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);

    Map map(&ui);
    QWidget *mapPage = ui.mapPage;
    QVERIFY(mapPage != nullptr);

    addPoint(mapPage, 0.0, 0.0, 0.0);
    addPoint(mapPage, 5.0, 0.0, 0.0);
    addPoint(mapPage, 10.0, 0.0, 0.0);
    addPoint(mapPage, 5.0, 5.0, 0.0);

    addPath(mapPage, 1, 2);
    addPath(mapPage, 2, 3);
    addRoute(mapPage, 1, 3);

    map.updateVehiclePose(0.0, 0.0, 0.0);
    pumpEvents();

    FakeRouteExecutor executor;
    executor.map = &map;
    connect(&map, &Map::routeSegmentDispatched, &executor, &FakeRouteExecutor::handleDispatched);

    startRoute(mapPage);
    QTRY_COMPARE_WITH_TIMEOUT(executor.dispatches.size(), 1, 1000);
    QCOMPARE(executor.dispatches.constFirst().fromId, 1);
    QCOMPARE(executor.dispatches.constFirst().toId, 3);

    addPath(mapPage, 2, 4);
    addPath(mapPage, 4, 3);
    removePath(mapPage, 2, 3);

    map.updateVehiclePose(5.0, 0.0, 0.0);
    pumpEvents();

    completeSegment(map, false);

    QTRY_COMPARE_WITH_TIMEOUT(executor.dispatches.size(), 2, 1000);
    QCOMPARE(executor.dispatches.at(1).fromId, 2);
    QCOMPARE(executor.dispatches.at(1).toId, 3);
    QVERIFY(polylineContains(executor.dispatches.at(1).polyline, QPointF(5.0, 5.0)));

    auto *queueList = requireChild<QListWidget>(mapPage, "mapRouteQueueList");
    QCOMPARE(queueList->count(), 1);
    QVERIFY(queueList->item(0)->text().contains(QStringLiteral("点2 → 点3")));
}

QTEST_MAIN(MapRouteIntegrationTest)
#include "test_map_route_integration.moc"

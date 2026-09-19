#include <QtWidgets>
#include <QtNetwork>
#include <QtTest>
#include <optional>
#include "rowworktypes.h"
#include "rowmissiontypes.h"
#include "taskcompiler.h"
#include "mapgeometry.h"
#include "jsonhttpclient.h"
// Exercise the actual page and graphics items without a device or test-only production API.
#define private public
#include "map.h"
#include "trackingclient.h"
#include "poseclient.h"
#undef private
#include "controlsessioncoordinator.h"
#include "ui_mainwindow.h"

static MapFrameBinding testBinding() {
    MapFrameBinding b;
    b.confirmed = true;
    b.context = {"map", "origin", "transform", "cal", "profile", 1};
    return b;
}

class PoseMapChainTest : public QObject {
    Q_OBJECT
private slots:
    void presentationDoesNotInvalidatePlanButGeometryDoes() {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.setControlCoordinator(&coordinator);
        map.refreshPlanningRevision();
        tracking.m_preparedPlan = {{"taskId", "task"}, {"revision", 1}};
        map.handleRotationSpinChanged(30);
        map.refreshPlanningRevision();
        QCOMPARE(map.m_binding.context.mapRevision, 1);
        QVERIFY(!tracking.preparedPlan().isEmpty());
        QVERIFY(map.addPointInternal(-2, -3, 0));
        map.refreshPlanningRevision();
        QCOMPARE(map.m_binding.context.mapRevision, 2);
        QVERIFY(tracking.preparedPlan().isEmpty());
    }
    void enuCardinalsMatchActualVehicleAndPointArrows_data() {
        QTest::addColumn<double>("yaw");
        QTest::addColumn<QPointF>("screenDirection");
        QTest::newRow("east") << 0.0 << QPointF(1, 0);
        QTest::newRow("north") << M_PI_2 << QPointF(0, -1);
        QTest::newRow("west") << M_PI << QPointF(-1, 0);
        QTest::newRow("south") << -M_PI_2 << QPointF(0, 1);
    }
    void enuCardinalsMatchActualVehicleAndPointArrows() {
        QFETCH(double, yaw);
        QFETCH(QPointF, screenDirection);
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {-3, 2};
        p.yaw = yaw;
        p.originRevision = "origin";
        p.calibrationId = "cal";
        p.validForControl = true;
        pose.poseChanged(p);
        QCOMPARE(map.m_vehicleItem->scenePos(), QPointF(-150, -100));
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, screenDirection).length() < 1e-9);
        QVERIFY(map.addPointInternal(-2, -3, MapGeometry::normalizeAngle(yaw + M_PI_2), 1));
        const auto *point = map.pointById(1);
        QCOMPARE(point->markerGroup->scenePos(), QPointF(-150, -100));
        const auto pointNose = point->arrow->mapToScene(QPointF(1, 0)) - point->arrow->mapToScene(QPointF(0, 0));
        QVERIFY(QLineF(pointNose, screenDirection).length() < 1e-9);
    }
    void nonzeroBindingCompilesMetersAndDisplaysSamePhysicalPoint() {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        auto b = testBinding();
        b.enuTranslation = {10, -4};
        b.enuYawOffsetRad = M_PI_2;
        map.m_binding = b;
        map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {8, -7}; p.yaw = M_PI_2;
        p.originRevision = "origin"; p.calibrationId = "cal"; p.validForControl = true;
        pose.poseChanged(p);
        QVERIFY(QLineF(map.m_vehicleItem->scenePos(), {-150, -100}).length() < 1e-9);
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, QPointF(1, 0)).length() < 1e-9);
        TaskCompileOptions options; options.safetyProfileId = "open";
        const auto task = TaskCompiler::route({{"section", {{-2, -3}, {-2, -1}}, {}}}, b, options);
        QVERIFY2(task.ok(), qPrintable(task.error));
        const auto xy = task.plan["steps"].toArray()[0].toObject()["points"].toArray();
        QCOMPARE(xy, (QJsonArray{QJsonArray{8, -7}, QJsonArray{8, -5}}));
        map.handleRotationSpinChanged(90);
        const auto again = TaskCompiler::route({{"section", {{-2, -3}, {-2, -1}}, {}}}, b, options);
        QCOMPARE(again.plan["steps"], task.plan["steps"]);
    }
    void stalePoseKeepsLastPositionVisiblyUnavailable() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {-3, 2}; p.originRevision = "origin"; p.calibrationId = "cal"; p.validForControl = true;
        pose.poseChanged(p);
        pose.availabilityChanged(false, "stale");
        QVERIFY(!map.m_hasVehiclePose);
        QVERIFY(!map.m_vehicleItem->isVisible() || map.m_vehicleItem->opacity() < .6);
        p.validForControl = false; p.position = {99, 99};
        pose.poseChanged(p);
        QVERIFY(!map.m_hasVehiclePose);
        QCOMPARE(map.m_vehicleItem->scenePos(), QPointF(-150, -100));
    }
    void smallPoseUpdatesReachGraphicsWithoutWaitingForBindingRefresh() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.updateVehiclePose(0, 0, 0);
        map.updateVehiclePose(.005, -.003, .004);
        QVERIFY(QLineF(map.m_vehicleItem->scenePos(), QPointF(-.15, .25)).length() < 1e-9);
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, QPointF(std::sin(.004), std::cos(.004))).length() < 1e-9);
    }
    void missionUploadFailureIsVisibleInMissionTab() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.setControlCoordinator(&coordinator);
        const auto before = map.m_rowMissionStatusLabel->text();
        map.uploadMissionTask();
        QVERIFY(map.m_rowMissionStatusLabel->text() != before);
    }
    void rowWorkArrowFollowsDisplayedLine_data() {
        QTest::addColumn<QPointF>("mapDelta");
        QTest::addColumn<QPointF>("screenDirection");
        QTest::newRow("south") << QPointF(1, 0) << QPointF(0, 1);
        QTest::newRow("north") << QPointF(-1, 0) << QPointF(0, -1);
        QTest::newRow("east") << QPointF(0, 1) << QPointF(1, 0);
        QTest::newRow("west") << QPointF(0, -1) << QPointF(-1, 0);
        QTest::newRow("north-east") << QPointF(-3, 4) << QPointF(.8, -.6);
    }
    void rowWorkArrowFollowsDisplayedLine() {
        QFETCH(QPointF, mapDelta);
        QFETCH(QPointF, screenDirection);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.startPose = {-2, -3, 0};
        map.m_rowWorkPlan.endPose = {-2 + mapDelta.x(), -3 + mapDelta.y(), 0};
        map.refreshRowWorkGraphics();
        QVERIFY(map.m_rowWorkDirectionArrowItem);
        const auto *arrow = map.m_rowWorkDirectionArrowItem;
        const auto direction = arrow->mapToScene(QPointF(26, 0)) - arrow->mapToScene(QPointF(0, 0));
        QVERIFY(QLineF(direction, screenDirection * 26).length() < 1e-9);
        QCOMPARE(map.m_rowWorkStartMarker->scenePos(), QPointF(-150, -100));
    }
};
QTEST_MAIN(PoseMapChainTest)
#include "test_pose_map_chain.moc"

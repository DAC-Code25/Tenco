#include <QtTest>

#include "../rowworktypes.h"

class RowWorkTypesTest : public QObject
{
    Q_OBJECT

private slots:
    void geometryProjectionAndClamp();
    void jsonRoundTripKeepsPlan();
};

void RowWorkTypesTest::geometryProjectionAndClamp()
{
    RowWorkPlan plan;
    plan.hasStartPose = true;
    plan.startPose = RowWorkPose{0.0, 0.0, 0.0};
    plan.hasEndPose = true;
    plan.endPose = RowWorkPose{10.0, 0.0, 0.0};

    QVERIFY(plan.isValid());
    QCOMPARE(plan.lineLength(), 10.0);

    double lateralError = 0.0;
    QPointF projectedPoint;
    const double progress = RowWorkGeometry::projectPointToProgress(
        plan, QPointF(3.5, 1.2), &lateralError, &projectedPoint);
    QCOMPARE(progress, 3.5);
    QCOMPARE(lateralError, 1.2);
    QCOMPARE(projectedPoint, QPointF(3.5, 0.0));

    QCOMPARE(RowWorkGeometry::clampProgress(plan, -5.0), 0.0);
    QCOMPARE(RowWorkGeometry::clampProgress(plan, 12.0), 10.0);
    QCOMPARE(RowWorkGeometry::pointAtProgress(plan, 7.0), QPointF(7.0, 0.0));
}

void RowWorkTypesTest::jsonRoundTripKeepsPlan()
{
    RowWorkPlan source;
    source.planId = QStringLiteral("row-20260429-001");
    source.version = 4;
    source.frameId = QStringLiteral("map");
    source.hasStartPose = true;
    source.startPose = RowWorkPose{1.0, 2.0, 0.1};
    source.hasEndPose = true;
    source.endPose = RowWorkPose{11.0, 2.0, 3.2};
    source.checkpoints = {
        RowCheckpoint{QStringLiteral("P1"), 3.2, 1500, true, true, false},
        RowCheckpoint{QStringLiteral("P2"), 8.6, 2000, true, false, true}
    };
    source.params.baseLinearSpeed = 0.28;
    source.params.maxLinearSpeed = 0.40;
    source.params.endpointArrivalDistance = 0.30;
    source.params.checkpointArrivalTolerance = 0.20;
    source.params.turnAngularSpeed = 0.55;
    source.params.loopEnabled = false;

    RowWorkPlan decoded;
    QVERIFY(RowWorkJson::planFromJson(RowWorkJson::planToJson(source), &decoded));

    QCOMPARE(decoded.planId, source.planId);
    QCOMPARE(decoded.version, source.version);
    QCOMPARE(decoded.frameId, source.frameId);
    QVERIFY(decoded.hasStartPose);
    QVERIFY(decoded.hasEndPose);
    QCOMPARE(decoded.startPose.x, source.startPose.x);
    QCOMPARE(decoded.endPose.yaw, source.endPose.yaw);
    QCOMPARE(decoded.checkpoints.size(), 2);
    QCOMPARE(decoded.checkpoints.at(0).name, QStringLiteral("P1"));
    QCOMPARE(decoded.checkpoints.at(1).triggerOnBackward, true);
    QCOMPARE(decoded.params.baseLinearSpeed, source.params.baseLinearSpeed);
    QCOMPARE(decoded.params.turnAngularSpeed, source.params.turnAngularSpeed);
    QCOMPARE(decoded.params.loopEnabled, source.params.loopEnabled);
}


QTEST_MAIN(RowWorkTypesTest)
#include "test_rowworktypes.moc"

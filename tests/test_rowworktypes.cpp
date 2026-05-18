#include <QtTest>

#include "../rowworktypes.h"

class RowWorkTypesTest : public QObject
{
    Q_OBJECT

private slots:
    void geometryProjectionAndClamp();
    void jsonRoundTripKeepsPlan();
    void statusAndCaptureParsing();
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
    source.params.endpointSlowdownDistance = 1.8;
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

void RowWorkTypesTest::statusAndCaptureParsing()
{
    const QJsonObject statusJson{
        {QStringLiteral("success"), true},
        {QStringLiteral("message"), QStringLiteral("executing forward")},
        {QStringLiteral("mode"), QStringLiteral("row_work_auto")},
        {QStringLiteral("state"), QStringLiteral("ExecutingForward")},
        {QStringLiteral("controlOwner"), QStringLiteral("row_work_auto")},
        {QStringLiteral("planId"), QStringLiteral("row-1")},
        {QStringLiteral("planVersion"), 3},
        {QStringLiteral("poseFresh"), true},
        {QStringLiteral("poseAgeMs"), 85},
        {QStringLiteral("lateralError"), 0.06},
        {QStringLiteral("headingErrorDeg"), 2.4},
        {QStringLiteral("progress"), 8.35},
        {QStringLiteral("lineLength"), 20.1},
        {QStringLiteral("currentDirection"), QStringLiteral("forward")},
        {QStringLiteral("currentCheckpointIndex"), 1},
        {QStringLiteral("pauseRemainingMs"), 1200},
        {QStringLiteral("lastEvent"), QStringLiteral("checkpoint P1 completed")}
    };

    RowWorkStatus status;
    QVERIFY(RowWorkJson::statusFromJson(statusJson, &status));
    QVERIFY(status.success);
    QVERIFY(status.isActive());
    QVERIFY(!status.isFaulted());
    QCOMPARE(status.planId, QStringLiteral("row-1"));
    QCOMPARE(status.planVersion, 3);
    QCOMPARE(status.currentCheckpointIndex, 1);
    QCOMPARE(status.pauseRemainingMs, 1200);

    const QJsonObject captureJson{
        {QStringLiteral("message"), QStringLiteral("pose captured")},
        {QStringLiteral("sampleDurationMs"), 2500},
        {QStringLiteral("sampleCount"), 24},
        {QStringLiteral("pose"),
         QJsonObject{
             {QStringLiteral("x"), 1.23},
             {QStringLiteral("y"), 4.56},
             {QStringLiteral("theta"), 0.12}
         }}
    };

    RowWorkPose pose;
    int durationMs = 0;
    int sampleCount = 0;
    QString message;
    QVERIFY(RowWorkJson::capturePoseResponseFromJson(captureJson, &pose, &durationMs, &sampleCount, &message));
    QCOMPARE(pose.x, 1.23);
    QCOMPARE(pose.y, 4.56);
    QCOMPARE(pose.yaw, 0.12);
    QCOMPARE(durationMs, 2500);
    QCOMPARE(sampleCount, 24);
    QCOMPARE(message, QStringLiteral("pose captured"));
}

QTEST_MAIN(RowWorkTypesTest)
#include "test_rowworktypes.moc"

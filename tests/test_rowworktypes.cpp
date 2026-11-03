#include <QtTest>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>

#include "../rowworktypes.h"

class RowWorkTypesTest : public QObject
{
    Q_OBJECT

private slots:
    void geometryProjectionAndClamp();
    void jsonRoundTripKeepsPlan();
    void legacyMotionSettingsAreDiscarded();
    void checkpointBoundariesAndLegacyDefaults();
    void malformedCheckpoint_data();
    void malformedCheckpoint();
    void malformedPlanNeverSilentlyLosesStops();
    void nonfiniteLineIsNotUsable();
    void localEditingVersionUsesTheIntegerRange();
    void checkpointsCanBeRecordedBeforeTheEndpoint();
    void finishingADraftRejectsOutOfRangePointsTransactionally();
    void malformedDraftPositionIsRejected();
    void recordedGeometrySurvivesCaptureAndJson();
    void invalidRecordedGeometryIsRejectedTransactionally();
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
        RowCheckpoint{QStringLiteral("P2"), 8.6, 0, false, false, true, true, 12000}
    };
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
    for (int i = 0; i < source.checkpoints.size(); ++i)
        QCOMPARE(RowWorkJson::checkpointToJson(decoded.checkpoints[i]),
                 RowWorkJson::checkpointToJson(source.checkpoints[i]));
    QCOMPARE(decoded.params.loopEnabled, source.params.loopEnabled);
    QCOMPARE(RowWorkJson::planToJson(decoded)["params"].toObject(),
             QJsonObject({{"loopEnabled", false}}));
}

void RowWorkTypesTest::legacyMotionSettingsAreDiscarded()
{
    RowWorkPlan source;
    source.planId = "legacy-row";
    source.hasStartPose = source.hasEndPose = true;
    source.startPose = {1.23456789, -2.34567891, .123456789};
    source.endPose = {4.23456789, -2.34567891, .123456789};
    RowCheckpoint checkpoint;
    checkpoint.name = "capture";
    checkpoint.progress = 1.123456789;
    checkpoint.dwellMs = 1530;
    checkpoint.capturePhoto = true;
    checkpoint.actionTimeoutMs = 12000;
    source.checkpoints = {checkpoint};
    auto legacy = RowWorkJson::planToJson(source);
    legacy["params"] = QJsonObject{{"baseLinearSpeed", 1.2}, {"maxLinearSpeed", .01},
        {"endpointArrivalDistance", .9}, {"checkpointArrivalTolerance", 3.},
        {"turnAngularSpeed", "retired-invalid-value"}, {"loopEnabled", false}};
    for (int schema : {1, 2}) {
        legacy["schemaVersion"] = schema;
        RowWorkPlan decoded;
        QVERIFY(RowWorkJson::planFromJson(legacy, &decoded));
        QCOMPARE(decoded.startPose.x, source.startPose.x);
        QCOMPARE(decoded.endPose.y, source.endPose.y);
        QCOMPARE(decoded.checkpoints.size(), 1);
        QCOMPARE(decoded.checkpoints[0].progress, checkpoint.progress);
        QCOMPARE(decoded.checkpoints[0].dwellMs, checkpoint.dwellMs);
        QVERIFY(decoded.checkpoints[0].capturePhoto);
        QCOMPARE(decoded.checkpoints[0].actionTimeoutMs, checkpoint.actionTimeoutMs);
        QCOMPARE(RowWorkJson::planToJson(decoded)["params"].toObject(),
                 QJsonObject({{"loopEnabled", false}}));
    }
}

void RowWorkTypesTest::checkpointBoundariesAndLegacyDefaults()
{
    for (int dwell : {0, 1, 1500, 3600000}) {
        for (int timeout : {1000, 30000, 3600000}) {
            RowCheckpoint original{"point", 1.125, dwell, true, false, true, true, timeout};
            RowCheckpoint restored;
            QVERIFY(RowWorkJson::checkpointFromJson(RowWorkJson::checkpointToJson(original), &restored));
            QCOMPARE(RowWorkJson::checkpointToJson(restored), RowWorkJson::checkpointToJson(original));
        }
    }
    RowCheckpoint defaults;
    RowCheckpoint restored{"previous", 9, 9999, false, false, false, true, 1000};
    QVERIFY(RowWorkJson::checkpointFromJson({{"progress", 0.0}}, &restored));
    QCOMPARE(restored.progress, 0.0);
    QCOMPARE(restored.dwellMs, defaults.dwellMs);
    QCOMPARE(restored.actionTimeoutMs, defaults.actionTimeoutMs);
    QCOMPARE(restored.enabled, defaults.enabled);
    QCOMPARE(restored.triggerOnForward, defaults.triggerOnForward);
    QCOMPARE(restored.triggerOnBackward, defaults.triggerOnBackward);
    QCOMPARE(restored.capturePhoto, defaults.capturePhoto);
}

void RowWorkTypesTest::malformedCheckpoint_data()
{
    QTest::addColumn<QString>("field");
    QTest::addColumn<QJsonValue>("value");
    for (const auto &field : {QString("dwellMs"), QString("actionTimeoutMs")}) {
        const auto prefix = field.toLatin1();
        QTest::newRow((prefix + "-negative").constData()) << field << QJsonValue(-1);
        QTest::newRow((prefix + "-overflow").constData()) << field << QJsonValue(3600001);
        QTest::newRow((prefix + "-fraction").constData()) << field << QJsonValue(1500.5);
        QTest::newRow((prefix + "-text").constData()) << field << QJsonValue("2000");
        QTest::newRow((prefix + "-bool").constData()) << field << QJsonValue(true);
        QTest::newRow((prefix + "-null").constData()) << field << QJsonValue(QJsonValue::Null);
    }
    QTest::newRow("timeout-zero") << QString("actionTimeoutMs") << QJsonValue(0);
    QTest::newRow("timeout-too-short") << QString("actionTimeoutMs") << QJsonValue(999);
    QTest::newRow("negative-progress") << QString("progress") << QJsonValue(-.01);
    QTest::newRow("text-progress") << QString("progress") << QJsonValue("1");
    QTest::newRow("null-progress") << QString("progress") << QJsonValue(QJsonValue::Null);
    QTest::newRow("name-bool") << QString("name") << QJsonValue(false);
    for (const auto *field : {"enabled", "triggerOnForward", "triggerOnBackward", "capturePhoto"}) {
        QTest::newRow(field) << QString(field) << QJsonValue("false");
        QTest::newRow((QByteArray(field) + "-number").constData()) << QString(field) << QJsonValue(0);
    }
}

void RowWorkTypesTest::malformedCheckpoint()
{
    QFETCH(QString, field);
    QFETCH(QJsonValue, value);
    RowCheckpoint preserved{"existing", 2, 1234, true, false, true, true, 12000};
    const auto before = RowWorkJson::checkpointToJson(preserved);
    auto broken = before;
    broken[field] = value;
    QVERIFY(!RowWorkJson::checkpointFromJson(broken, &preserved));
    QCOMPARE(RowWorkJson::checkpointToJson(preserved), before);
}

void RowWorkTypesTest::malformedPlanNeverSilentlyLosesStops()
{
    RowWorkPlan original;
    original.planId = "preserved";
    original.hasStartPose = original.hasEndPose = true;
    original.endPose = {0, 4, 0};
    original.checkpoints = {{"first", 1, 1500}, {"second", 2, 2500}};
    const auto before = RowWorkJson::planToJson(original);
    QList<QJsonObject> invalid;
    for (const auto &value : {QJsonValue("2"), QJsonValue(1.5), QJsonValue(3), QJsonValue(4), QJsonValue(QJsonValue::Null)}) {
        auto broken = before;
        broken["schemaVersion"] = value;
        invalid.append(broken);
    }
    for (const auto *field : {"checkpoints", "params", "startPoint", "endPoint", "frameBinding"}) {
        auto broken = before;
        broken[field] = "wrong-type";
        invalid.append(broken);
    }
    for (const auto &value : {QJsonValue("broken-checkpoint"), QJsonValue(QJsonObject{{"name", "missing-progress"}}),
                              QJsonValue(QJsonObject{{"progress", 2}, {"dwellMs", "2500"}})}) {
        auto broken = before;
        auto checkpoints = broken["checkpoints"].toArray();
        checkpoints[1] = value;
        broken["checkpoints"] = checkpoints;
        invalid.append(broken);
    }
    for (const auto *field : {"startPoint", "endPoint"}) {
        auto broken = before;
        broken[field] = QJsonObject{{"x", 0}, {"y", 1}, {"yaw", "bad"}};
        invalid.append(broken);
    }
    auto badLoop = before;
    badLoop["params"] = QJsonObject{{"loopEnabled", "false"}};
    invalid.append(badLoop);
    for (const auto &broken : invalid) {
        RowWorkPlan restored = original;
        QVERIFY(!RowWorkJson::planFromJson(broken, &restored));
        QCOMPARE(RowWorkJson::planToJson(restored), before);
    }
    RowWorkPlan incomplete;
    QVERIFY(RowWorkJson::planFromJson({{"planId", "unfinished-draft"}}, &incomplete));
    QVERIFY(!incomplete.isValid());
}

void RowWorkTypesTest::nonfiniteLineIsNotUsable()
{
    RowWorkPlan plan;
    plan.hasStartPose = plan.hasEndPose = true;
    plan.endPose = {std::numeric_limits<double>::infinity(), 0, 0};
    QVERIFY(!plan.isValid());
    plan.startPose = {-std::numeric_limits<double>::max(), 0, 0};
    plan.endPose = {std::numeric_limits<double>::max(), 0, 0};
    QVERIFY(!plan.isValid());
}

void RowWorkTypesTest::localEditingVersionUsesTheIntegerRange()
{
    RowWorkPlan plan;
    for (int version : {1, 1000001, std::numeric_limits<int>::max()}) {
        QVERIFY(RowWorkJson::planFromJson({{"version", version}}, &plan));
        QCOMPARE(plan.version, version);
    }
    for (double version : {0., -1., 1.5, double(std::numeric_limits<int>::max()) + 1})
        QVERIFY(!RowWorkJson::planFromJson({{"version", version}}, &plan));
}

void RowWorkTypesTest::checkpointsCanBeRecordedBeforeTheEndpoint()
{
    RowWorkPlan plan;
    RowCheckpoint first{"near", 0, 1250, true, true, false, true, 12000};
    QVERIFY(!RowWorkGeometry::setCheckpointPosition(plan, {2, 1}, &first));
    plan.hasStartPose = true;
    plan.startPose = {1, 1, .8};
    QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, {3, 2}, &first));
    QVERIFY(first.pendingMapPosition);
    QCOMPARE(*first.pendingMapPosition, QPointF(3, 2));
    QCOMPARE(RowWorkGeometry::checkpointPosition(plan, first), QPointF(3, 2));
    RowCheckpoint second{"far", 0, 2500, false, false, true, false, 30000};
    QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, {5, 3}, &second));
    plan.checkpoints = {first, second};
    QVERIFY(!plan.isValid());
    const auto draft = RowWorkJson::planToJson(plan);
    RowWorkPlan restored;
    QVERIFY(RowWorkJson::planFromJson(draft, &restored));
    QCOMPARE(RowWorkJson::planToJson(restored), draft);
    QVERIFY(!restored.isValid());
    // B need not follow A's heading: use the final A/B line, not draft radial distance.
    restored.hasEndPose = true;
    restored.endPose = {7, 4, -.2};
    QVERIFY(RowWorkGeometry::finalizeCheckpoints(&restored));
    QVERIFY(restored.isValid());
    QVERIFY(!restored.checkpoints[0].pendingMapPosition);
    QVERIFY(!restored.checkpoints[1].pendingMapPosition);
    QVERIFY(std::abs(restored.checkpoints[0].progress - std::sqrt(5.)) < 1e-12);
    QVERIFY(std::abs(restored.checkpoints[1].progress - 2 * std::sqrt(5.)) < 1e-12);
    QCOMPARE(restored.checkpoints[0].dwellMs, first.dwellMs);
    QCOMPARE(restored.checkpoints[0].capturePhoto, first.capturePhoto);
    QCOMPARE(restored.checkpoints[1].enabled, second.enabled);
    QCOMPARE(restored.checkpoints[1].triggerOnBackward, second.triggerOnBackward);
    QCOMPARE(RowWorkGeometry::checkpointPosition(restored, restored.checkpoints[0]), QPointF(3, 2));
    QVERIFY(!RowWorkJson::checkpointToJson(restored.checkpoints[0]).contains("pendingMapPosition"));
    QCOMPARE(restored.checkpoints[0].recordedMapPosition, std::optional<QPointF>({3, 2}));
    QCOMPARE(restored.checkpoints[1].recordedMapPosition, std::optional<QPointF>({5, 3}));
    const auto finished = RowWorkJson::planToJson(restored);
    QCOMPARE(finished["schemaVersion"].toInt(), 3);
    RowWorkPlan reloaded;
    QVERIFY(RowWorkJson::planFromJson(finished, &reloaded));
    QCOMPARE(RowWorkJson::planToJson(reloaded), finished);
}

void RowWorkTypesTest::finishingADraftRejectsOutOfRangePointsTransactionally()
{
    RowWorkPlan plan;
    plan.hasStartPose = true;
    plan.startPose = {0, 0, 0};
    for (const auto &point : {QPointF(1, .2), QPointF(4, .1)}) {
        RowCheckpoint checkpoint;
        QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, point, &checkpoint));
        plan.checkpoints.append(checkpoint);
    }
    for (const auto &end : {QPointF(2, 0), QPointF(-5, 0)}) {
        auto candidate = plan;
        candidate.hasEndPose = true;
        candidate.endPose = {end.x(), end.y(), 0};
        const auto before = RowWorkJson::planToJson(candidate);
        QVERIFY(!RowWorkGeometry::finalizeCheckpoints(&candidate));
        QCOMPARE(RowWorkJson::planToJson(candidate), before);
    }
    plan.hasEndPose = true;
    plan.endPose = {5, 0, 0};
    QVERIFY(RowWorkGeometry::finalizeCheckpoints(&plan));
    QCOMPARE(plan.checkpoints[0].progress, 1.0);
    QCOMPARE(plan.checkpoints[1].progress, 4.0);
    QCOMPARE(RowWorkGeometry::checkpointPosition(plan, plan.checkpoints[0]), QPointF(1, .2));
    // An existing saved distance must also be covered by a replacement endpoint.
    plan.endPose.x = 3;
    const auto before = RowWorkJson::planToJson(plan);
    QVERIFY(!RowWorkGeometry::finalizeCheckpoints(&plan));
    QCOMPARE(RowWorkJson::planToJson(plan), before);
}

void RowWorkTypesTest::malformedDraftPositionIsRejected()
{
    RowCheckpoint checkpoint{"preserved", 1, 2000};
    const auto before = RowWorkJson::checkpointToJson(checkpoint);
    for (const auto &value : {QJsonValue(QJsonValue::Null), QJsonValue(true),
                             QJsonValue(QJsonObject{}),
                             QJsonValue(QJsonObject{{"x", "1"}, {"y", 2}}),
                             QJsonValue(QJsonObject{{"x", 1}, {"y", "2"}})}) {
        auto invalid = before;
        invalid["pendingMapPosition"] = value;
        QVERIFY(!RowWorkJson::checkpointFromJson(invalid, &checkpoint));
        QCOMPARE(RowWorkJson::checkpointToJson(checkpoint), before);
    }
}

void RowWorkTypesTest::recordedGeometrySurvivesCaptureAndJson()
{
    RowWorkPlan plan;
    plan.hasStartPose = plan.hasEndPose = true;
    plan.endPose = {8, 0, 0};
    const QList<QPointF> samples{{2, .2}, {4, -.1}, {6, .1}};
    for (const auto &sample : samples) {
        RowCheckpoint checkpoint;
        checkpoint.enabled = false; // Geometry still defines the route without its business action.
        QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, sample, &checkpoint));
        QVERIFY(!checkpoint.pendingMapPosition);
        QCOMPARE(checkpoint.recordedMapPosition, std::optional<QPointF>(sample));
        plan.checkpoints.append(checkpoint);
    }
    RowCheckpoint legacy{"progress-only", 3, 1500};
    plan.checkpoints.append(legacy);
    QCOMPARE(RowWorkGeometry::pointAtProgress(plan, 3), QPointF(3, .05));
    QCOMPARE(RowWorkGeometry::checkpointPosition(plan, legacy), QPointF(3, .05));
    const QList<QPointF> expected{{0, 0}, {2, .2}, {4, -.1}, {6, .1}, {8, 0}};
    QCOMPARE(RowWorkGeometry::pathBetweenProgress(plan, 0, 8), expected);
    auto reversed = expected;
    std::reverse(reversed.begin(), reversed.end());
    QCOMPARE(RowWorkGeometry::pathBetweenProgress(plan, 8, 0), reversed);
    QCOMPARE(RowWorkGeometry::pathBetweenProgress(plan, 1, 5),
             QList<QPointF>({{1, .1}, {2, .2}, {4, -.1}, {5, 0}}));
    const auto json = RowWorkJson::planToJson(plan);
    QCOMPARE(json["schemaVersion"].toInt(), 3);
    RowWorkPlan restored;
    QVERIFY(RowWorkJson::planFromJson(json, &restored));
    QCOMPARE(RowWorkJson::planToJson(restored), json);
    QCOMPARE(RowWorkGeometry::pathBetweenProgress(restored, 0, 8), expected);
    QVERIFY(restored.checkpoints.last().recordedMapPosition == std::nullopt);
    auto downgraded = json;
    downgraded["schemaVersion"] = 2;
    QVERIFY(!RowWorkJson::planFromJson(downgraded, &restored));

    RowWorkPlan oldLine;
    oldLine.hasStartPose = oldLine.hasEndPose = true;
    oldLine.endPose = {8, 0, 0};
    oldLine.checkpoints = {{"old", 4, 0}};
    QCOMPARE(RowWorkJson::planToJson(oldLine)["schemaVersion"].toInt(), 2);
    QVERIFY(RowWorkJson::planFromJson(RowWorkJson::planToJson(oldLine), &restored));
    QCOMPARE(RowWorkGeometry::pathBetweenProgress(restored, 0, 8), QList<QPointF>({{0, 0}, {8, 0}}));
    QVERIFY(!restored.checkpoints[0].recordedMapPosition);
}

void RowWorkTypesTest::invalidRecordedGeometryIsRejectedTransactionally()
{
    RowWorkPlan plan;
    plan.planId = "preserve";
    plan.hasStartPose = plan.hasEndPose = true;
    plan.endPose = {8, 0, 0};
    RowCheckpoint first{"first", 0, 1000};
    QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, {2, .2}, &first));
    plan.checkpoints.append(first);
    RowCheckpoint next{"next", 5, 2000};
    const auto nextBefore = RowWorkJson::checkpointToJson(next);
    for (const auto &point : {QPointF(-.1, 0), QPointF(8.1, 0), QPointF(2, -.2),
                              QPointF(0, .2), QPointF(std::numeric_limits<double>::quiet_NaN(), 0)}) {
        QVERIFY(!RowWorkGeometry::setCheckpointPosition(plan, point, &next));
        QCOMPARE(RowWorkJson::checkpointToJson(next), nextBefore);
    }
    // Re-recording an existing checkpoint replaces its old position atomically.
    QVERIFY(RowWorkGeometry::setCheckpointPosition(plan, {3, .3}, &plan.checkpoints[0]));
    QCOMPARE(plan.checkpoints[0].recordedMapPosition, std::optional<QPointF>({3, .3}));
    auto duplicate = plan.checkpoints[0];
    duplicate.enabled = false;
    plan.checkpoints.append(duplicate);
    QVERIFY(plan.isValid()); // Multiple actions may share exactly the same geometry sample.
    const auto before = RowWorkJson::planToJson(plan);
    QList<RowWorkPlan> invalid;
    auto conflict = plan;
    conflict.checkpoints[1].recordedMapPosition = QPointF(3, -.3);
    invalid.append(conflict);
    conflict = plan;
    conflict.checkpoints[1].progress = 4;
    invalid.append(conflict);
    conflict = plan;
    conflict.checkpoints[1].recordedMapPosition = QPointF(9, 0);
    invalid.append(conflict);
    conflict = plan;
    conflict.checkpoints[1].recordedMapPosition = QPointF(3, std::numeric_limits<double>::infinity());
    invalid.append(conflict);
    conflict = plan;
    conflict.checkpoints[1].pendingMapPosition = QPointF(3, .3);
    invalid.append(conflict);
    for (const auto &broken : invalid) {
        QVERIFY(!broken.isValid());
        QVERIFY(RowWorkGeometry::pathBetweenProgress(broken, 0, 8).isEmpty());
        auto restored = plan;
        QVERIFY(!RowWorkJson::planFromJson(RowWorkJson::planToJson(broken), &restored));
        QCOMPARE(RowWorkJson::planToJson(restored), before);
    }
    for (const auto &value : {QJsonValue(QJsonValue::Null), QJsonValue(true), QJsonValue(QJsonObject{}),
                              QJsonValue(QJsonObject{{"x", "1"}, {"y", 2}})}) {
        auto broken = RowWorkJson::checkpointToJson(first);
        broken["recordedMapPosition"] = value;
        auto restored = first;
        QVERIFY(!RowWorkJson::checkpointFromJson(broken, &restored));
        QCOMPARE(RowWorkJson::checkpointToJson(restored), RowWorkJson::checkpointToJson(first));
    }
}


QTEST_MAIN(RowWorkTypesTest)
#include "test_rowworktypes.moc"

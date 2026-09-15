#include "taskcompiler.h"
#include <QtTest>
#include <cmath>

class TaskCompilerTest : public QObject {
    Q_OBJECT
    MapFrameBinding binding() const {
        MapFrameBinding b;
        b.confirmed = true;
        b.context = {"map", "origin", "transform", "calibration", "profile", 1};
        return b;
    }
    TaskCompileOptions options() const {
        TaskCompileOptions o;
        o.safetyProfileId = "open";
        o.rotationZoneId = "open";
        return o;
    }
  private slots:
    void axesAndRoundtrip() {
        auto b = binding();
        MapFrameAdapter identity(b);
        QCOMPARE(identity.toEnu({0, 1}), QPointF(1, 0));
        QCOMPARE(identity.toEnu({-1, 0}), QPointF(0, 1));
        b.enuTranslation = {10, 20};
        b.enuYawOffsetRad = .7;
        MapFrameAdapter transform(b);
        const auto back = transform.fromEnu(transform.toEnu({3, -2}));
        QVERIFY(QLineF(back, {3, -2}).length() < 1e-10);
        QVERIFY(std::abs(transform.yawFromEnu(transform.yawToEnu(.6)) - .6) < 1e-10);
        QVERIFY(MapFrameBinding::fromJson(b.toJson()).isUsable());
    }
    void cornersAndBlend() {
        QList<TaskRouteSection> route{{"corner", {{0, 0}, {0, 2}, {-2, 2}}, std::nullopt}};
        auto r = TaskCompiler::route(route, binding(), options());
        QVERIFY2(r.ok(), qPrintable(r.error));
        auto steps = r.plan["steps"].toArray();
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps[1].toObject()["type"].toString(), QString("turn"));
        QVERIFY(std::abs(steps[1].toObject()["targetYaw"].toDouble() - std::acos(-1.) / 2) < 1e-10);
        route = {{"a", {{0, 0}, {0, 1}}, std::nullopt}, {"b", {{0, 1}, {0, 2}}, std::nullopt}};
        r = TaskCompiler::route(route, binding(), options());
        QVERIFY(r.ok());
        QCOMPARE(r.plan["steps"].toArray()[0].toObject()["endBehavior"].toString(), QString("blend"));
        auto unbound = binding();
        unbound.confirmed = false;
        QVERIFY(!TaskCompiler::route(route, unbound, options()).ok());
        route[1].points[0] = {9, 9};
        QVERIFY(!TaskCompiler::route(route, binding(), options()).ok());
    }
    void rowForwardAndReturn() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.startPose = {0, 0, 0};
        row.endPose = {0, 2, 0};
        RowCheckpoint forward;
        forward.name = "forward";
        forward.progress = .5;
        forward.triggerOnBackward = false;
        RowCheckpoint back;
        back.name = "back";
        back.progress = 1.5;
        back.triggerOnForward = false;
        row.checkpoints = {forward, back};
        auto r = TaskCompiler::row(row, binding(), options(), true);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const auto steps = r.plan["steps"].toArray();
        int turns = 0;
        QStringList checkpoints;
        for (const auto &v : steps) {
            const auto s = v.toObject();
            if (s["type"].toString() == "turn")
                ++turns;
            if (s["type"].toString() == "wait")
                checkpoints << s["checkpointId"].toString();
            QVERIFY(!s.contains("reverse"));
        }
        QCOMPARE(turns, 2);
        QCOMPARE(checkpoints, QStringList({"forward", "back"}));
        auto single = TaskCompiler::row(row, binding(), options(), false);
        QVERIFY(single.ok());
        QCOMPARE(single.plan["steps"].toArray().size(), 3);
        auto repeat = options();
        repeat.repeatCount = 2;
        QVERIFY(!TaskCompiler::row(row, binding(), repeat, false).ok());
        QVERIFY(TaskCompiler::row(row, binding(), repeat, true).ok());
    }
    void missionExpansion() {
        RowMissionPlan m;
        m.frameBinding = binding();
        m.name = "mission";
        RowMissionStep row;
        row.stepId = "row";
        row.primitivePlan.frameBinding = binding();
        row.primitivePlan.hasStartPose = row.primitivePlan.hasEndPose = true;
        row.primitivePlan.startPose = {0, 0, 0};
        row.primitivePlan.endPose = {0, 1, 0};
        RowMissionStep transfer = row;
        transfer.stepId = "transfer";
        transfer.type = RowMissionStepType::Transfer;
        transfer.primitivePlan.startPose = {0, 1, 0};
        transfer.primitivePlan.endPose = {-1, 1, 0};
        RowMissionStep wait;
        wait.stepId = "wait";
        wait.type = RowMissionStepType::Wait;
        wait.dwellMs = 200;
        m.steps = {row, transfer, wait};
        auto r = TaskCompiler::mission(m, binding(), options());
        QVERIFY2(r.ok(), qPrintable(r.error));
        const auto steps = r.plan["steps"].toArray();
        QCOMPARE(steps.size(), 4);
        QCOMPARE(steps[1].toObject()["type"].toString(), QString("turn"));
        QCOMPARE(steps[3].toObject()["completion"].toObject()["durationMs"].toInt(), 200);
    }
    void sequencePrecision() {
        quint64 seq = 0;
        QVERIFY(TrackingJson::sequence(QString("18446744073709551615"), &seq));
        QCOMPARE(seq, UINT64_MAX);
        QVERIFY(!TrackingJson::sequence(42, &seq));
        QVERIFY(!TrackingJson::sequence(QString("-1"), &seq));
        QVERIFY(!TrackingJson::sequence(QString("18446744073709551616"), &seq));
    }
    void persistedBindingAndExplicitTolerance() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.startPose = {0, 0, 0};
        row.endPose = {0, 2, 0};
        row.params.endpointArrivalDistance = .15;
        RowCheckpoint photo;
        photo.name = "camera";
        photo.progress = 1;
        photo.capturePhoto = true;
        photo.actionTimeoutMs = 12000;
        row.checkpoints = {photo};
        RowWorkPlan loaded;
        QVERIFY(RowWorkJson::planFromJson(RowWorkJson::planToJson(row), &loaded));
        auto result = TaskCompiler::row(loaded, binding(), options(), false);
        QVERIFY2(result.ok(), qPrintable(result.error));
        const auto steps = result.plan["steps"].toArray();
        QCOMPARE(steps.last().toObject()["goalToleranceMeters"].toDouble(), .15);
        QCOMPARE(steps[2].toObject()["completion"].toObject()["type"].toString(), "external_ack");
        QCOMPARE(steps[2].toObject()["completion"].toObject()["timeoutMs"].toInt(), 12000);
        loaded.frameBinding.context.originRevision = "old";
        QVERIFY(!TaskCompiler::row(loaded, binding(), options(), false).ok());
        auto legacy = RowWorkJson::planToJson(row);
        legacy.remove("schemaVersion");
        legacy.remove("frameBinding");
        QVERIFY(RowWorkJson::planFromJson(legacy, &loaded));
        QVERIFY(!TaskCompiler::row(loaded, binding(), options(), false).ok());
    }
};
QTEST_APPLESS_MAIN(TaskCompilerTest)
#include "test_taskcompiler.moc"

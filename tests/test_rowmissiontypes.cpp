#include <QtTest>

#include "../rowmissiontypes.h"

class RowMissionTypesTest : public QObject
{
    Q_OBJECT

private slots:
    void stepAndPlanValidation();
    void jsonRoundTripKeepsMission();
};

void RowMissionTypesTest::stepAndPlanValidation()
{
    RowWorkPlan primitivePlan;
    primitivePlan.planId = QStringLiteral("row-primitive-1");
    primitivePlan.hasStartPose = true;
    primitivePlan.startPose = RowWorkPose{0.0, 0.0, 0.0};
    primitivePlan.hasEndPose = true;
    primitivePlan.endPose = RowWorkPose{8.0, 0.0, 0.0};

    RowMissionStep rowLeg;
    rowLeg.stepId = QStringLiteral("step-1");
    rowLeg.name = QStringLiteral("垄1往返");
    rowLeg.type = RowMissionStepType::RowLeg;
    rowLeg.primitivePlan = primitivePlan;
    QVERIFY(rowLeg.isValid());

    RowMissionStep turn;
    turn.stepId = QStringLiteral("step-2");
    turn.name = QStringLiteral("出垄转向");
    turn.type = RowMissionStepType::Turn;
    turn.targetYawRad = 1.57079632679;
    QVERIFY(turn.isValid());

    RowMissionPlan mission;
    mission.missionId = QStringLiteral("mission-1");
    mission.steps = {rowLeg, turn};
    QVERIFY(mission.isValid());
    QCOMPARE(mission.enabledStepCount(), 2);
}

void RowMissionTypesTest::jsonRoundTripKeepsMission()
{
    RowWorkPlan primitivePlan;
    primitivePlan.planId = QStringLiteral("row-primitive-2");
    primitivePlan.version = 3;
    primitivePlan.frameId = QStringLiteral("map");
    primitivePlan.hasStartPose = true;
    primitivePlan.startPose = RowWorkPose{1.0, 2.0, 0.1};
    primitivePlan.hasEndPose = true;
    primitivePlan.endPose = RowWorkPose{11.0, 2.0, 3.2};
    primitivePlan.checkpoints = {
        RowCheckpoint{QStringLiteral("P1"), 3.2, 1200, true, true, false}
    };

    RowMissionPlan source;
    source.missionId = QStringLiteral("mission-20260518-001");
    source.version = 5;
    source.frameId = QStringLiteral("map");
    source.name = QStringLiteral("双垄穿梭");
    source.loopEnabled = true;

    RowMissionStep firstStep;
    firstStep.stepId = QStringLiteral("step-1");
    firstStep.name = QStringLiteral("垄1作业");
    firstStep.type = RowMissionStepType::RowLeg;
    firstStep.note = QStringLiteral("先在原垄往返");
    firstStep.primitivePlan = primitivePlan;

    RowMissionStep secondStep;
    secondStep.stepId = QStringLiteral("step-2");
    secondStep.name = QStringLiteral("转向");
    secondStep.type = RowMissionStepType::Turn;
    secondStep.targetYawRad = 1.57079632679;

    RowMissionStep thirdStep;
    thirdStep.stepId = QStringLiteral("step-3");
    thirdStep.name = QStringLiteral("短暂停留");
    thirdStep.type = RowMissionStepType::Wait;
    thirdStep.dwellMs = 1500;

    source.steps = {firstStep, secondStep, thirdStep};

    RowMissionPlan decoded;
    QVERIFY(RowMissionJson::planFromJson(RowMissionJson::planToJson(source), &decoded));

    QCOMPARE(decoded.missionId, source.missionId);
    QCOMPARE(decoded.version, source.version);
    QCOMPARE(decoded.frameId, source.frameId);
    QCOMPARE(decoded.name, source.name);
    QCOMPARE(decoded.loopEnabled, source.loopEnabled);
    QCOMPARE(decoded.steps.size(), 3);
    QCOMPARE(decoded.steps.at(0).type, RowMissionStepType::RowLeg);
    QCOMPARE(decoded.steps.at(0).primitivePlan.planId, primitivePlan.planId);
    QCOMPARE(decoded.steps.at(1).type, RowMissionStepType::Turn);
    QCOMPARE(decoded.steps.at(2).dwellMs, 1500);
}

QTEST_MAIN(RowMissionTypesTest)
#include "test_rowmissiontypes.moc"

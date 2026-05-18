#include <QtTest>

#include <QJsonObject>

#include "../mapdocument.h"

class MapDocumentTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripKeepsTopology();
    void rejectsInvalidGridSize();
    void rejectsNewerSchema();
};

void MapDocumentTest::roundTripKeepsTopology()
{
    MapDocument source;
    source.schemaVersion = 2;
    source.savedAtIsoUtc = QStringLiteral("2026-03-12T10:11:12Z");
    source.gridWidth = 40;
    source.gridHeight = 25;
    source.cellSizeMeters = 0.5;
    source.rotationDeg = 15.0;
    source.points = {
        {1, 0.0, 0.0, 0.0},
        {2, 3.0, 4.0, 1.57}
    };
    source.paths = {
        {10, 1, 2, MapDocumentPathType::Line, 0.0},
        {11, 2, 1, MapDocumentPathType::Arc, 0.8}
    };
    source.hasRowWorkPlan = true;
    source.rowWorkPlan.planId = QStringLiteral("row-001");
    source.rowWorkPlan.version = 2;
    source.rowWorkPlan.frameId = QStringLiteral("map");
    source.rowWorkPlan.hasStartPose = true;
    source.rowWorkPlan.startPose = RowWorkPose{0.0, 0.0, 0.0};
    source.rowWorkPlan.hasEndPose = true;
    source.rowWorkPlan.endPose = RowWorkPose{10.0, 0.0, 3.14159};
    source.rowWorkPlan.checkpoints = {
        RowCheckpoint{QStringLiteral("P1"), 3.0, 1500, true, true, false}
    };
    source.rowWorkPlan.params.baseLinearSpeed = 0.28;
    source.rowWorkPlan.params.loopEnabled = false;
    source.hasRowMissionPlan = true;
    source.rowMissionPlan.missionId = QStringLiteral("mission-001");
    source.rowMissionPlan.version = 3;
    source.rowMissionPlan.frameId = QStringLiteral("map");
    source.rowMissionPlan.name = QStringLiteral("双垄往返");
    source.rowMissionPlan.loopEnabled = true;
    RowMissionStep rowLegStep;
    rowLegStep.stepId = QStringLiteral("step-1");
    rowLegStep.name = QStringLiteral("垄1");
    rowLegStep.type = RowMissionStepType::RowLeg;
    rowLegStep.primitivePlan = source.rowWorkPlan;
    RowMissionStep turnStep;
    turnStep.stepId = QStringLiteral("step-2");
    turnStep.name = QStringLiteral("转场转向");
    turnStep.type = RowMissionStepType::Turn;
    turnStep.targetYawRad = 1.57079632679;
    source.rowMissionPlan.steps = {rowLegStep, turnStep};

    const QJsonObject json = MapDocumentCodec::toJson(source);
    MapDocument decoded;
    QString error;
    QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error, 2), qPrintable(error));

    QCOMPARE(decoded.schemaVersion, source.schemaVersion);
    QCOMPARE(decoded.savedAtIsoUtc, source.savedAtIsoUtc);
    QCOMPARE(decoded.gridWidth, source.gridWidth);
    QCOMPARE(decoded.gridHeight, source.gridHeight);
    QCOMPARE(decoded.cellSizeMeters, source.cellSizeMeters);
    QCOMPARE(decoded.rotationDeg, source.rotationDeg);
    QCOMPARE(decoded.points.size(), source.points.size());
    QCOMPARE(decoded.paths.size(), source.paths.size());
    QVERIFY(decoded.paths.at(1).type == MapDocumentPathType::Arc);
    QCOMPARE(decoded.paths.at(1).sagitta, source.paths.at(1).sagitta);
    QVERIFY(decoded.hasRowWorkPlan);
    QCOMPARE(decoded.rowWorkPlan.planId, source.rowWorkPlan.planId);
    QCOMPARE(decoded.rowWorkPlan.version, source.rowWorkPlan.version);
    QVERIFY(decoded.rowWorkPlan.hasStartPose);
    QVERIFY(decoded.rowWorkPlan.hasEndPose);
    QCOMPARE(decoded.rowWorkPlan.checkpoints.size(), 1);
    QCOMPARE(decoded.rowWorkPlan.checkpoints.first().name, QStringLiteral("P1"));
    QCOMPARE(decoded.rowWorkPlan.checkpoints.first().progress, 3.0);
    QCOMPARE(decoded.rowWorkPlan.params.baseLinearSpeed, 0.28);
    QVERIFY(!decoded.rowWorkPlan.params.loopEnabled);
    QVERIFY(decoded.hasRowMissionPlan);
    QCOMPARE(decoded.rowMissionPlan.missionId, QStringLiteral("mission-001"));
    QCOMPARE(decoded.rowMissionPlan.version, 3);
    QCOMPARE(decoded.rowMissionPlan.steps.size(), 2);
    QCOMPARE(decoded.rowMissionPlan.steps.at(0).type, RowMissionStepType::RowLeg);
    QCOMPARE(decoded.rowMissionPlan.steps.at(1).type, RowMissionStepType::Turn);
}

void MapDocumentTest::rejectsInvalidGridSize()
{
    QJsonObject json;
    json.insert(QStringLiteral("schemaVersion"), 1);
    json.insert(QStringLiteral("gridWidth"), 0);
    json.insert(QStringLiteral("gridHeight"), 30);

    MapDocument doc;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(json, &doc, &error, 2));
    QVERIFY(!error.trimmed().isEmpty());
}

void MapDocumentTest::rejectsNewerSchema()
{
    QJsonObject json;
    json.insert(QStringLiteral("schemaVersion"), 99);
    json.insert(QStringLiteral("gridWidth"), 10);
    json.insert(QStringLiteral("gridHeight"), 10);
    json.insert(QStringLiteral("cellSizeMeters"), 1.0);
    json.insert(QStringLiteral("rotationDeg"), 0.0);

    MapDocument doc;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(json, &doc, &error, 2));
    QVERIFY(error.contains(QStringLiteral("schemaVersion")));
}

QTEST_MAIN(MapDocumentTest)
#include "test_mapdocument.moc"

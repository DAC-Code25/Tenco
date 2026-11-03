#include <QtTest>

#include <QJsonObject>
#include <QJsonArray>

#include "../mapdocument.h"

class MapDocumentTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripKeepsTopology();
    void legacyMapsKeepOneWayRelations();
    void rejectsMalformedDirection();
    void olderReaderRejectsBidirectionalSchema();
    void retiredMissionDataRoundTripsWithoutValidationOrFieldLoss();
    void rejectsInvalidGridSize();
    void rejectsNewerSchema();
    void rejectsDamagedStraightTaskWithoutReplacingMap();
};

void MapDocumentTest::roundTripKeepsTopology()
{
    MapDocument source;
    source.schemaVersion = 3;
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
        {10, 1, 2, MapDocumentPathType::Line, 0.0, true},
        {11, 2, 1, MapDocumentPathType::Arc, 0.8, false}
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
    source.rowWorkPlan.params.loopEnabled = false;

    const QJsonObject json = MapDocumentCodec::toJson(source);
    MapDocument decoded;
    QString error;
    QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error, 3), qPrintable(error));

    QCOMPARE(decoded.schemaVersion, source.schemaVersion);
    QCOMPARE(decoded.savedAtIsoUtc, source.savedAtIsoUtc);
    QCOMPARE(decoded.gridWidth, source.gridWidth);
    QCOMPARE(decoded.gridHeight, source.gridHeight);
    QCOMPARE(decoded.cellSizeMeters, source.cellSizeMeters);
    QCOMPARE(decoded.rotationDeg, source.rotationDeg);
    QCOMPARE(decoded.points.size(), source.points.size());
    QCOMPARE(decoded.paths.size(), source.paths.size());
    QVERIFY(decoded.paths.at(0).bidirectional);
    QVERIFY(!decoded.paths.at(1).bidirectional);
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
    QCOMPARE(json["rowWork"].toObject()["params"].toObject(), QJsonObject({{"loopEnabled", false}}));
    QVERIFY(!decoded.rowWorkPlan.params.loopEnabled);
}

void MapDocumentTest::legacyMapsKeepOneWayRelations()
{
    for (const int version : {1, 2}) {
        MapDocument source;
        source.schemaVersion = version;
        source.gridWidth = 10;
        source.gridHeight = 10;
        source.points = {{1, 0.0, 0.0, 0.0}, {2, 1.0, 0.0, 0.0}};
        source.paths = {{5, 1, 2, MapDocumentPathType::Line, 0.0}};
        const QJsonObject json = MapDocumentCodec::toJson(source);
        QVERIFY(!json["paths"].toArray().first().toObject().contains("bidirectional"));
        MapDocument decoded;
        QString error;
        QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.schemaVersion, version);
        QCOMPARE(decoded.paths.size(), 1);
        QVERIFY(!decoded.paths.first().bidirectional);
    }
}

void MapDocumentTest::rejectsMalformedDirection()
{
    MapDocument source;
    source.gridWidth = 10;
    source.gridHeight = 10;
    source.paths = {{5, 1, 2, MapDocumentPathType::Line, 0.0, true}};
    for (const QJsonValue malformed : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
        QJsonObject json = MapDocumentCodec::toJson(source);
        QJsonArray paths = json["paths"].toArray();
        QJsonObject path = paths.first().toObject();
        path["bidirectional"] = malformed;
        paths[0] = path;
        json["paths"] = paths;
        MapDocument decoded;
        QString error;
        QVERIFY(!MapDocumentCodec::fromJson(json, &decoded, &error));
        QVERIFY(error.contains("bidirectional"));
    }
}

void MapDocumentTest::olderReaderRejectsBidirectionalSchema()
{
    MapDocument source;
    source.gridWidth = 10;
    source.gridHeight = 10;
    source.paths = {{5, 1, 2, MapDocumentPathType::Arc, 0.2, true}};
    MapDocument decoded;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(MapDocumentCodec::toJson(source), &decoded, &error, 2));
    QVERIFY(error.contains("schemaVersion"));
}

void MapDocumentTest::retiredMissionDataRoundTripsWithoutValidationOrFieldLoss()
{
    const QJsonObject unfinishedMission{
        {"missionId", "legacy-incomplete"},
        {"version", 3},
        {"futureField", QJsonObject{{"keep", "原始业务数据"}}},
        {"steps", QJsonArray{QJsonObject{{"type", "future_step"}, {"enabled", false}}}}
    };
    for (const QJsonObject &legacy : {unfinishedMission, QJsonObject{}}) {
        MapDocument source;
        source.gridWidth = 10;
        source.gridHeight = 10;
        QJsonObject json = MapDocumentCodec::toJson(source);
        json["rowMission"] = legacy;
        MapDocument decoded;
        QString error;
        QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error), qPrintable(error));
        QVERIFY(decoded.hasLegacyRowMissionDocument);
        QCOMPARE(decoded.legacyRowMissionDocument, legacy);
        QCOMPARE(MapDocumentCodec::toJson(decoded)["rowMission"].toObject(), legacy);
    }
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

void MapDocumentTest::rejectsDamagedStraightTaskWithoutReplacingMap()
{
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 2.0, 3.0, 0.0}};
    source.hasRowWorkPlan = true;
    source.rowWorkPlan.hasStartPose = source.rowWorkPlan.hasEndPose = true;
    source.rowWorkPlan.endPose = {2.0, 0.0, 0.0};
    source.rowWorkPlan.checkpoints = {{QStringLiteral("停留点"), 1.0, 1750}};
    const auto valid = MapDocumentCodec::toJson(source);
    auto damagedTask = valid["rowWork"].toObject();
    auto checkpoints = damagedTask["checkpoints"].toArray();
    auto checkpoint = checkpoints[0].toObject();
    checkpoint["dwellMs"] = "1750";
    checkpoints[0] = checkpoint;
    damagedTask["checkpoints"] = checkpoints;
    for (const auto &value : {QJsonValue(false), QJsonValue("broken"), QJsonValue(QJsonValue::Null),
                             QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}), QJsonValue(damagedTask)}) {
        auto damaged = valid;
        damaged["rowWork"] = value;
        MapDocument previous = source;
        QString error;
        QVERIFY(!MapDocumentCodec::fromJson(damaged, &previous, &error));
        QVERIFY(error.contains("rowWork"));
        QCOMPARE(MapDocumentCodec::toJson(previous), valid);
    }
    auto noTask = valid;
    noTask.remove("rowWork");
    MapDocument decoded;
    QString error;
    QVERIFY2(MapDocumentCodec::fromJson(noTask, &decoded, &error), qPrintable(error));
    QVERIFY(!decoded.hasRowWorkPlan);
}

QTEST_MAIN(MapDocumentTest)
#include "test_mapdocument.moc"

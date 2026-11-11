#include <QtTest>

#include <QJsonObject>
#include <QJsonArray>
#include <limits>

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
    void pointDwellRoundTripsWithoutChangingGeometry();
    void legacyPointsDefaultToNoDwell();
    void rejectsMalformedPointDwellWithoutReplacingMap();
    void olderReaderRejectsPointDwellSchema();
    void rejectsInvalidTopologyWithoutReplacingMap_data();
    void rejectsInvalidTopologyWithoutReplacingMap();
    void rejectsMalformedFieldsWithoutReplacingMap_data();
    void rejectsMalformedFieldsWithoutReplacingMap();
    void legacyDefaultsAndOppositeDirectionsRemainValid();
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
    source.points = {{1, 0.0, 0.0, 0.0}, {2, 1.0, 0.0, 0.0}};
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
    source.points = {{1, 0.0, 0.0, 0.0}, {2, 1.0, 0.0, 0.0}};
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

void MapDocumentTest::pointDwellRoundTripsWithoutChangingGeometry()
{
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 1.234567890123, -2.345678901234, .456789012345, 0},
                     {2, 3.456789012345, -4.567890123456, -.567890123456, 1750},
                     {3, 5.678901234567, -6.789012345678, .678901234567, 3600000}};
    source.paths = {{4, 1, 2, MapDocumentPathType::Line, 0.0, false},
                    {5, 2, 3, MapDocumentPathType::Arc, .25, true}};
    const auto json = MapDocumentCodec::toJson(source);
    QCOMPARE(json["schemaVersion"].toInt(), 4);
    MapDocument decoded;
    QString error;
    QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error), qPrintable(error));
    QCOMPARE(MapDocumentCodec::toJson(decoded), json);
    QCOMPARE(decoded.points.size(), source.points.size());
    for (qsizetype i = 0; i < source.points.size(); ++i) {
        QCOMPARE(decoded.points[i].dwellMs, source.points[i].dwellMs);
        QCOMPARE(decoded.points[i].x, source.points[i].x);
        QCOMPARE(decoded.points[i].y, source.points[i].y);
        QCOMPARE(decoded.points[i].theta, source.points[i].theta);
    }
}

void MapDocumentTest::legacyPointsDefaultToNoDwell()
{
    for (const int version : {1, 2, 3}) {
        MapDocument source;
        source.schemaVersion = version;
        source.gridWidth = source.gridHeight = 10;
        source.points = {{1, 1.25, -2.5, .75}};
        auto json = MapDocumentCodec::toJson(source);
        auto points = json["points"].toArray();
        auto point = points[0].toObject();
        point.remove("dwellMs");
        points[0] = point;
        json["points"] = points;
        MapDocument decoded;
        QString error;
        QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.points.first().dwellMs, 0);
        QCOMPARE(decoded.points.first().x, source.points.first().x);
        QCOMPARE(decoded.points.first().y, source.points.first().y);
        QCOMPARE(decoded.points.first().theta, source.points.first().theta);
    }
}

void MapDocumentTest::rejectsMalformedPointDwellWithoutReplacingMap()
{
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 1.25, -2.5, .75, 1234}};
    const auto valid = MapDocumentCodec::toJson(source);
    for (const auto &value : {QJsonValue(-1), QJsonValue(3600001), QJsonValue(.5),
                             QJsonValue("1234"), QJsonValue(true), QJsonValue(QJsonValue::Null),
                             QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}),
                             QJsonValue(std::numeric_limits<double>::infinity()),
                             QJsonValue(std::numeric_limits<double>::quiet_NaN())}) {
        auto damaged = valid;
        auto points = damaged["points"].toArray();
        auto point = points[0].toObject();
        point["dwellMs"] = value;
        points[0] = point;
        damaged["points"] = points;
        MapDocument previous = source;
        QString error;
        QVERIFY(!MapDocumentCodec::fromJson(damaged, &previous, &error));
        QVERIFY(error.contains("dwellMs"));
        QCOMPARE(MapDocumentCodec::toJson(previous), valid);
    }
}

void MapDocumentTest::olderReaderRejectsPointDwellSchema()
{
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 1.25, -2.5, .75, 1234}};
    const auto json = MapDocumentCodec::toJson(source);
    QCOMPARE(json["schemaVersion"].toInt(), 4);
    MapDocument decoded;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(json, &decoded, &error, 3));
    QVERIFY(error.contains("schemaVersion"));
    QVERIFY2(MapDocumentCodec::fromJson(json, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.schemaVersion, 4);
    decoded.points[0].dwellMs = 0;
    QCOMPARE(MapDocumentCodec::toJson(decoded)["schemaVersion"].toInt(), 4);
    source.points[0].dwellMs = 0;
    QCOMPARE(MapDocumentCodec::toJson(source)["schemaVersion"].toInt(), 3);
}

void MapDocumentTest::rejectsInvalidTopologyWithoutReplacingMap_data()
{
    QTest::addColumn<QString>("damage");
    for (const auto *name : {"duplicate-point-id", "duplicate-path-id", "duplicate-direction",
                             "missing-start", "missing-end", "self-loop", "zero-length",
                             "overflow-chord", "overflow-arc", "arc-without-sagitta"})
        QTest::newRow(name) << QString::fromLatin1(name);
}

void MapDocumentTest::rejectsInvalidTopologyWithoutReplacingMap()
{
    QFETCH(QString, damage);
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 0, 0, .25, 2000}, {2, 1, 0, .5, 0}, {3, 2, 0, .75, 1750}};
    source.paths = {{11, 1, 2, MapDocumentPathType::Line, 0, false},
                    {12, 2, 3, MapDocumentPathType::Arc, .2, true}};
    const auto valid = MapDocumentCodec::toJson(source);
    auto damaged = valid;
    auto points = damaged["points"].toArray();
    auto paths = damaged["paths"].toArray();
    auto point = points[1].toObject();
    auto path = paths[1].toObject();
    if (damage == "duplicate-point-id") point["id"] = 1;
    else if (damage == "duplicate-path-id") path["id"] = 11;
    else if (damage == "duplicate-direction") { path["start"] = 1; path["end"] = 2; }
    else if (damage == "missing-start") path["start"] = 99;
    else if (damage == "missing-end") path["end"] = 99;
    else if (damage == "self-loop") path["end"] = 2;
    else if (damage == "zero-length") { point["x"] = 0; point["y"] = 0; }
    else if (damage == "overflow-chord") {
        auto first = points[0].toObject();
        first["x"] = -std::numeric_limits<double>::max();
        points[0] = first;
        point["x"] = std::numeric_limits<double>::max();
    } else if (damage == "overflow-arc") {
        auto last = points[2].toObject();
        last["x"] = 1e200;
        points[2] = last;
    }
    else if (damage == "arc-without-sagitta") path.remove("sagitta");
    points[1] = point;
    paths[1] = path;
    damaged["points"] = points;
    damaged["paths"] = paths;
    MapDocument previous = source;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(damaged, &previous, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(MapDocumentCodec::toJson(previous), valid);
}

void MapDocumentTest::rejectsMalformedFieldsWithoutReplacingMap_data()
{
    QTest::addColumn<QString>("scope");
    QTest::addColumn<QString>("field");
    QTest::addColumn<QJsonValue>("value");
    auto add = [](const char *scope, const char *field, const char *name, const QJsonValue &value) {
        QTest::newRow((QByteArray(scope) + '-' + field + '-' + name).constData())
            << QString::fromLatin1(scope) << QString::fromLatin1(field) << value;
    };
    for (const auto *field : {"schemaVersion", "gridWidth", "gridHeight"}) {
        add("root", field, "string", QJsonValue("3"));
        add("root", field, "bool", QJsonValue(true));
        add("root", field, "null", QJsonValue(QJsonValue::Null));
        add("root", field, "fraction", QJsonValue(1.5));
        add("root", field, "zero", QJsonValue(0));
    }
    add("root", "gridWidth", "too-large", QJsonValue(501));
    for (const auto *field : {"cellSizeMeters", "rotationDeg"}) {
        add("root", field, "string", QJsonValue("1"));
        add("root", field, "null", QJsonValue(QJsonValue::Null));
        add("root", field, "bool", QJsonValue(true));
        add("root", field, "nonfinite", QJsonValue(std::numeric_limits<double>::infinity()));
    }
    add("root", "cellSizeMeters", "zero", QJsonValue(0));
    for (const auto *field : {"points", "paths"}) {
        add("root", field, "object", QJsonValue(QJsonObject{}));
        add("root", field, "null", QJsonValue(QJsonValue::Null));
    }
    for (const auto *scope : {"point", "path"}) {
        add(scope, "id", "fraction", QJsonValue(.5));
        add(scope, "id", "overflow-next-id", QJsonValue(std::numeric_limits<int>::max()));
        add(scope, "id", "string", QJsonValue("1"));
        add(scope, "id", "missing", QJsonValue(QJsonValue::Undefined));
    }
    for (const auto *field : {"x", "y", "theta"}) {
        add("point", field, "string", QJsonValue("1"));
        add("point", field, "null", QJsonValue(QJsonValue::Null));
        add("point", field, "bool", QJsonValue(false));
        add("point", field, "missing", QJsonValue(QJsonValue::Undefined));
        add("point", field, "nonfinite", QJsonValue(std::numeric_limits<double>::infinity()));
    }
    for (const auto *field : {"start", "end", "sagitta"}) {
        add("path", field, "string", QJsonValue("1"));
        add("path", field, "null", QJsonValue(QJsonValue::Null));
        add("path", field, "bool", QJsonValue(false));
    }
    add("path", "start", "fraction", QJsonValue(1.5));
    add("path", "end", "missing", QJsonValue(QJsonValue::Undefined));
    add("path", "type", "unknown", QJsonValue("curve"));
    add("path", "type", "missing", QJsonValue(QJsonValue::Undefined));
    add("path", "type", "bool", QJsonValue(false));
}

void MapDocumentTest::rejectsMalformedFieldsWithoutReplacingMap()
{
    QFETCH(QString, scope);
    QFETCH(QString, field);
    QFETCH(QJsonValue, value);
    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{1, 0, 0, .25, 2000}, {2, 1, 0, .5, 0}};
    source.paths = {{11, 1, 2, MapDocumentPathType::Line, 0, false}};
    const auto valid = MapDocumentCodec::toJson(source);
    auto damaged = valid;
    if (scope == "root") {
        damaged[field] = value;
    } else {
        const QString arrayName = scope == "point" ? "points" : "paths";
        auto array = damaged[arrayName].toArray();
        auto entry = array[0].toObject();
        entry[field] = value;
        array[0] = entry;
        damaged[arrayName] = array;
    }
    MapDocument previous = source;
    QString error;
    QVERIFY(!MapDocumentCodec::fromJson(damaged, &previous, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(MapDocumentCodec::toJson(previous), valid);
}

void MapDocumentTest::legacyDefaultsAndOppositeDirectionsRemainValid()
{
    MapDocument decoded;
    QString error;
    QVERIFY2(MapDocumentCodec::fromJson({{"gridWidth", 10}, {"gridHeight", 10}}, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.schemaVersion, 1);
    QCOMPARE(decoded.cellSizeMeters, 1.0);
    QCOMPARE(decoded.rotationDeg, 0.0);
    QVERIFY(decoded.points.isEmpty());
    QVERIFY(decoded.paths.isEmpty());

    MapDocument source;
    source.gridWidth = source.gridHeight = 10;
    source.points = {{0, 0, 0, 0, 0}, {2, 1.25, -2.5, .75, 2000}};
    source.paths = {{11, 0, 2, MapDocumentPathType::Line, 0, false},
                    {12, 2, 0, MapDocumentPathType::Arc, -.25, true}};
    auto valid = MapDocumentCodec::toJson(source);
    auto paths = valid["paths"].toArray();
    auto line = paths[0].toObject();
    line.remove("sagitta"); // Historic lines did not require an arc parameter.
    paths[0] = line;
    valid["paths"] = paths;
    QVERIFY2(MapDocumentCodec::fromJson(valid, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.points[0].dwellMs, 0);
    QCOMPARE(decoded.points[1].dwellMs, 2000);
    QCOMPARE(decoded.paths.size(), 2);
    QCOMPARE(decoded.paths[0].sagitta, 0.0);
    QVERIFY(!decoded.paths[0].bidirectional);
    QVERIFY(decoded.paths[1].bidirectional);
}

QTEST_MAIN(MapDocumentTest)
#include "test_mapdocument.moc"

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

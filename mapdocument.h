#ifndef MAPDOCUMENT_H
#define MAPDOCUMENT_H

#include "rowworktypes.h"
#include "trackingtypes.h"

#include <QList>
#include <QJsonObject>
#include <QString>

struct MapDocumentPoint
{
    int id = -1;
    double x = 0.0;
    double y = 0.0;
    double theta = 0.0;
    int dwellMs = 0;
};

enum class MapDocumentPathType
{
    Line,
    Arc
};

struct MapDocumentPath
{
    int id = -1;
    int startId = -1;
    int endId = -1;
    MapDocumentPathType type = MapDocumentPathType::Line;
    double sagitta = 0.0;
    bool bidirectional = false;
};

struct MapDocument
{
    int schemaVersion = 3;
    MapFrameBinding frameBinding;
    QString savedAtIsoUtc;
    int gridWidth = 0;
    int gridHeight = 0;
    double cellSizeMeters = 1.0;
    double rotationDeg = 0.0;
    QList<MapDocumentPoint> points;
    QList<MapDocumentPath> paths;
    bool hasRowWorkPlan = false;
    RowWorkPlan rowWorkPlan;
    bool hasLegacyRowMissionDocument = false;
    QJsonObject legacyRowMissionDocument;
};

namespace MapDocumentCodec
{
QJsonObject toJson(const MapDocument &doc);
bool fromJson(const QJsonObject &json, MapDocument *outDoc, QString *errorMessage = nullptr, int maxSupportedSchemaVersion = 4);
} // namespace MapDocumentCodec

#endif // MAPDOCUMENT_H

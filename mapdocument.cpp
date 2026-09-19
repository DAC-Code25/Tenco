#include "mapdocument.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>

namespace
{
void setError(QString *errorMessage, const QString &message)
{
    if (errorMessage) {
        *errorMessage = message;
    }
}

bool isFiniteNumber(double value)
{
    return std::isfinite(value);
}
} // namespace

QJsonObject MapDocumentCodec::toJson(const MapDocument &doc)
{
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), doc.schemaVersion);
    root.insert("frameBinding", doc.frameBinding.toJson());
    if (!doc.savedAtIsoUtc.trimmed().isEmpty()) {
        root.insert(QStringLiteral("savedAt"), doc.savedAtIsoUtc.trimmed());
    }
    root.insert(QStringLiteral("gridWidth"), doc.gridWidth);
    root.insert(QStringLiteral("gridHeight"), doc.gridHeight);
    root.insert(QStringLiteral("cellSizeMeters"), doc.cellSizeMeters);
    root.insert(QStringLiteral("rotationDeg"), doc.rotationDeg);

    QJsonArray pointsArray;
    for (const MapDocumentPoint &point : doc.points) {
        QJsonObject pointObj;
        pointObj.insert(QStringLiteral("id"), point.id);
        pointObj.insert(QStringLiteral("x"), point.x);
        pointObj.insert(QStringLiteral("y"), point.y);
        pointObj.insert(QStringLiteral("theta"), point.theta);
        pointsArray.append(pointObj);
    }
    root.insert(QStringLiteral("points"), pointsArray);

    QJsonArray pathsArray;
    for (const MapDocumentPath &path : doc.paths) {
        QJsonObject pathObj;
        pathObj.insert(QStringLiteral("id"), path.id);
        pathObj.insert(QStringLiteral("start"), path.startId);
        pathObj.insert(QStringLiteral("end"), path.endId);
        pathObj.insert(QStringLiteral("type"), path.type == MapDocumentPathType::Arc ? QStringLiteral("arc")
                                                                                     : QStringLiteral("line"));
        pathObj.insert(QStringLiteral("sagitta"), path.sagitta);
        pathsArray.append(pathObj);
    }
    root.insert(QStringLiteral("paths"), pathsArray);

    if (doc.hasRowWorkPlan) {
        root.insert(QStringLiteral("rowWork"), RowWorkJson::planToJson(doc.rowWorkPlan));
    }

    if (doc.hasRowMissionPlan) {
        root.insert(QStringLiteral("rowMission"), RowMissionJson::planToJson(doc.rowMissionPlan));
    }

    return root;
}

bool MapDocumentCodec::fromJson(const QJsonObject &json,
                                MapDocument *outDoc,
                                QString *errorMessage,
                                int maxSupportedSchemaVersion)
{
    if (!outDoc) {
        setError(errorMessage, QStringLiteral("输出对象为空"));
        return false;
    }
    if (!json.contains(QStringLiteral("gridWidth")) || !json.contains(QStringLiteral("gridHeight"))) {
        setError(errorMessage, QStringLiteral("缺少 gridWidth/gridHeight"));
        return false;
    }

    MapDocument doc;
    const int schemaVersion = json.value(QStringLiteral("schemaVersion")).toInt(1);
    if (schemaVersion < 1 || schemaVersion > maxSupportedSchemaVersion) {
        setError(errorMessage,
                 QStringLiteral("schemaVersion 过新: %1 > %2").arg(schemaVersion).arg(maxSupportedSchemaVersion));
        return false;
    }
    doc.schemaVersion = schemaVersion;
    if (schemaVersion >= 2) doc.frameBinding = MapFrameBinding::fromJson(json["frameBinding"].toObject());
    doc.savedAtIsoUtc = json.value(QStringLiteral("savedAt")).toString().trimmed();

    doc.gridWidth = json.value(QStringLiteral("gridWidth")).toInt();
    doc.gridHeight = json.value(QStringLiteral("gridHeight")).toInt();
    if (doc.gridWidth <= 0 || doc.gridHeight <= 0) {
        setError(errorMessage, QStringLiteral("gridWidth/gridHeight 必须大于 0"));
        return false;
    }

    doc.cellSizeMeters = json.value(QStringLiteral("cellSizeMeters")).toDouble(1.0);
    doc.rotationDeg = json.value(QStringLiteral("rotationDeg")).toDouble(0.0);
    if (!isFiniteNumber(doc.cellSizeMeters) || doc.cellSizeMeters <= 0.0) {
        setError(errorMessage, QStringLiteral("cellSizeMeters 非法"));
        return false;
    }
    if (!isFiniteNumber(doc.rotationDeg)) {
        setError(errorMessage, QStringLiteral("rotationDeg 非法"));
        return false;
    }

    const QJsonArray pointsArray = json.value(QStringLiteral("points")).toArray();
    doc.points.reserve(pointsArray.size());
    for (const QJsonValue &pointValue : pointsArray) {
        if (!pointValue.isObject()) {
            setError(errorMessage, QStringLiteral("points 条目必须是对象"));
            return false;
        }
        const QJsonObject pointObj = pointValue.toObject();
        MapDocumentPoint point;
        point.id = pointObj.value(QStringLiteral("id")).toInt(-1);
        point.x = pointObj.value(QStringLiteral("x")).toDouble();
        point.y = pointObj.value(QStringLiteral("y")).toDouble();
        point.theta = pointObj.value(QStringLiteral("theta")).toDouble();
        if (point.id < 0 || !isFiniteNumber(point.x) || !isFiniteNumber(point.y) || !isFiniteNumber(point.theta)) {
            setError(errorMessage, QStringLiteral("points 条目包含非法字段"));
            return false;
        }
        doc.points.append(point);
    }

    const QJsonArray pathsArray = json.value(QStringLiteral("paths")).toArray();
    doc.paths.reserve(pathsArray.size());
    for (const QJsonValue &pathValue : pathsArray) {
        if (!pathValue.isObject()) {
            setError(errorMessage, QStringLiteral("paths 条目必须是对象"));
            return false;
        }
        const QJsonObject pathObj = pathValue.toObject();
        MapDocumentPath path;
        path.id = pathObj.value(QStringLiteral("id")).toInt(-1);
        path.startId = pathObj.value(QStringLiteral("start")).toInt(-1);
        path.endId = pathObj.value(QStringLiteral("end")).toInt(-1);
        path.sagitta = pathObj.value(QStringLiteral("sagitta")).toDouble(0.0);
        const QString type = pathObj.value(QStringLiteral("type")).toString().trimmed().toLower();
        if (type == QStringLiteral("arc")) {
            path.type = MapDocumentPathType::Arc;
        } else {
            path.type = MapDocumentPathType::Line;
        }

        if (path.id < 0 || path.startId < 0 || path.endId < 0 || !isFiniteNumber(path.sagitta)) {
            setError(errorMessage, QStringLiteral("paths 条目包含非法字段"));
            return false;
        }
        doc.paths.append(path);
    }

    const QJsonObject rowWorkObj = json.value(QStringLiteral("rowWork")).toObject();
    if (!rowWorkObj.isEmpty()) {
        RowWorkPlan rowWorkPlan;
        if (!RowWorkJson::planFromJson(rowWorkObj, &rowWorkPlan)) {
            setError(errorMessage, QStringLiteral("rowWork 字段无效"));
            return false;
        }
        doc.hasRowWorkPlan = true;
        doc.rowWorkPlan = rowWorkPlan;
    }

    const QJsonObject rowMissionObj = json.value(QStringLiteral("rowMission")).toObject();
    if (!rowMissionObj.isEmpty()) {
        RowMissionPlan rowMissionPlan;
        if (!RowMissionJson::planFromJson(rowMissionObj, &rowMissionPlan)) {
            setError(errorMessage, QStringLiteral("rowMission 字段无效"));
            return false;
        }
        doc.hasRowMissionPlan = true;
        doc.rowMissionPlan = rowMissionPlan;
    }

    *outDoc = doc;
    return true;
}

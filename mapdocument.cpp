#include "mapdocument.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QPointF>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

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

bool readNumber(const QJsonValue &value, double *number)
{
    if (!value.isDouble() || !isFiniteNumber(value.toDouble())) return false;
    *number = value.toDouble();
    return true;
}

bool readInteger(const QJsonValue &value, int minimum, int maximum, int *number)
{
    double parsed = 0.0;
    if (!readNumber(value, &parsed) || parsed != std::floor(parsed) ||
        parsed < minimum || parsed > maximum) return false;
    *number = static_cast<int>(parsed);
    return true;
}
} // namespace

QJsonObject MapDocumentCodec::toJson(const MapDocument &doc)
{
    QJsonObject root;
    // Readers predating point dwell must reject this document, rather than
    // silently dropping its stop actions on the next upload/save.
    const bool hasPointDwell = std::any_of(doc.points.cbegin(), doc.points.cend(),
                                         [](const auto &point) { return point.dwellMs != 0; });
    const int schemaVersion = std::max(doc.schemaVersion, hasPointDwell ? 4 : doc.schemaVersion);
    root.insert(QStringLiteral("schemaVersion"), schemaVersion);
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
        pointObj.insert(QStringLiteral("dwellMs"), point.dwellMs);
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
        if (schemaVersion >= 3) {
            pathObj.insert(QStringLiteral("bidirectional"), path.bidirectional);
        }
        pathsArray.append(pathObj);
    }
    root.insert(QStringLiteral("paths"), pathsArray);

    if (doc.hasRowWorkPlan) {
        root.insert(QStringLiteral("rowWork"), RowWorkJson::planToJson(doc.rowWorkPlan));
    }

    if (doc.hasLegacyRowMissionDocument || !doc.legacyRowMissionDocument.isEmpty()) {
        root.insert(QStringLiteral("rowMission"), doc.legacyRowMissionDocument);
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
    int schemaVersion = 1;
    if (json.contains(QStringLiteral("schemaVersion")) &&
        !readInteger(json.value(QStringLiteral("schemaVersion")), 1, maxSupportedSchemaVersion, &schemaVersion)) {
        setError(errorMessage,
                 QStringLiteral("schemaVersion 必须为 1～%1 的整数").arg(maxSupportedSchemaVersion));
        return false;
    }
    doc.schemaVersion = schemaVersion;
    if (schemaVersion >= 2) doc.frameBinding = MapFrameBinding::fromJson(json["frameBinding"].toObject());
    doc.savedAtIsoUtc = json.value(QStringLiteral("savedAt")).toString().trimmed();

    if (!readInteger(json.value(QStringLiteral("gridWidth")), 1, 500, &doc.gridWidth) ||
        !readInteger(json.value(QStringLiteral("gridHeight")), 1, 500, &doc.gridHeight)) {
        setError(errorMessage, QStringLiteral("gridWidth/gridHeight 必须为 1～500 的整数"));
        return false;
    }

    if ((json.contains(QStringLiteral("cellSizeMeters")) &&
         !readNumber(json.value(QStringLiteral("cellSizeMeters")), &doc.cellSizeMeters)) ||
        doc.cellSizeMeters <= 0.0) {
        setError(errorMessage, QStringLiteral("cellSizeMeters 非法"));
        return false;
    }
    if (json.contains(QStringLiteral("rotationDeg")) &&
        !readNumber(json.value(QStringLiteral("rotationDeg")), &doc.rotationDeg)) {
        setError(errorMessage, QStringLiteral("rotationDeg 非法"));
        return false;
    }

    if ((json.contains(QStringLiteral("points")) && !json.value(QStringLiteral("points")).isArray()) ||
        (json.contains(QStringLiteral("paths")) && !json.value(QStringLiteral("paths")).isArray())) {
        setError(errorMessage, QStringLiteral("points/paths 必须为数组"));
        return false;
    }

    const QJsonArray pointsArray = json.value(QStringLiteral("points")).toArray();
    doc.points.reserve(pointsArray.size());
    QSet<int> pointIds;
    QHash<int, QPointF> pointPositions;
    for (const QJsonValue &pointValue : pointsArray) {
        if (!pointValue.isObject()) {
            setError(errorMessage, QStringLiteral("points 条目必须是对象"));
            return false;
        }
        const QJsonObject pointObj = pointValue.toObject();
        MapDocumentPoint point;
        // The next editor ID is id + 1, so INT_MAX itself is not representable.
        if (!readInteger(pointObj.value(QStringLiteral("id")), 0, std::numeric_limits<int>::max() - 1, &point.id) ||
            !readNumber(pointObj.value(QStringLiteral("x")), &point.x) ||
            !readNumber(pointObj.value(QStringLiteral("y")), &point.y) ||
            !readNumber(pointObj.value(QStringLiteral("theta")), &point.theta) || pointIds.contains(point.id)) {
            setError(errorMessage, QStringLiteral("points 条目包含非法字段或重复编号"));
            return false;
        }
        if (pointObj.contains(QStringLiteral("dwellMs"))) {
            const auto value = pointObj.value(QStringLiteral("dwellMs"));
            const double duration = value.toDouble();
            if (!value.isDouble() || !isFiniteNumber(duration) || duration != std::floor(duration) ||
                duration < 0 || duration > 3600000) {
                setError(errorMessage, QStringLiteral("points 的 dwellMs 必须为 0～3600000 的整数"));
                return false;
            }
            point.dwellMs = static_cast<int>(duration);
        }
        pointIds.insert(point.id);
        pointPositions.insert(point.id, QPointF(point.x, point.y));
        doc.points.append(point);
    }

    const QJsonArray pathsArray = json.value(QStringLiteral("paths")).toArray();
    doc.paths.reserve(pathsArray.size());
    QSet<int> pathIds;
    QSet<QPair<int, int>> directedPairs;
    for (const QJsonValue &pathValue : pathsArray) {
        if (!pathValue.isObject()) {
            setError(errorMessage, QStringLiteral("paths 条目必须是对象"));
            return false;
        }
        const QJsonObject pathObj = pathValue.toObject();
        MapDocumentPath path;
        if (!readInteger(pathObj.value(QStringLiteral("id")), 0, std::numeric_limits<int>::max() - 1, &path.id) ||
            !readInteger(pathObj.value(QStringLiteral("start")), 0, std::numeric_limits<int>::max() - 1, &path.startId) ||
            !readInteger(pathObj.value(QStringLiteral("end")), 0, std::numeric_limits<int>::max() - 1, &path.endId) ||
            (pathObj.contains(QStringLiteral("sagitta")) &&
             !readNumber(pathObj.value(QStringLiteral("sagitta")), &path.sagitta))) {
            setError(errorMessage, QStringLiteral("paths 条目包含非法字段"));
            return false;
        }
        if (schemaVersion >= 3 && pathObj.contains(QStringLiteral("bidirectional"))) {
            const QJsonValue direction = pathObj.value(QStringLiteral("bidirectional"));
            if (!direction.isBool()) {
                setError(errorMessage, QStringLiteral("paths 的 bidirectional 必须为布尔值"));
                return false;
            }
            path.bidirectional = direction.toBool();
        }
        const QString type = pathObj.value(QStringLiteral("type")).toString().trimmed().toLower();
        if (type == QStringLiteral("arc")) {
            path.type = MapDocumentPathType::Arc;
        } else if (type == QStringLiteral("line")) {
            path.type = MapDocumentPathType::Line;
        } else {
            setError(errorMessage, QStringLiteral("paths 的 type 必须为 line 或 arc"));
            return false;
        }

        const QPair<int, int> endpoints(path.startId, path.endId);
        if (pathIds.contains(path.id) || directedPairs.contains(endpoints) ||
            !pointIds.contains(path.startId) || !pointIds.contains(path.endId) || path.startId == path.endId) {
            setError(errorMessage, QStringLiteral("paths 存在重复编号、重复方向、自环或不存在的端点"));
            return false;
        }
        const auto start = pointPositions.value(path.startId);
        const auto end = pointPositions.value(path.endId);
        const double chord = std::hypot(end.x() - start.x(), end.y() - start.y());
        if (!isFiniteNumber(chord) || chord <= 0.0 ||
            (path.type == MapDocumentPathType::Arc && !pathObj.contains(QStringLiteral("sagitta")))) {
            setError(errorMessage, QStringLiteral("paths 的几何无效"));
            return false;
        }
        if (path.type == MapDocumentPathType::Arc && std::abs(path.sagitta) >= 1e-3) {
            const double radius = chord * chord / (8.0 * std::abs(path.sagitta)) + std::abs(path.sagitta) / 2.0;
            // Match the editor's arc arithmetic before allowing it to replace a map.
            if (!isFiniteNumber(radius) ||
                !isFiniteNumber((start.x() + end.x()) / 2.0) ||
                !isFiniteNumber((start.y() + end.y()) / 2.0) ||
                !isFiniteNumber(std::hypot(start.x(), start.y()) + radius * 2.0) ||
                !isFiniteNumber(std::hypot(end.x(), end.y()) + radius * 2.0)) {
                setError(errorMessage, QStringLiteral("paths 的圆弧几何非有限"));
                return false;
            }
        }
        pathIds.insert(path.id);
        directedPairs.insert(endpoints);
        doc.paths.append(path);
    }

    if (json.contains(QStringLiteral("rowWork"))) {
        const QJsonValue rowWorkValue = json.value(QStringLiteral("rowWork"));
        RowWorkPlan rowWorkPlan;
        if (!rowWorkValue.isObject() || !RowWorkJson::planFromJson(rowWorkValue.toObject(), &rowWorkPlan)) {
            setError(errorMessage, QStringLiteral("rowWork 字段无效"));
            return false;
        }
        doc.hasRowWorkPlan = true;
        doc.rowWorkPlan = rowWorkPlan;
    }

    const QJsonValue rowMissionValue = json.value(QStringLiteral("rowMission"));
    if (rowMissionValue.isObject()) {
        const QJsonObject rowMissionObj = rowMissionValue.toObject();
        // Retired task data is opaque document content. It has no model or compiler.
        doc.hasLegacyRowMissionDocument = true;
        doc.legacyRowMissionDocument = rowMissionObj;
    }

    *outDoc = doc;
    return true;
}

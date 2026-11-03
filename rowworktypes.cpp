#include "rowworktypes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr double kMinLineLengthMeters = 1e-3;

bool isFiniteNumber(double value)
{
    return std::isfinite(value);
}

bool readInteger(const QJsonObject &json, const QString &key, int minimum, int maximum, int *out)
{
    if (!json.contains(key)) return true;
    const auto value = json.value(key);
    const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || !isFiniteNumber(number) || number != std::floor(number)
        || number < minimum || number > maximum) return false;
    *out = static_cast<int>(number);
    return true;
}

bool readBoolean(const QJsonObject &json, const QString &key, bool *out)
{
    if (!json.contains(key)) return true;
    const auto value = json.value(key);
    if (!value.isBool()) return false;
    *out = value.toBool();
    return true;
}

struct PathStation {
    double progress;
    QPointF position;
};

bool pathStations(const RowWorkPlan &plan, QList<PathStation> *out)
{
    if (!RowWorkGeometry::hasUsableLine(plan)) return false;
    const double length = plan.lineLength();
    const auto start = plan.startPose.toPointF();
    const auto tangent = (plan.endPose.toPointF() - start) / length;
    QList<PathStation> stations{{0, start}, {length, plan.endPose.toPointF()}};
    for (const auto &checkpoint : plan.checkpoints) {
        if (checkpoint.pendingMapPosition || !isFiniteNumber(checkpoint.progress)
            || checkpoint.progress < 0 || checkpoint.progress > length) return false;
        if (!checkpoint.recordedMapPosition) continue;
        const auto point = *checkpoint.recordedMapPosition;
        if (!isFiniteNumber(point.x()) || !isFiniteNumber(point.y())) return false;
        const double station = QPointF::dotProduct(point - start, tangent);
        if (!isFiniteNumber(station) || station < -1e-9 || station > length + 1e-9
            || std::abs(station - checkpoint.progress) > 1e-7) return false;
        stations.append({checkpoint.progress, point});
    }
    std::stable_sort(stations.begin(), stations.end(), [](const auto &a, const auto &b) {
        return a.progress < b.progress;
    });
    QList<PathStation> unique;
    for (const auto &station : stations) {
        if (!unique.isEmpty() && std::abs(station.progress - unique.last().progress) <= 1e-9) {
            // One station cannot describe two different sides of the line.
            if (std::hypot(station.position.x() - unique.last().position.x(),
                           station.position.y() - unique.last().position.y()) > 1e-7) return false;
        } else {
            unique.append(station);
        }
    }
    *out = unique;
    return true;
}

QPointF interpolate(const QList<PathStation> &stations, double progress)
{
    for (qsizetype i = 1; i < stations.size(); ++i) {
        if (progress <= stations[i].progress) {
            const auto &a = stations[i - 1], &b = stations[i];
            return a.position + (b.position - a.position)
                * ((progress - a.progress) / (b.progress - a.progress));
        }
    }
    return stations.last().position;
}

} // namespace

bool RowWorkPlan::isValid() const
{
    return RowWorkGeometry::hasUsablePath(*this);
}

double RowWorkPlan::lineLength() const
{
    return RowWorkGeometry::lineLength(*this);
}

bool RowWorkGeometry::hasUsableStart(const RowWorkPlan &plan)
{
    return plan.hasStartPose && isFiniteNumber(plan.startPose.x)
        && isFiniteNumber(plan.startPose.y) && isFiniteNumber(plan.startPose.yaw);
}

bool RowWorkGeometry::hasUsableLine(const RowWorkPlan &plan)
{
    return hasUsableStart(plan)
           && plan.hasEndPose
           && isFiniteNumber(plan.endPose.yaw)
           && isFiniteNumber(lineLength(plan))
           && lineLength(plan) >= kMinLineLengthMeters;
}

bool RowWorkGeometry::hasUsablePath(const RowWorkPlan &plan)
{
    QList<PathStation> stations;
    return pathStations(plan, &stations);
}

double RowWorkGeometry::lineLength(const RowWorkPlan &plan)
{
    if (!plan.hasStartPose || !plan.hasEndPose) {
        return 0.0;
    }
    const double dx = plan.endPose.x - plan.startPose.x;
    const double dy = plan.endPose.y - plan.startPose.y;
    return std::hypot(dx, dy);
}

double RowWorkGeometry::clampProgress(const RowWorkPlan &plan, double progress)
{
    const double length = lineLength(plan);
    if (length <= 0.0) {
        return 0.0;
    }
    return std::clamp(progress, 0.0, length);
}

double RowWorkGeometry::projectPointToProgress(const RowWorkPlan &plan,
                                               const QPointF &point,
                                               double *outSignedLateralError,
                                               QPointF *outProjectedPoint)
{
    if (!hasUsableLine(plan)) {
        if (outSignedLateralError) {
            *outSignedLateralError = 0.0;
        }
        if (outProjectedPoint) {
            *outProjectedPoint = QPointF();
        }
        return 0.0;
    }

    const QPointF start = plan.startPose.toPointF();
    const QPointF end = plan.endPose.toPointF();
    const QPointF lineVector = end - start;
    const double length = lineLength(plan);
    const QPointF tangent(lineVector.x() / length, lineVector.y() / length);
    const QPointF normal(-tangent.y(), tangent.x());
    const QPointF relative = point - start;

    const double rawProgress = QPointF::dotProduct(relative, tangent);
    const double signedLateralError = QPointF::dotProduct(relative, normal);
    const double clampedProgress = clampProgress(plan, rawProgress);

    if (outSignedLateralError) {
        *outSignedLateralError = signedLateralError;
    }
    if (outProjectedPoint) {
        *outProjectedPoint = start + tangent * clampedProgress;
    }
    return clampedProgress;
}

QPointF RowWorkGeometry::pointAtProgress(const RowWorkPlan &plan, double progress)
{
    QList<PathStation> stations;
    if (!isFiniteNumber(progress) || !pathStations(plan, &stations)) return {};
    return interpolate(stations, clampProgress(plan, progress));
}

QList<QPointF> RowWorkGeometry::pathBetweenProgress(const RowWorkPlan &plan, double from, double to)
{
    QList<PathStation> stations;
    if (!isFiniteNumber(from) || !isFiniteNumber(to) || !pathStations(plan, &stations)
        || from < 0 || to < 0 || from > plan.lineLength() || to > plan.lineLength()) return {};
    const double low = std::min(from, to), high = std::max(from, to);
    QList<QPointF> path{interpolate(stations, low)};
    for (const auto &station : stations)
        if (station.progress > low && station.progress < high) path.append(station.position);
    if (high > low) path.append(interpolate(stations, high));
    if (from > to) std::reverse(path.begin(), path.end());
    return path;
}

bool RowWorkGeometry::setCheckpointPosition(const RowWorkPlan &plan, const QPointF &point,
                                           RowCheckpoint *checkpoint)
{
    if (!checkpoint || !hasUsableStart(plan) || !isFiniteNumber(point.x()) || !isFiniteNumber(point.y()))
        return false;
    auto updated = *checkpoint;
    if (hasUsableLine(plan)) {
        const auto tangent = (plan.endPose.toPointF() - plan.startPose.toPointF()) / plan.lineLength();
        const double progress = QPointF::dotProduct(point - plan.startPose.toPointF(), tangent);
        if (!isFiniteNumber(progress) || progress < -1e-9 || progress > plan.lineLength() + 1e-9) return false;
        updated.progress = std::clamp(progress, 0.0, plan.lineLength());
        updated.recordedMapPosition = point;
        updated.pendingMapPosition.reset();
        auto candidate = plan;
        bool replaced = false;
        for (qsizetype i = 0; i < plan.checkpoints.size(); ++i) {
            if (&plan.checkpoints[i] != checkpoint) continue;
            candidate.checkpoints[i] = updated;
            replaced = true;
            break;
        }
        if (!replaced) candidate.checkpoints.append(updated);
        if (!hasUsablePath(candidate)) return false;
    } else {
        if (plan.hasEndPose) return false;
        const double distance = std::hypot(point.x() - plan.startPose.x, point.y() - plan.startPose.y);
        if (!isFiniteNumber(distance)) return false;
        updated.progress = distance;
        updated.pendingMapPosition = point;
        updated.recordedMapPosition.reset();
    }
    *checkpoint = updated;
    return true;
}

bool RowWorkGeometry::finalizeCheckpoints(RowWorkPlan *plan)
{
    if (!plan || !hasUsableLine(*plan)) return false;
    auto checkpoints = plan->checkpoints;
    const auto start = plan->startPose.toPointF();
    const double length = plan->lineLength();
    const auto tangent = (plan->endPose.toPointF() - start) / length;
    for (auto &checkpoint : checkpoints) {
        double progress = checkpoint.progress;
        if (checkpoint.pendingMapPosition || checkpoint.recordedMapPosition) {
            if (checkpoint.pendingMapPosition && checkpoint.recordedMapPosition) return false;
            const auto point = checkpoint.pendingMapPosition
                ? *checkpoint.pendingMapPosition : *checkpoint.recordedMapPosition;
            if (!isFiniteNumber(point.x()) || !isFiniteNumber(point.y())) return false;
            progress = QPointF::dotProduct(point - start, tangent);
            checkpoint.recordedMapPosition = point;
        }
        // Do not silently move points outside the finished segment onto an endpoint.
        if (!isFiniteNumber(progress) || progress < -1e-9 || progress > length + 1e-9) return false;
        checkpoint.progress = std::clamp(progress, 0.0, length);
        checkpoint.pendingMapPosition.reset();
    }
    auto candidate = *plan;
    candidate.checkpoints = checkpoints;
    if (!hasUsablePath(candidate)) return false;
    plan->checkpoints = checkpoints;
    return true;
}

QPointF RowWorkGeometry::checkpointPosition(const RowWorkPlan &plan, const RowCheckpoint &checkpoint)
{
    if (checkpoint.pendingMapPosition) return *checkpoint.pendingMapPosition;
    if (checkpoint.recordedMapPosition) return *checkpoint.recordedMapPosition;
    return pointAtProgress(plan, checkpoint.progress);
}

QString RowWorkJson::generatePlanId()
{
    return QStringLiteral("row-%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")));
}

QJsonObject RowWorkJson::poseToJson(const RowWorkPose &pose)
{
    return QJsonObject{
        {QStringLiteral("x"), pose.x},
        {QStringLiteral("y"), pose.y},
        {QStringLiteral("yaw"), pose.yaw}
    };
}

bool RowWorkJson::poseFromJson(const QJsonObject &json, RowWorkPose *outPose)
{
    if (!outPose || json.isEmpty()) {
        return false;
    }

    const double x = json.value(QStringLiteral("x")).toDouble(std::numeric_limits<double>::quiet_NaN());
    const double y = json.value(QStringLiteral("y")).toDouble(std::numeric_limits<double>::quiet_NaN());
    double yaw = json.value(QStringLiteral("yaw")).toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!isFiniteNumber(yaw) && json.contains(QStringLiteral("theta"))) {
        yaw = json.value(QStringLiteral("theta")).toDouble(std::numeric_limits<double>::quiet_NaN());
    }

    if (!isFiniteNumber(x) || !isFiniteNumber(y) || !isFiniteNumber(yaw)) {
        return false;
    }

    outPose->x = x;
    outPose->y = y;
    outPose->yaw = yaw;
    return true;
}

QJsonObject RowWorkJson::checkpointToJson(const RowCheckpoint &checkpoint)
{
    QJsonObject result{
        {QStringLiteral("name"), checkpoint.name.trimmed()},
        {QStringLiteral("progress"), checkpoint.progress},
        {QStringLiteral("dwellMs"), checkpoint.dwellMs},
        {QStringLiteral("capturePhoto"), checkpoint.capturePhoto},
        {QStringLiteral("actionTimeoutMs"), checkpoint.actionTimeoutMs},
        {QStringLiteral("enabled"), checkpoint.enabled},
        {QStringLiteral("triggerOnForward"), checkpoint.triggerOnForward},
        {QStringLiteral("triggerOnBackward"), checkpoint.triggerOnBackward}
    };
    if (checkpoint.pendingMapPosition) {
        result.insert(QStringLiteral("pendingMapPosition"), QJsonObject{
            {QStringLiteral("x"), checkpoint.pendingMapPosition->x()},
            {QStringLiteral("y"), checkpoint.pendingMapPosition->y()}});
    }
    if (checkpoint.recordedMapPosition) {
        result.insert(QStringLiteral("recordedMapPosition"), QJsonObject{
            {QStringLiteral("x"), checkpoint.recordedMapPosition->x()},
            {QStringLiteral("y"), checkpoint.recordedMapPosition->y()}});
    }
    return result;
}

bool RowWorkJson::checkpointFromJson(const QJsonObject &json, RowCheckpoint *outCheckpoint)
{
    if (!outCheckpoint || json.isEmpty()) {
        return false;
    }

    const double progress = json.value(QStringLiteral("progress")).toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!isFiniteNumber(progress) || progress < 0) {
        return false;
    }

    RowCheckpoint checkpoint;
    if ((json.contains("name") && !json["name"].isString())
        || !readInteger(json, "dwellMs", 0, 3600000, &checkpoint.dwellMs)
        || !readInteger(json, "actionTimeoutMs", 1000, 3600000, &checkpoint.actionTimeoutMs)
        || !readBoolean(json, "capturePhoto", &checkpoint.capturePhoto)
        || !readBoolean(json, "enabled", &checkpoint.enabled)
        || !readBoolean(json, "triggerOnForward", &checkpoint.triggerOnForward)
        || !readBoolean(json, "triggerOnBackward", &checkpoint.triggerOnBackward)) return false;
    checkpoint.name = json.value(QStringLiteral("name")).toString().trimmed();
    checkpoint.progress = progress;
    if (json.contains("pendingMapPosition")) {
        if (!json["pendingMapPosition"].isObject()) return false;
        const auto point = json["pendingMapPosition"].toObject();
        const double x = point["x"].toDouble(std::numeric_limits<double>::quiet_NaN());
        const double y = point["y"].toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!isFiniteNumber(x) || !isFiniteNumber(y)) return false;
        checkpoint.pendingMapPosition = QPointF(x, y);
    }
    if (json.contains("recordedMapPosition")) {
        if (checkpoint.pendingMapPosition || !json["recordedMapPosition"].isObject()) return false;
        const auto point = json["recordedMapPosition"].toObject();
        const double x = point["x"].toDouble(std::numeric_limits<double>::quiet_NaN());
        const double y = point["y"].toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!isFiniteNumber(x) || !isFiniteNumber(y)) return false;
        checkpoint.recordedMapPosition = QPointF(x, y);
    }
    *outCheckpoint = checkpoint;
    return true;
}

QJsonObject RowWorkJson::paramsToJson(const RowWorkParams &params)
{
    return QJsonObject{
        {QStringLiteral("loopEnabled"), params.loopEnabled}
    };
}

bool RowWorkJson::paramsFromJson(const QJsonObject &json, RowWorkParams *outParams)
{
    if (!outParams || json.isEmpty()) {
        return false;
    }

    // Older plans embedded motion settings here. They no longer override the
    // controller's applied configuration; retain only task business data.
    RowWorkParams params;
    if (!readBoolean(json, "loopEnabled", &params.loopEnabled)) return false;
    *outParams = params;
    return true;
}

QJsonObject RowWorkJson::planToJson(const RowWorkPlan &plan)
{
    const bool recorded = std::any_of(plan.checkpoints.begin(), plan.checkpoints.end(), [](const auto &checkpoint) {
        return checkpoint.recordedMapPosition.has_value();
    });
    QJsonObject root{
        // Older readers must reject taught XY rather than flatten it back onto AB.
        {QStringLiteral("schemaVersion"), recorded ? 3 : 2},
        {QStringLiteral("frameBinding"), plan.frameBinding.toJson()},
        {QStringLiteral("planId"), plan.planId.trimmed()},
        {QStringLiteral("version"), plan.version},
        {QStringLiteral("frameId"), plan.frameId.trimmed().isEmpty() ? QStringLiteral("map") : plan.frameId.trimmed()},
        {QStringLiteral("params"), paramsToJson(plan.params)}
    };

    if (plan.hasStartPose) {
        root.insert(QStringLiteral("startPoint"), poseToJson(plan.startPose));
    }
    if (plan.hasEndPose) {
        root.insert(QStringLiteral("endPoint"), poseToJson(plan.endPose));
    }

    QJsonArray checkpointsArray;
    for (const RowCheckpoint &checkpoint : plan.checkpoints) {
        checkpointsArray.append(checkpointToJson(checkpoint));
    }
    root.insert(QStringLiteral("checkpoints"), checkpointsArray);
    return root;
}

bool RowWorkJson::planFromJson(const QJsonObject &json, RowWorkPlan *outPlan)
{
    if (!outPlan || json.isEmpty()) {
        return false;
    }

    int schema = 1;
    if (!readInteger(json, "schemaVersion", 1, 3, &schema)) return false;
    RowWorkPlan plan;
    if ((json.contains("planId") && !json["planId"].isString())
        || (json.contains("frameId") && !json["frameId"].isString())
        || (json.contains("frameBinding") && !json["frameBinding"].isObject())
        || !readInteger(json, "version", 1, std::numeric_limits<int>::max(), &plan.version)) return false;
    if (schema >= 2) plan.frameBinding = MapFrameBinding::fromJson(json["frameBinding"].toObject());
    plan.planId = json.value(QStringLiteral("planId")).toString(plan.planId).trimmed();
    plan.frameId = json.value(QStringLiteral("frameId")).toString(plan.frameId).trimmed();
    if (plan.frameId.isEmpty()) {
        plan.frameId = QStringLiteral("map");
    }

    if (json.contains("startPoint")) {
        if (!json["startPoint"].isObject() || !poseFromJson(json["startPoint"].toObject(), &plan.startPose)) return false;
        plan.hasStartPose = true;
    }

    if (json.contains("endPoint")) {
        if (!json["endPoint"].isObject() || !poseFromJson(json["endPoint"].toObject(), &plan.endPose)) return false;
        plan.hasEndPose = true;
    }

    if (json.contains("params") && !json["params"].isObject()) return false;
    const QJsonObject paramsObj = json.value(QStringLiteral("params")).toObject();
    if (!paramsObj.isEmpty()) {
        if (!paramsFromJson(paramsObj, &plan.params)) return false;
    }

    QList<RowCheckpoint> checkpoints;
    if (json.contains("checkpoints") && !json["checkpoints"].isArray()) return false;
    const QJsonArray checkpointsArray = json.value(QStringLiteral("checkpoints")).toArray();
    checkpoints.reserve(checkpointsArray.size());
    for (const QJsonValue &value : checkpointsArray) {
        if (!value.isObject()) {
            return false;
        }
        RowCheckpoint checkpoint;
        if (!checkpointFromJson(value.toObject(), &checkpoint)) {
            return false;
        }
        checkpoints.append(checkpoint);
    }
    plan.checkpoints = checkpoints;
    const bool recorded = std::any_of(checkpoints.begin(), checkpoints.end(), [](const auto &checkpoint) {
        return checkpoint.recordedMapPosition.has_value();
    });
    if ((recorded && schema != 3) || (schema == 3 && !recorded)
        || (recorded && !RowWorkGeometry::hasUsablePath(plan))) return false;
    *outPlan = plan;
    return true;
}

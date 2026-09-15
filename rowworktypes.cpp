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

} // namespace

bool RowWorkPlan::isValid() const
{
    return RowWorkGeometry::hasUsableLine(*this);
}

double RowWorkPlan::lineLength() const
{
    return RowWorkGeometry::lineLength(*this);
}

bool RowWorkGeometry::hasUsableLine(const RowWorkPlan &plan)
{
    return plan.hasStartPose
           && plan.hasEndPose
           && lineLength(plan) >= kMinLineLengthMeters;
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
    if (!hasUsableLine(plan)) {
        return QPointF();
    }

    const QPointF start = plan.startPose.toPointF();
    const QPointF end = plan.endPose.toPointF();
    const QPointF lineVector = end - start;
    const double length = lineLength(plan);
    const QPointF tangent(lineVector.x() / length, lineVector.y() / length);
    return start + tangent * clampProgress(plan, progress);
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
    return QJsonObject{
        {QStringLiteral("name"), checkpoint.name.trimmed()},
        {QStringLiteral("progress"), checkpoint.progress},
        {QStringLiteral("dwellMs"), checkpoint.dwellMs},
        {QStringLiteral("capturePhoto"), checkpoint.capturePhoto},
        {QStringLiteral("actionTimeoutMs"), checkpoint.actionTimeoutMs},
        {QStringLiteral("enabled"), checkpoint.enabled},
        {QStringLiteral("triggerOnForward"), checkpoint.triggerOnForward},
        {QStringLiteral("triggerOnBackward"), checkpoint.triggerOnBackward}
    };
}

bool RowWorkJson::checkpointFromJson(const QJsonObject &json, RowCheckpoint *outCheckpoint)
{
    if (!outCheckpoint || json.isEmpty()) {
        return false;
    }

    const double progress = json.value(QStringLiteral("progress")).toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!isFiniteNumber(progress)) {
        return false;
    }

    outCheckpoint->name = json.value(QStringLiteral("name")).toString().trimmed();
    outCheckpoint->progress = progress;
    outCheckpoint->dwellMs = json.value(QStringLiteral("dwellMs")).toInt(outCheckpoint->dwellMs);
    outCheckpoint->capturePhoto = json.value("capturePhoto").toBool(false);
    outCheckpoint->actionTimeoutMs = json.value("actionTimeoutMs").toInt(30000);
    outCheckpoint->enabled = json.value(QStringLiteral("enabled")).toBool(outCheckpoint->enabled);
    outCheckpoint->triggerOnForward = json.value(QStringLiteral("triggerOnForward")).toBool(outCheckpoint->triggerOnForward);
    outCheckpoint->triggerOnBackward = json.value(QStringLiteral("triggerOnBackward")).toBool(outCheckpoint->triggerOnBackward);
    return true;
}

QJsonObject RowWorkJson::paramsToJson(const RowWorkParams &params)
{
    return QJsonObject{
        {QStringLiteral("baseLinearSpeed"), params.baseLinearSpeed},
        {QStringLiteral("maxLinearSpeed"), params.maxLinearSpeed},
        {QStringLiteral("endpointArrivalDistance"), params.endpointArrivalDistance},
        {QStringLiteral("checkpointArrivalTolerance"), params.checkpointArrivalTolerance},
        {QStringLiteral("turnAngularSpeed"), params.turnAngularSpeed},
        {QStringLiteral("loopEnabled"), params.loopEnabled}
    };
}

bool RowWorkJson::paramsFromJson(const QJsonObject &json, RowWorkParams *outParams)
{
    if (!outParams || json.isEmpty()) {
        return false;
    }

    const auto readDouble = [&json](const QString &key, double fallback) {
        const double value = json.value(key).toDouble(fallback);
        return isFiniteNumber(value) ? value : fallback;
    };

    outParams->baseLinearSpeed = readDouble(QStringLiteral("baseLinearSpeed"), outParams->baseLinearSpeed);
    outParams->maxLinearSpeed = readDouble(QStringLiteral("maxLinearSpeed"), outParams->maxLinearSpeed);
    outParams->endpointArrivalDistance = readDouble(QStringLiteral("endpointArrivalDistance"), outParams->endpointArrivalDistance);
    outParams->checkpointArrivalTolerance = readDouble(QStringLiteral("checkpointArrivalTolerance"), outParams->checkpointArrivalTolerance);
    outParams->turnAngularSpeed = readDouble(QStringLiteral("turnAngularSpeed"), outParams->turnAngularSpeed);
    outParams->loopEnabled = json.value(QStringLiteral("loopEnabled")).toBool(outParams->loopEnabled);
    return true;
}

QJsonObject RowWorkJson::planToJson(const RowWorkPlan &plan)
{
    QJsonObject root{
        {QStringLiteral("schemaVersion"), 2},
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

    const int schema = json.value("schemaVersion").toInt(1);
    if (schema < 1 || schema > 2) return false;
    RowWorkPlan plan;
    if (schema == 2) plan.frameBinding = MapFrameBinding::fromJson(json["frameBinding"].toObject());
    else { plan.params.endpointArrivalDistance = .25; plan.params.checkpointArrivalTolerance = .15; }
    plan.planId = json.value(QStringLiteral("planId")).toString(plan.planId).trimmed();
    plan.version = json.value(QStringLiteral("version")).toInt(plan.version);
    plan.frameId = json.value(QStringLiteral("frameId")).toString(plan.frameId).trimmed();
    if (plan.frameId.isEmpty()) {
        plan.frameId = QStringLiteral("map");
    }

    const QJsonObject startObj = json.value(QStringLiteral("startPoint")).toObject();
    if (!startObj.isEmpty()) {
        plan.hasStartPose = poseFromJson(startObj, &plan.startPose);
    }

    const QJsonObject endObj = json.value(QStringLiteral("endPoint")).toObject();
    if (!endObj.isEmpty()) {
        plan.hasEndPose = poseFromJson(endObj, &plan.endPose);
    }

    const QJsonObject paramsObj = json.value(QStringLiteral("params")).toObject();
    if (!paramsObj.isEmpty()) {
        paramsFromJson(paramsObj, &plan.params);
    }

    QList<RowCheckpoint> checkpoints;
    const QJsonArray checkpointsArray = json.value(QStringLiteral("checkpoints")).toArray();
    checkpoints.reserve(checkpointsArray.size());
    for (const QJsonValue &value : checkpointsArray) {
        if (!value.isObject()) {
            continue;
        }
        RowCheckpoint checkpoint;
        if (!checkpointFromJson(value.toObject(), &checkpoint)) {
            continue;
        }
        checkpoints.append(checkpoint);
    }
    plan.checkpoints = checkpoints;
    *outPlan = plan;
    return true;
}

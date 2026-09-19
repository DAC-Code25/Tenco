#include "trackingtypes.h"
#include <QJsonArray>
#include <QUuid>
#include <cmath>
#include <limits>

namespace {
bool finiteNumber(const QJsonObject &o, const QString &key, double *output) {
    const auto v = o.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()))
        return false;
    *output = v.toDouble();
    return true;
}
bool reject(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
} // namespace
bool TrackingContext::isComplete() const {
    return !mapId.isEmpty() && mapRevision > 0 && !originRevision.isEmpty() &&
           !frameTransformRevision.isEmpty() && !calibrationId.isEmpty() &&
           !controllerProfileRevision.isEmpty();
}
QJsonObject TrackingContext::toJson() const {
    return {{"mapId", mapId},
            {"mapRevision", mapRevision},
            {"originRevision", originRevision},
            {"frameTransformRevision", frameTransformRevision},
            {"calibrationId", calibrationId},
            {"controllerProfileRevision", controllerProfileRevision},
            {"frameId", "map"}};
}
TrackingContext TrackingContext::fromJson(const QJsonObject &o) {
    TrackingContext c;
    c.mapId = o["mapId"].toString();
    c.mapRevision = o["mapRevision"].toInt(0);
    c.originRevision = o["originRevision"].toString();
    c.frameTransformRevision = o["frameTransformRevision"].toString();
    c.calibrationId = o["calibrationId"].toString();
    c.controllerProfileRevision = o["controllerProfileRevision"].toString();
    if (o["frameId"].toString() != "map")
        c.originRevision.clear();
    return c;
}
bool TrackingContext::sameCoordinates(const TrackingContext &o) const {
    return originRevision == o.originRevision && calibrationId == o.calibrationId &&
           frameTransformRevision == o.frameTransformRevision;
}
bool MapFrameBinding::isUsable() const {
    return confirmed && context.isComplete() && std::isfinite(enuTranslation.x()) &&
           std::isfinite(enuTranslation.y()) && std::isfinite(enuYawOffsetRad);
}
QJsonObject MapFrameBinding::toJson() const {
    return {{"schemaVersion", 1},
            {"context", context.toJson()},
            {"confirmed", confirmed},
            {"enuTranslation", QJsonArray{enuTranslation.x(), enuTranslation.y()}},
            {"enuYawOffsetRad", enuYawOffsetRad}};
}
MapFrameBinding MapFrameBinding::fromJson(const QJsonObject &o) {
    MapFrameBinding b;
    if (o["schemaVersion"].toInt() != 1)
        return b;
    b.context = TrackingContext::fromJson(o["context"].toObject());
    b.confirmed = o["confirmed"].toBool();
    const auto xy = o["enuTranslation"].toArray();
    if (xy.size() != 2 || !xy[0].isDouble() || !xy[1].isDouble() || !o["enuYawOffsetRad"].isDouble()) {
        b.confirmed = false;
        return b;
    }
    b.enuTranslation = {xy[0].toDouble(), xy[1].toDouble()};
    b.enuYawOffsetRad = o["enuYawOffsetRad"].toDouble();
    return b;
}
bool TrackingSnapshot::isExecuting() const {
    return QStringList{"Starting", "Aligning", "Tracking", "Braking",
                       "Turning",  "Waiting",  "Pausing",  "Aborting"}
        .contains(state);
}
bool TrackingSnapshot::isTerminal() const {
    return state == "Completed" || state == "Aborted" || state == "Fault";
}
bool TrackingJson::sequence(const QJsonValue &v, quint64 *out) {
    if (!out || !v.isString())
        return false;
    const auto s = v.toString();
    if (s.isEmpty() || s.size() > 20)
        return false;
    for (const auto ch : s)
        if (ch < u'0' || ch > u'9')
            return false;
    bool ok = false;
    const auto n = s.toULongLong(&ok);
    if (ok)
        *out = n;
    return ok;
}
QString TrackingJson::newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool TrackingJson::pose(const QJsonObject &o, ControlPoseSnapshot *out, QString *error) {
    if (!out || o["apiVersion"].toInt() != 1 || o["frameId"].toString() != "map" ||
        o["baseFrameId"].toString() != "base_link")
        return reject(error, QStringLiteral("位姿接口版本或坐标系不匹配"));
    ControlPoseSnapshot p;
    p.bootId = o["bootId"].toString();
    p.originRevision = o["originRevision"].toString();
    p.calibrationId = o["calibrationId"].toString();
    p.clockDomain = o["clockDomain"].toString();
    p.mode = o["localizationMode"].toString();
    double x = 0, y = 0;
    const auto xy = o["pose"].toObject(), twist = o["estimatedTwist"].toObject();
    if (p.bootId.isEmpty() || p.originRevision.isEmpty() || p.calibrationId.isEmpty() ||
        !sequence(o["seq"], &p.seq) || !finiteNumber(xy, "x", &x) || !finiteNumber(xy, "y", &y) ||
        !finiteNumber(xy, "yaw", &p.yaw) || !finiteNumber(twist, "v", &p.v) ||
        !finiteNumber(twist, "omega", &p.omega) || !finiteNumber(o, "stateAgeMs", &p.stateAgeMs) ||
        !finiteNumber(o, "positionObservationAgeMs", &p.positionAgeMs) ||
        !finiteNumber(o, "headingObservationAgeMs", &p.headingAgeMs) || p.stateAgeMs < 0 ||
        p.positionAgeMs < 0 || p.headingAgeMs < 0)
        return reject(error, QStringLiteral("位姿字段无效"));
    p.position = {x, y};
    // Observation quality is diagnostic. The publisher decides whether its
    // continuous estimate is usable; downstream clients must not reapply FIX gates.
    p.validForControl = o["validForControl"].toBool();
    for (const auto &reason : o["reasonCodes"].toArray())
        p.reasons.append(reason.toString());
    *out = p;
    return true;
}
bool TrackingJson::status(const QJsonObject &o, TrackingSnapshot *out, QString *error) {
    if (!out || o["apiVersion"].toInt() != 1)
        return reject(error, QStringLiteral("任务接口版本不匹配"));
    TrackingSnapshot s;
    s.bootId = o["bootId"].toString();
    s.state = o["state"].toString();
    s.taskId = o["taskId"].toString();
    s.executionId = o["executionId"].toString();
    s.stepId = o["stepId"].toString();
    if (s.bootId.isEmpty() || !sequence(o["seq"], &s.seq) || !sequence(o["stateVersion"], &s.stateVersion))
        return reject(error, QStringLiteral("任务序号无效"));
    if (o.contains("lastEventSeq") && !sequence(o["lastEventSeq"], &s.lastEventSeq))
        return reject(error, QStringLiteral("事件序号无效"));
    if (!QStringList{"Idle", "Ready", "Starting", "Aligning", "Tracking", "Braking", "Turning", "Waiting",
                     "Pausing", "Paused", "Aborting", "Completed", "Aborted", "Fault"}
             .contains(s.state))
        return reject(error, QStringLiteral("未知任务状态"));
    s.pauseReason = o["pauseReason"].toString();
    s.faultCode = o["faultCode"].toString();
    s.result = o["result"].toString();
    s.waitingEventId = o["waitingEventId"].toString();
    s.taskRevision = o["taskRevision"].toInt();
    s.stepIndex = o["stepIndex"].toInt();
    s.loopIndex = o["loopIndex"].toInt();
    for (auto entry : {std::pair{"stepProgressMeters", &s.progressMeters},
                       {"stepLengthMeters", &s.lengthMeters},
                       {"lateralErrorMeters", &s.lateralError},
                       {"headingErrorRad", &s.headingErrorRad},
                       {"remainingAngleRad", &s.remainingAngleRad},
                       {"remainingWaitMs", &s.remainingWaitMs}})
        if (!finiteNumber(o, QString::fromLatin1(entry.first), entry.second))
            return reject(error, QStringLiteral("任务数值字段无效"));
    const auto measured = o["measuredTwist"].toObject();
    if (!finiteNumber(measured, "v", &s.measuredV) || !finiteNumber(measured, "omega", &s.measuredOmega))
        return reject(error, QStringLiteral("底盘实测速度无效"));
    *out = s;
    return true;
}

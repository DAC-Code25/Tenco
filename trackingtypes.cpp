#include "trackingtypes.h"
#include <QObject>
#include <QJsonArray>
#include <QHash>
#include <QRegularExpression>
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
QString TrackingJson::reasonText(const QString &reason) {
    static const QHash<QString, QString> messages{
        {"pose_stream_disconnected", QObject::tr("定位连接已断开，正在重连")},
        {"pose_stream_timeout", QObject::tr("定位更新超时，正在重连")},
        {"pose_stream_content_type", QObject::tr("定位数据格式异常，请检查定位服务")},
        {"pose_stream_invalid_json", QObject::tr("定位数据格式异常，请检查定位服务")},
        {"pose_stream_line_too_large", QObject::tr("定位数据异常，请检查定位服务")},
        {"pose_stream_monotonic_time_invalid", QObject::tr("定位时间异常，请检查工控机")},
        {"pose_stream_send_time_invalid", QObject::tr("定位时间异常，请检查工控机")},
        {"pose_stream_time_basis_changed", QObject::tr("定位时间依据已改变，请重新连接")},
        {"pose_stream_clock_rewound", QObject::tr("定位时间异常，请检查工控机")},
        {"request_timeout", QObject::tr("工控机响应超时，请稍后重试")},
        {"connection_failed", QObject::tr("无法连接工控机，请检查网络")},
        {"invalid_json_response", QObject::tr("工控机返回的数据异常，请检查服务")},
        {"response_too_large", QObject::tr("工控机返回的数据过大，请检查服务")},
        {"operator_session_in_use", QObject::tr("其他操作正在使用控制会话，请稍后重试")},
        {"invalid_session_response", QObject::tr("控制会话异常，请重新连接")},
        {"invalid_heartbeat_response", QObject::tr("控制会话更新失败，请重新连接")},
        {"invalid_task_readback", QObject::tr("任务读取失败，请重新核对")},
        {"execution_history_unavailable", QObject::tr("任务记录暂不可用，正在重新读取")},
        {"execution_history_recovery_exhausted", QObject::tr("任务记录读取失败，请检查工控机连接")},
        {"capture_timeout", QObject::tr("位置采样超时，请停稳后重新采样")},
        {"capture_not_stationary_or_invalid", QObject::tr("车辆尚未停稳或定位、实测反馈不可用，请停稳后重新采样")},
        {"capture_scatter_exceeded", QObject::tr("定位样本变化过大，未记录点位，请等待稳定后重新采样")},
        {"insufficient_independent_samples", QObject::tr("有效定位样本不足，未记录点位，请稍后重新采样")},
        {"capture_source_timeout", QObject::tr("采样期间定位更新中断，未记录点位，请检查定位连接")},
        {"capture_version_or_boot_mismatch", QObject::tr("定位服务或坐标已改变，请重新采样")},
        {"invalid_capture_response", QObject::tr("工控机采样响应异常，未记录点位，请重新采样")},
        {"control_cycle_late", QObject::tr("控制调度延迟，停稳后自动继续")},
        {"control_deadline", QObject::tr("控制响应超时，请检查车辆状态")},
        {"pose_sample_invalid", QObject::tr("定位数据异常，等待恢复")},
        {"pose_sample_jump", QObject::tr("定位出现跳点，等待恢复")},
        {"pose_sample_unstable", QObject::tr("定位尚未稳定恢复")},
        {"path_alignment_braking", QObject::tr("路径方向偏差，正在停稳后校向")},
        {"path_realigning", QObject::tr("已停稳，正在对齐路径方向")},
        {"source_observation_time_unavailable", QObject::tr("源观测时间未知，按实时 HTTP 接收更新")},
        {"pose_stale", QObject::tr("定位更新延迟，等待恢复")},
        {"pose_hard_timeout", QObject::tr("定位更新中断，请检查定位服务")},
        {"pose_invalid", QObject::tr("定位尚未就绪，请检查定位服务")},
        {"localization_invalid", QObject::tr("定位尚未就绪，请检查定位服务")},
        {"chassis_feedback_lost", QObject::tr("底盘反馈中断，请检查连接")},
        {"chassis_unavailable", QObject::tr("底盘未连接，请检查底盘服务")},
        {"chassis_not_stopped", QObject::tr("尚未确认车辆停稳，请等待")},
        {"measured_twist_stale", QObject::tr("实测速度反馈延迟，请检查底盘")},
        {"fusion_restarted", QObject::tr("定位服务已重启，请核对后重新操作")},
        {"device_restarted", QObject::tr("设备连接已重启，请核对后重新操作")},
        {"context_mismatch", QObject::tr("地图与工控机配置不匹配，请重新核对")},
        {"outside_capture_range", QObject::tr("车辆不在任务允许的接入范围，请调整位置后重试")},
        {"unsupported_step_type", QObject::tr("工控机导航版本不支持此任务，请更新后重新下发")},
        {"goal_position_outside_tolerance", QObject::tr("未准确到达目标点，请检查现场")},
        {"normal_handover_unstable", QObject::tr("停车后的定位尚未稳定，请核对后手动继续")},
        {"progress_timeout", QObject::tr("车辆长时间未前进，请检查现场")},
        {"terminal_stop_timeout", QObject::tr("停车确认超时，请检查车辆是否停稳")},
        {"turn_timeout", QObject::tr("转向超时，请检查现场")},
        {"turn_recovery_timeout", QObject::tr("转向回点超时，请检查现场")},
        {"turn_space_exceeded", QObject::tr("转向偏移过大，请检查现场")},
        {"endpoint_motion_unconfirmed", QObject::tr("位置偏移缺少运动证据，已停车，请核对定位与车辆位置")},
        {"endpoint_space_unavailable", QObject::tr("端点返回空间不足，已停车，请检查现场")},
        {"endpoint_recovery_timeout", QObject::tr("端点回位超时，已停车，请检查现场")},
        {"endpoint_position_recovery", QObject::tr("正在低速返回原端点")},
        {"endpoint_position_recovered", QObject::tr("已回到原端点并确认停稳")},
        {"endpoint_recovery_braking", QObject::tr("端点偏离已核对，正在停稳后回位")},
        {"lateral_safety_limit", QObject::tr("车辆偏离路径过远，请检查现场")},
        {"command_expired", QObject::tr("控制指令已过期，请检查车辆状态")},
        {"emergency_stop", QObject::tr("已触发急停，请确认现场安全后复位")},
        {"operator_abort", QObject::tr("任务已手动结束")},
        {"state_conflict", QObject::tr("任务状态已改变，请刷新后重试")},
        {"external_action_timeout", QObject::tr("检查点操作等待超时，请核对执行结果")},
        {"external_action_failed", QObject::tr("检查点操作失败，请核对执行结果")}
    };
    const auto text = reason.trimmed();
    if (text.isEmpty()) return {};
    if (const auto it = messages.constFind(text); it != messages.cend()) return it.value();
    const auto httpCode = text.startsWith("pose_stream_http_") ? text.mid(17) :
                          text.startsWith("http_") ? text.mid(5) : QString();
    if (!httpCode.isEmpty()) {
        if (httpCode == "401" || httpCode == "403") return QObject::tr("工控机连接验证失败，请检查连接设置");
        if (httpCode == "404") return QObject::tr("工控机服务接口不可用，请检查服务版本");
        return QObject::tr("工控机服务暂不可用，请检查服务状态");
    }
    // Internal signals and status snapshots retain the original reason for diagnostics.
    // Unknown machine codes must not become labels intended for the operator.
    static const QRegularExpression machineCode(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*(?::.*)?$"));
    if (machineCode.match(text).hasMatch()) return QObject::tr("操作未完成，请查看维护诊断");
    return reason;
}
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
    p.freshnessBasis = o.contains("freshnessBasis") ? o["freshnessBasis"].toString() : QStringLiteral("source_time");
    if (p.freshnessBasis != "source_time" && p.freshnessBasis != "http_receipt")
        return reject(error, QStringLiteral("位姿新鲜度依据无效"));
    if (o.contains("sendMonotonicSeconds") || p.freshnessBasis == "http_receipt") {
        double sent = 0;
        if (!finiteNumber(o, "sendMonotonicSeconds", &sent) || sent < 0)
            return reject(error, QStringLiteral("pose_stream_monotonic_time_invalid"));
        p.sendMonotonicSeconds = sent;
    }
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
    s.hasEventSequence = o.contains("lastEventSeq");
    if (s.hasEventSequence && !sequence(o["lastEventSeq"], &s.lastEventSeq))
        return reject(error, QStringLiteral("事件序号无效"));
    if (!QStringList{"Idle", "Ready", "Starting", "Aligning", "Tracking", "Braking", "Turning", "Waiting",
                     "Pausing", "Paused", "Aborting", "Completed", "Aborted", "Fault"}
             .contains(s.state))
        return reject(error, QStringLiteral("未知任务状态"));
    s.pauseReason = o["pauseReason"].toString();
    if (o.contains("autoResumePending") && !o["autoResumePending"].isBool())
        return reject(error, QStringLiteral("自动恢复状态无效"));
    s.autoResumePending = o["autoResumePending"].toBool();
    if (o.contains("returningToStart") && !o["returningToStart"].isBool())
        return reject(error, QStringLiteral("返回起点状态无效"));
    s.returningToStart = o["returningToStart"].toBool();
    if (o.contains("turnRecovering") && !o["turnRecovering"].isBool())
        return reject(error, QStringLiteral("转向回点状态无效"));
    s.turnRecovering = o["turnRecovering"].toBool();
    if (o.contains("endpointRecovering") && !o["endpointRecovering"].isBool())
        return reject(error, QStringLiteral("端点回位状态无效"));
    s.endpointRecovering = o["endpointRecovering"].toBool();
    if (o.contains("turnOffsetMeters") &&
        (!finiteNumber(o, "turnOffsetMeters", &s.turnOffsetMeters) || s.turnOffsetMeters < 0))
        return reject(error, QStringLiteral("转向偏移距离无效"));
    if (o.contains("recoveryStableMs") &&
        (!finiteNumber(o, "recoveryStableMs", &s.recoveryStableMs) || s.recoveryStableMs < 0))
        return reject(error, QStringLiteral("恢复稳定时间无效"));
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
    for (const auto &key : {QStringLiteral("chassisConnected"), QStringLiteral("measuredStopped")})
        if (o.contains(key) && !o[key].isBool())
            return reject(error, QStringLiteral("底盘实测状态无效"));
    if (measured.contains("valid") && !measured["valid"].isBool())
        return reject(error, QStringLiteral("底盘实测反馈状态无效"));
    s.chassisConnected = o["chassisConnected"].toBool();
    s.measuredValid = measured["valid"].toBool();
    s.measuredStopped = o["measuredStopped"].toBool();
    *out = s;
    return true;
}

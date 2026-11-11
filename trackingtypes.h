#pragma once
#include <QJsonObject>
#include <QMetaType>
#include <QPointF>
#include <QStringList>
#include <optional>

struct TrackingContext {
    QString mapId, originRevision, frameTransformRevision, calibrationId, controllerProfileRevision;
    int mapRevision = 1;
    bool isComplete() const;
    QJsonObject toJson() const;
    static TrackingContext fromJson(const QJsonObject &object);
    bool sameCoordinates(const TrackingContext &other) const;
};

struct MapFrameBinding {
    TrackingContext context;
    // Existing map coordinates use South/East axes. This verified rigid transform
    // maps their standard East/North representation to the active fusion ENU frame.
    QPointF enuTranslation;
    double enuYawOffsetRad = 0;
    bool confirmed = false;
    bool isUsable() const;
    QJsonObject toJson() const;
    static MapFrameBinding fromJson(const QJsonObject &object);
};

struct ControlPoseSnapshot {
    QString bootId, originRevision, calibrationId, clockDomain, mode;
    QString freshnessBasis = QStringLiteral("source_time");
    std::optional<double> sendMonotonicSeconds;
    quint64 seq = 0;
    QPointF position;
    double yaw = 0, v = 0, omega = 0;
    double stateAgeMs = 1e12, positionAgeMs = 1e12, headingAgeMs = 1e12;
    bool validForControl = false;
    bool monitorDelayed = false;
    QStringList reasons;
};

struct TrackingSnapshot {
    QString bootId, state, taskId, executionId, stepId;
    QString pauseReason, faultCode, result, waitingEventId;
    quint64 seq = 0, stateVersion = 0, lastEventSeq = 0;
    bool hasEventSequence = false;
    bool autoResumePending = false;
    bool returningToStart = false;
    bool turnRecovering = false;
    bool endpointRecovering = false;
    double turnOffsetMeters = 0;
    double recoveryStableMs = 0;
    int taskRevision = 0, stepIndex = 0, loopIndex = 0;
    double progressMeters = 0, lengthMeters = 0, lateralError = 0, headingErrorRad = 0;
    double remainingAngleRad = 0, remainingWaitMs = 0, measuredV = 0, measuredOmega = 0;
    bool chassisConnected = false, measuredValid = false, measuredStopped = false;
    bool isExecuting() const;
    bool isTerminal() const;
};

namespace TrackingJson {
bool sequence(const QJsonValue &value, quint64 *output);
bool pose(const QJsonObject &object, ControlPoseSnapshot *output, QString *error = nullptr);
bool status(const QJsonObject &object, TrackingSnapshot *output, QString *error = nullptr);
QString newId();
QString reasonText(const QString &reason);
} // namespace TrackingJson
Q_DECLARE_METATYPE(ControlPoseSnapshot)
Q_DECLARE_METATYPE(TrackingSnapshot)
Q_DECLARE_METATYPE(MapFrameBinding)

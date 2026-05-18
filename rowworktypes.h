#ifndef ROWWORKTYPES_H
#define ROWWORKTYPES_H

#include <QList>
#include <QMetaType>
#include <QPointF>
#include <QString>

class QJsonObject;

struct RowWorkPose
{
    double x = 0.0;
    double y = 0.0;
    double yaw = 0.0;

    QPointF toPointF() const { return QPointF(x, y); }
};

struct RowCheckpoint
{
    QString name;
    double progress = 0.0;
    int dwellMs = 2000;
    bool enabled = true;
    bool triggerOnForward = true;
    bool triggerOnBackward = true;
};

struct RowWorkParams
{
    double baseLinearSpeed = 0.25;
    double maxLinearSpeed = 0.35;
    double endpointSlowdownDistance = 1.5;
    double endpointArrivalDistance = 0.25;
    double checkpointArrivalTolerance = 0.15;
    double turnAngularSpeed = 0.45;
    bool loopEnabled = true;
};

struct RowWorkPlan
{
    QString planId;
    int version = 1;
    QString frameId = QStringLiteral("map");
    bool hasStartPose = false;
    RowWorkPose startPose;
    bool hasEndPose = false;
    RowWorkPose endPose;
    QList<RowCheckpoint> checkpoints;
    RowWorkParams params;

    bool isValid() const;
    double lineLength() const;
};

struct RowWorkStatus
{
    bool success = false;
    QString message;
    QString mode;
    QString state;
    QString controlOwner;
    QString planId;
    int planVersion = 0;
    bool poseFresh = false;
    int poseAgeMs = 0;
    double lateralError = 0.0;
    double headingErrorDeg = 0.0;
    double progress = 0.0;
    double lineLength = 0.0;
    QString currentDirection;
    int currentCheckpointIndex = -1;
    int pauseRemainingMs = 0;
    QString faultCode;
    QString faultMessage;
    QString lastEvent;

    bool isActive() const;
    bool isFaulted() const;
};

namespace RowWorkGeometry
{
bool hasUsableLine(const RowWorkPlan &plan);
double lineLength(const RowWorkPlan &plan);
double clampProgress(const RowWorkPlan &plan, double progress);
double projectPointToProgress(const RowWorkPlan &plan,
                              const QPointF &point,
                              double *outSignedLateralError = nullptr,
                              QPointF *outProjectedPoint = nullptr);
QPointF pointAtProgress(const RowWorkPlan &plan, double progress);
}

namespace RowWorkJson
{
QString generatePlanId();

QJsonObject poseToJson(const RowWorkPose &pose);
bool poseFromJson(const QJsonObject &json, RowWorkPose *outPose);

QJsonObject checkpointToJson(const RowCheckpoint &checkpoint);
bool checkpointFromJson(const QJsonObject &json, RowCheckpoint *outCheckpoint);

QJsonObject paramsToJson(const RowWorkParams &params);
bool paramsFromJson(const QJsonObject &json, RowWorkParams *outParams);

QJsonObject planToJson(const RowWorkPlan &plan);
bool planFromJson(const QJsonObject &json, RowWorkPlan *outPlan);

bool statusFromJson(const QJsonObject &json, RowWorkStatus *outStatus);
bool capturePoseResponseFromJson(const QJsonObject &json,
                                 RowWorkPose *outPose,
                                 int *outSampleDurationMs = nullptr,
                                 int *outSampleCount = nullptr,
                                 QString *outMessage = nullptr);
}

Q_DECLARE_METATYPE(RowWorkPose)
Q_DECLARE_METATYPE(RowCheckpoint)
Q_DECLARE_METATYPE(RowWorkPlan)
Q_DECLARE_METATYPE(RowWorkStatus)

#endif // ROWWORKTYPES_H

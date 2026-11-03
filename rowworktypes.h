#ifndef ROWWORKTYPES_H
#define ROWWORKTYPES_H

#include <QList>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <optional>
#include "trackingtypes.h"
#include "vehicleparameters.h"

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
    bool capturePhoto = false;
    int actionTimeoutMs = 30000;
    // Unfinished teaching sample, before endpoint B has been chosen.
    std::optional<QPointF> pendingMapPosition;
    // Confirmed teaching sample. Progress is its A/B station, not a replacement for this XY.
    std::optional<QPointF> recordedMapPosition;
};

struct RowWorkParams
{
    bool loopEnabled = true;
};

struct RowWorkPlan
{
    QString planId;
    int version = 1;
    QString frameId = QStringLiteral("map");
    MapFrameBinding frameBinding;
    bool hasStartPose = false;
    RowWorkPose startPose;
    bool hasEndPose = false;
    RowWorkPose endPose;
    QList<RowCheckpoint> checkpoints;
    RowWorkParams params;

    bool isValid() const;
    double lineLength() const;
};

namespace RowWorkGeometry
{
bool hasUsableStart(const RowWorkPlan &plan);
bool hasUsableLine(const RowWorkPlan &plan);
bool hasUsablePath(const RowWorkPlan &plan);
double lineLength(const RowWorkPlan &plan);
double clampProgress(const RowWorkPlan &plan, double progress);
double projectPointToProgress(const RowWorkPlan &plan,
                              const QPointF &point,
                              double *outSignedLateralError = nullptr,
                              QPointF *outProjectedPoint = nullptr);
QPointF pointAtProgress(const RowWorkPlan &plan, double progress);
QList<QPointF> pathBetweenProgress(const RowWorkPlan &plan, double from, double to);
bool setCheckpointPosition(const RowWorkPlan &plan, const QPointF &point, RowCheckpoint *checkpoint);
bool finalizeCheckpoints(RowWorkPlan *plan);
QPointF checkpointPosition(const RowWorkPlan &plan, const RowCheckpoint &checkpoint);
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


}

Q_DECLARE_METATYPE(RowWorkPose)
Q_DECLARE_METATYPE(RowCheckpoint)
Q_DECLARE_METATYPE(RowWorkPlan)

#endif // ROWWORKTYPES_H

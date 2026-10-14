#ifndef ROWMISSIONTYPES_H
#define ROWMISSIONTYPES_H

#include "rowworktypes.h"

#include <QList>
#include <QMetaType>
#include <QString>

class QJsonObject;

enum class RowMissionStepType
{
    RowLeg,
    Transfer,
    Turn,
    Wait
};

struct RowMissionStep
{
    QString stepId;
    QString name;
    QString note;
    RowMissionStepType type = RowMissionStepType::RowLeg;
    bool enabled = true;
    RowWorkPlan primitivePlan;
    double targetYawRad = 0.0;
    int dwellMs = 0;

    bool isValid() const;
};

struct RowMissionPlan
{
    QString missionId;
    int version = 1;
    QString frameId = QStringLiteral("map");
    MapFrameBinding frameBinding;
    QString name;
    bool loopEnabled = false;
    QList<RowMissionStep> steps;

    bool isValid() const;
    int enabledStepCount() const;
};

namespace RowMissionJson
{
QString generateMissionId();
QString generateStepId(int index = -1);

QJsonObject stepToJson(const RowMissionStep &step);
bool stepFromJson(const QJsonObject &json, RowMissionStep *outStep);

QJsonObject planToJson(const RowMissionPlan &plan);
bool planFromJson(const QJsonObject &json, RowMissionPlan *outPlan);
}

Q_DECLARE_METATYPE(RowMissionStepType)
Q_DECLARE_METATYPE(RowMissionStep)
Q_DECLARE_METATYPE(RowMissionPlan)

#endif // ROWMISSIONTYPES_H

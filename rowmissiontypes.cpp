#include "rowmissiontypes.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <cmath>
#include <algorithm>
#include <limits>

namespace
{
bool isFiniteNumber(double value)
{
    return std::isfinite(value);
}

QString normalized(const QString &value)
{
    return value.trimmed();
}

QString stepTypeToString(RowMissionStepType type)
{
    switch (type) {
    case RowMissionStepType::RowLeg:
        return QStringLiteral("row_leg");
    case RowMissionStepType::Transfer:
        return QStringLiteral("transfer");
    case RowMissionStepType::Turn:
        return QStringLiteral("turn");
    case RowMissionStepType::Wait:
        return QStringLiteral("wait");
    }
    return QStringLiteral("row_leg");
}

bool stepTypeFromString(const QString &value, RowMissionStepType *outType)
{
    if (!outType) {
        return false;
    }

    const QString normalizedValue = value.trimmed().toLower();
    if (normalizedValue == QStringLiteral("row_leg") || normalizedValue == QStringLiteral("rowleg")) {
        *outType = RowMissionStepType::RowLeg;
        return true;
    }
    if (normalizedValue == QStringLiteral("transfer")) {
        *outType = RowMissionStepType::Transfer;
        return true;
    }
    if (normalizedValue == QStringLiteral("turn")) {
        *outType = RowMissionStepType::Turn;
        return true;
    }
    if (normalizedValue == QStringLiteral("wait") || normalizedValue == QStringLiteral("pause")) {
        *outType = RowMissionStepType::Wait;
        return true;
    }
    return false;
}

bool isStepSemanticallyValid(const RowMissionStep &step)
{
    switch (step.type) {
    case RowMissionStepType::RowLeg:
    case RowMissionStepType::Transfer:
        return step.primitivePlan.isValid();
    case RowMissionStepType::Turn:
        return isFiniteNumber(step.targetYawRad);
    case RowMissionStepType::Wait:
        return step.dwellMs >= 0;
    }
    return false;
}
} // namespace

bool RowMissionStep::isValid() const
{
    return enabled && isStepSemanticallyValid(*this);
}

bool RowMissionPlan::isValid() const
{
    if (steps.isEmpty()) {
        return false;
    }

    for (const RowMissionStep &step : steps) {
        if (step.enabled && !isStepSemanticallyValid(step)) {
            return false;
        }
    }
    return enabledStepCount() > 0;
}

int RowMissionPlan::enabledStepCount() const
{
    int count = 0;
    for (const RowMissionStep &step : steps) {
        if (step.enabled && isStepSemanticallyValid(step)) {
            ++count;
        }
    }
    return count;
}

QString RowMissionJson::generateMissionId()
{
    return QStringLiteral("mission-%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")));
}

QString RowMissionJson::generateStepId(int index)
{
    if (index >= 0) {
        return QStringLiteral("step-%1").arg(index + 1);
    }
    return QStringLiteral("step-%1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmsszzz")));
}

QJsonObject RowMissionJson::stepToJson(const RowMissionStep &step)
{
    QJsonObject root{
        {QStringLiteral("stepId"), normalized(step.stepId)},
        {QStringLiteral("name"), normalized(step.name)},
        {QStringLiteral("type"), stepTypeToString(step.type)},
        {QStringLiteral("enabled"), step.enabled},
        {QStringLiteral("note"), normalized(step.note)},
        {QStringLiteral("targetYawRad"), step.targetYawRad},
        {QStringLiteral("dwellMs"), step.dwellMs}
    };

    if (step.primitivePlan.isValid() || !step.primitivePlan.checkpoints.isEmpty()) {
        root.insert(QStringLiteral("primitivePlan"), RowWorkJson::planToJson(step.primitivePlan));
    }
    return root;
}

bool RowMissionJson::stepFromJson(const QJsonObject &json, RowMissionStep *outStep)
{
    if (!outStep || json.isEmpty()) {
        return false;
    }

    RowMissionStep step = *outStep;
    step.stepId = normalized(json.value(QStringLiteral("stepId")).toString(step.stepId));
    step.name = normalized(json.value(QStringLiteral("name")).toString(step.name));
    step.note = normalized(json.value(QStringLiteral("note")).toString(step.note));
    step.enabled = json.value(QStringLiteral("enabled")).toBool(step.enabled);
    step.targetYawRad = json.value(QStringLiteral("targetYawRad")).toDouble(step.targetYawRad);
    step.dwellMs = json.value(QStringLiteral("dwellMs")).toInt(step.dwellMs);

    const QString typeString = json.value(QStringLiteral("type")).toString();
    if (!typeString.isEmpty()) {
        if (!stepTypeFromString(typeString, &step.type)) {
            return false;
        }
    }

    if (const QJsonObject primitivePlanObj = json.value(QStringLiteral("primitivePlan")).toObject(); !primitivePlanObj.isEmpty()) {
        RowWorkPlan primitivePlan = step.primitivePlan;
        if (!RowWorkJson::planFromJson(primitivePlanObj, &primitivePlan)) {
            return false;
        }
        step.primitivePlan = primitivePlan;
    }

    if (step.stepId.isEmpty()) {
        step.stepId = generateStepId();
    }
    if (step.name.isEmpty()) {
        step.name = step.stepId;
    }
    if (!step.enabled) {
        step.enabled = false;
    }
    if (step.type == RowMissionStepType::RowLeg || step.type == RowMissionStepType::Transfer) {
        if (!step.primitivePlan.isValid()) {
            return false;
        }
    } else if (step.type == RowMissionStepType::Turn) {
        if (!isFiniteNumber(step.targetYawRad)) {
            return false;
        }
    } else if (step.type == RowMissionStepType::Wait) {
        step.dwellMs = std::max(0, step.dwellMs);
    }

    *outStep = step;
    return true;
}

QJsonObject RowMissionJson::planToJson(const RowMissionPlan &plan)
{
    QJsonObject root{
        {QStringLiteral("missionId"), normalized(plan.missionId)},
        {QStringLiteral("version"), plan.version},
        {QStringLiteral("frameId"), normalized(plan.frameId).isEmpty() ? QStringLiteral("map") : normalized(plan.frameId)},
        {QStringLiteral("name"), normalized(plan.name)},
        {QStringLiteral("loopEnabled"), plan.loopEnabled}
    };

    QJsonArray stepsArray;
    for (const RowMissionStep &step : plan.steps) {
        stepsArray.append(stepToJson(step));
    }
    root.insert(QStringLiteral("steps"), stepsArray);
    return root;
}

bool RowMissionJson::planFromJson(const QJsonObject &json, RowMissionPlan *outPlan)
{
    if (!outPlan || json.isEmpty()) {
        return false;
    }

    RowMissionPlan plan = *outPlan;
    plan.missionId = normalized(json.value(QStringLiteral("missionId")).toString(plan.missionId));
    plan.version = json.value(QStringLiteral("version")).toInt(plan.version);
    plan.frameId = normalized(json.value(QStringLiteral("frameId")).toString(plan.frameId));
    if (plan.frameId.isEmpty()) {
        plan.frameId = QStringLiteral("map");
    }
    plan.name = normalized(json.value(QStringLiteral("name")).toString(plan.name));
    plan.loopEnabled = json.value(QStringLiteral("loopEnabled")).toBool(plan.loopEnabled);

    QList<RowMissionStep> steps;
    const QJsonArray stepsArray = json.value(QStringLiteral("steps")).toArray();
    steps.reserve(stepsArray.size());
    for (const QJsonValue &value : stepsArray) {
        if (!value.isObject()) {
            return false;
        }
        RowMissionStep step;
        if (!RowMissionJson::stepFromJson(value.toObject(), &step)) {
            return false;
        }
        steps.append(step);
    }
    plan.steps = steps;

    if (plan.missionId.isEmpty()) {
        plan.missionId = generateMissionId();
    }

    *outPlan = plan;
    return true;
}

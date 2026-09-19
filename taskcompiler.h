#pragma once
#include "mapframeadapter.h"
#include "rowmissiontypes.h"
#include <QJsonArray>
#include <optional>

struct TaskCompileOptions {
    double speedLimit = .25, goalToleranceMeters = .03, angularSpeedLimit = .35, angleToleranceRad = .05;
    QString safetyProfileId, rotationZoneId;
    bool repeatUntilStopped = false;
    int repeatCount = 1;
};
struct TaskRouteSection {
    QString id;
    QList<QPointF> points;
    std::optional<double> goalMapYaw;
};
struct CompiledTask {
    QJsonObject plan;
    QString error;
    bool ok() const { return error.isEmpty() && !plan.isEmpty(); }
};

class TaskCompiler {
  public:
    static CompiledTask route(const QList<TaskRouteSection> &sections, const MapFrameBinding &binding,
                              const TaskCompileOptions &options, const QString &name = {});
    static CompiledTask row(const RowWorkPlan &row, const MapFrameBinding &binding,
                            const TaskCompileOptions &options, bool returnTrip);
    static CompiledTask mission(const RowMissionPlan &mission, const MapFrameBinding &binding,
                                const TaskCompileOptions &options);
};

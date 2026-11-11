#pragma once
#include "mapframeadapter.h"
#include "rowworktypes.h"
#include <QJsonArray>
#include <limits>
#include <optional>

struct TaskCompileOptions {
    // No local navigation defaults. Production callers populate these from the
    // controller's applied configuration before compiling a new task.
    double speedLimit = std::numeric_limits<double>::quiet_NaN();
    double goalToleranceMeters = std::numeric_limits<double>::quiet_NaN();
    double angularSpeedLimit = std::numeric_limits<double>::quiet_NaN();
    double angleToleranceRad = std::numeric_limits<double>::quiet_NaN();
    int turnTimeoutMs = 0;
    QString safetyProfileId, rotationZoneId;
    bool vehicleFootprintRotation = false;
    // Derived from the applied control basis; never a map/task tuning value.
    bool shortPathEndpointCapture = false;
    bool repeatUntilStopped = false;
    int repeatCount = 1;
    void applyControllerDefaults(const QJsonObject &configuration);
};
struct TaskRouteSection {
    QString id;
    QList<QPointF> points;
    std::optional<double> goalMapYaw;
    // Per-point task actions; zero preserves continuous route traversal.
    int dwellMs = 0;
    // First section only. A repeating body dwells on its arrival back at A.
    int startDwellMs = 0;
    std::optional<double> startMapYaw;
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
                            const TaskCompileOptions &options, bool returnTrip,
                            bool returnToStart = false);
};

#include "taskcompiler.h"
#include "mapgeometry.h"
#include <QLineF>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool valid, const char *message) {
    if (!valid)
        throw std::invalid_argument(message);
}
double heading(const QPointF &a, const QPointF &b) { return std::atan2(b.y() - a.y(), b.x() - a.x()); }
class Compiler {
  public:
    MapFrameAdapter frame;
    TaskCompileOptions options;
    QJsonArray steps;
    std::optional<QPointF> lastPosition;
    std::optional<double> lastHeading;
    int counter = 0;
    Compiler(const MapFrameBinding &binding, const TaskCompileOptions &settings)
        : frame(binding), options(settings) {
        require(binding.isUsable(), "地图尚未确认当前原点、标定及坐标变换");
        for (double number : {options.speedLimit, options.goalToleranceMeters, options.angularSpeedLimit,
                              options.angleToleranceRad})
            require(std::isfinite(number) && number > 0, "任务速度或到达容差无效");
        require(!options.safetyProfileId.isEmpty(), "请先选择工控机已核实的作业通道");
        require(options.repeatCount > 0, "循环次数必须大于零");
    }
    QString nextId(const QString &prefix) { return prefix + "-" + QString::number(++counter); }
    void turn(double target, const QString &prefix) {
        require(!options.rotationZoneId.isEmpty(), "该路线需要原地转向，请先选择已核实的转向区域");
        require(std::isfinite(target), "目标航向无效");
        steps.append(QJsonObject{{"stepId", nextId(prefix)},
                                 {"type", "turn"},
                                 {"targetYaw", MapGeometry::normalizeAngle(target)},
                                 {"angularSpeedLimit", options.angularSpeedLimit},
                                 {"angleToleranceRad", options.angleToleranceRad},
                                 {"rotationZoneId", options.rotationZoneId},
                                 {"timeoutMs", 30000}});
        lastHeading = target;
    }
    void wait(int duration, const QString &checkpoint, const QString &prefix) {
        require(duration >= 0 && duration <= 3600000, "停留时长超出范围");
        steps.append(QJsonObject{{"stepId", nextId(prefix)},
                                 {"type", "wait"},
                                 {"checkpointId", checkpoint},
                                 {"completion", QJsonObject{{"type", "timer"}, {"durationMs", duration}}}});
    }
    void path(QList<QPointF> points, const QString &prefix, double speed = 0, double tolerance = 0) {
        require(points.size() >= 2, "路径至少需要两个不同点");
        QList<QPointF> clean;
        for (const auto &point : points) {
            require(std::isfinite(point.x()) && std::isfinite(point.y()), "路径包含非有限坐标");
            if (clean.isEmpty() || QLineF(clean.back(), point).length() > 1e-7)
                clean.append(point);
        }
        require(clean.size() >= 2, "路径长度为零");
        if (lastPosition)
            require(QLineF(*lastPosition, clean.front()).length() <= .02, "步骤之间不连续，请补充转场路径");
        const double startHeading = heading(clean[0], clean[1]);
        if (lastHeading && std::abs(MapGeometry::normalizeAngle(startHeading - *lastHeading)) > .05)
            turn(startHeading, prefix + "-align");
        // Split geometric corners into explicit stop/turn boundaries. Curved
        // map edges already arrive as sampled polylines and retain their shape.
        QList<QPointF> segment{clean.front()};
        for (qsizetype i = 1; i < clean.size(); ++i) {
            segment.append(clean[i]);
            const bool corner = i + 1 < clean.size() &&
                                std::abs(MapGeometry::normalizeAngle(heading(clean[i], clean[i + 1]) -
                                                                     heading(clean[i - 1], clean[i]))) > .35;
            if (corner || i + 1 == clean.size()) {
                QJsonArray array;
                for (const auto &point : segment)
                    array.append(QJsonArray{point.x(), point.y()});
                steps.append(QJsonObject{
                    {"stepId", nextId(prefix)},
                    {"type", "follow_path"},
                    {"points", array},
                    {"speedLimit", speed > 0 ? speed : options.speedLimit},
                    {"goalToleranceMeters", tolerance > 0 ? tolerance : options.goalToleranceMeters},
                    {"safetyProfileId", options.safetyProfileId},
                    {"endBehavior", "stop"}});
                lastPosition = segment.back();
                lastHeading = heading(segment[segment.size() - 2], segment.back());
                if (corner) {
                    turn(heading(clean[i], clean[i + 1]), prefix + "-corner");
                    segment = {clean[i]};
                }
            }
        }
    }
    QList<QPointF> mapped(const QList<QPointF> &points) {
        QList<QPointF> result;
        for (const auto &p : points)
            result.append(frame.toEnu(p));
        return result;
    }
    void leg(const RowWorkPlan &row, bool backwards, const QString &prefix, bool checkpoints) {
        require(row.isValid(), "单垄起点或终点无效");
        require(row.frameBinding.isUsable() &&
                    row.frameBinding.context.sameCoordinates(frame.binding().context) &&
                    row.frameBinding.context.mapId == frame.binding().context.mapId &&
                    row.frameBinding.context.controllerProfileRevision ==
                        frame.binding().context.controllerProfileRevision,
                "单垄文件尚未确认当前地图、原点和标定，请重新确认坐标或示教");
        require(std::isfinite(row.params.baseLinearSpeed) && row.params.baseLinearSpeed > 0 &&
                    std::isfinite(row.params.maxLinearSpeed) &&
                    row.params.maxLinearSpeed >= row.params.baseLinearSpeed &&
                    std::isfinite(row.params.turnAngularSpeed) && row.params.turnAngularSpeed > 0,
                "单垄速度无效");
        const double angularLimit = options.angularSpeedLimit;
        options.angularSpeedLimit = std::min(angularLimit, row.params.turnAngularSpeed);
        const double length = row.lineLength();
        QList<RowCheckpoint> selected;
        if (checkpoints)
            for (const auto &cp : row.checkpoints) {
                if (!cp.enabled || !(backwards ? cp.triggerOnBackward : cp.triggerOnForward))
                    continue;
                require(std::isfinite(cp.progress) && cp.progress >= 0 && cp.progress <= length,
                        "检查点超出作业线");
                selected.append(cp);
            }
        std::stable_sort(selected.begin(), selected.end(), [&](const auto &a, const auto &b) {
            return backwards ? a.progress > b.progress : a.progress < b.progress;
        });
        double progress = backwards ? length : 0;
        const double finish = backwards ? 0 : length;
        auto emitTo = [&](double target, bool endpoint) {
            if (std::abs(target - progress) > 1e-7) {
                const double tolerance =
                    endpoint ? row.params.endpointArrivalDistance : row.params.checkpointArrivalTolerance;
                require(std::isfinite(tolerance) && tolerance > 0, "单垄停车容差无效");
                path(mapped({RowWorkGeometry::pointAtProgress(row, progress),
                             RowWorkGeometry::pointAtProgress(row, target)}),
                     prefix, std::min(options.speedLimit, row.params.baseLinearSpeed), tolerance);
            }
            progress = target;
        };
        for (const auto &cp : selected) {
            emitTo(cp.progress, false);
            wait(cp.dwellMs, cp.name, prefix + "-checkpoint");
            if (cp.capturePhoto) {
                require(cp.actionTimeoutMs >= 1000 && cp.actionTimeoutMs <= 3600000, "拍摄超时时间无效");
                steps.append(QJsonObject{{"stepId", nextId(prefix + "-photo")},
                                         {"type", "wait"},
                                         {"checkpointId", cp.name},
                                         {"completion", QJsonObject{{"type", "external_ack"},
                                                                    {"eventType", "capture"},
                                                                    {"timeoutMs", cp.actionTimeoutMs}}}});
            }
        }
        emitTo(finish, true);
        options.angularSpeedLimit = angularLimit;
    }
    QJsonObject finish(const QString &name) {
        require(!steps.isEmpty() && steps.size() <= 1000, "任务为空或步骤数超过上限");
        if (options.repeatUntilStopped || options.repeatCount > 1) {
            std::optional<QPointF> first;
            for (const auto &value : steps) {
                const auto step = value.toObject();
                if (step["type"].toString() == "follow_path") {
                    const auto xy = step["points"].toArray()[0].toArray();
                    first = QPointF(xy[0].toDouble(), xy[1].toDouble());
                    break;
                }
            }
            if (first && lastPosition)
                require(QLineF(*first, *lastPosition).length() <= .02, "循环任务末端未返回起点");
        }
        return {{"schemaVersion", 1},
                {"taskId", TrackingJson::newId()},
                {"revision", 1},
                {"name", name},
                {"context", frame.binding().context.toJson()},
                {"steps", steps},
                {"repeat", QJsonObject{{"mode", options.repeatUntilStopped ? "until_stopped" : "count"},
                                       {"count", options.repeatCount}}}};
    }
};
template <class F> CompiledTask build(F f) {
    try {
        return {f(), {}};
    } catch (const std::exception &e) {
        return {{}, QString::fromUtf8(e.what())};
    }
}
} // namespace
CompiledTask TaskCompiler::route(const QList<TaskRouteSection> &sections, const MapFrameBinding &binding,
                                 const TaskCompileOptions &options, const QString &name) {
    return build([&] {
        Compiler compiler(binding, options);
        for (const auto &section : sections) {
            compiler.path(compiler.mapped(section.points), section.id);
            if (section.goalMapYaw) {
                const double target = compiler.frame.yawToEnu(*section.goalMapYaw);
                if (std::abs(MapGeometry::normalizeAngle(target - *compiler.lastHeading)) >
                    options.angleToleranceRad)
                    compiler.turn(target, section.id + "-heading");
            }
        }
        // Straight, equally constrained logical sections can flow continuously.
        for (qsizetype i = 0; i + 1 < compiler.steps.size(); ++i) {
            auto a = compiler.steps[i].toObject();
            const auto b = compiler.steps[i + 1].toObject();
            if (a["type"].toString() == "follow_path" && b["type"].toString() == "follow_path") {
                a["endBehavior"] = "blend";
                compiler.steps[i] = a;
            }
        }
        return compiler.finish(name);
    });
}
CompiledTask TaskCompiler::row(const RowWorkPlan &row, const MapFrameBinding &binding,
                               const TaskCompileOptions &options, bool returnTrip) {
    return build([&] {
        auto rowOptions = options;
        require(std::isfinite(row.params.turnAngularSpeed) && row.params.turnAngularSpeed > 0,
                "单垄转向速度无效");
        rowOptions.angularSpeedLimit = std::min(rowOptions.angularSpeedLimit, row.params.turnAngularSpeed);
        Compiler compiler(binding, rowOptions);
        compiler.leg(row, false, "out", true);
        if (returnTrip) {
            const auto start = compiler.frame.toEnu(row.startPose.toPointF()),
                       end = compiler.frame.toEnu(row.endPose.toPointF());
            compiler.turn(heading(end, start), "turn-end");
            compiler.leg(row, true, "return", true);
            compiler.turn(heading(start, end), "turn-start");
        }
        return compiler.finish(QStringLiteral("单垄作业"));
    });
}
CompiledTask TaskCompiler::mission(const RowMissionPlan &mission, const MapFrameBinding &binding,
                                   const TaskCompileOptions &options) {
    return build([&] {
        Compiler compiler(binding, options);
        require(mission.frameBinding.isUsable() &&
                    mission.frameBinding.context.sameCoordinates(binding.context) &&
                    mission.frameBinding.context.mapId == binding.context.mapId &&
                    mission.frameBinding.context.controllerProfileRevision ==
                        binding.context.controllerProfileRevision,
                "多垄文件尚未确认当前坐标版本");
        for (const auto &step : mission.steps) {
            if (!step.enabled)
                continue;
            switch (step.type) {
            case RowMissionStepType::RowLeg:
                compiler.leg(step.primitivePlan, false, step.stepId, true);
                break;
            case RowMissionStepType::Transfer:
                compiler.leg(step.primitivePlan, false, step.stepId, false);
                break;
            case RowMissionStepType::Turn:
                compiler.turn(compiler.frame.yawToEnu(step.targetYawRad), step.stepId);
                break;
            case RowMissionStepType::Wait:
                compiler.wait(step.dwellMs, {}, step.stepId);
                break;
            }
        }
        return compiler.finish(mission.name);
    });
}

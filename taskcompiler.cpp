#include "taskcompiler.h"
#include "mapgeometry.h"
#include <QLineF>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <stdexcept>

void TaskCompileOptions::applyControllerDefaults(const QJsonObject &configuration) {
    speedLimit = goalToleranceMeters = angularSpeedLimit = angleToleranceRad =
        std::numeric_limits<double>::quiet_NaN();
    safetyProfileId.clear();
    rotationZoneId.clear();
    vehicleFootprintRotation = false;
    shortPathEndpointCapture = false;
    turnTimeoutMs = 0;
    const auto defaults = configuration["navigation_defaults"].toObject();
    const auto policy = defaults["rotation_policy"].toString();
    if (defaults["task_parameters_version"] != 1 ||
        (policy != "vehicle_footprint" && policy != "verified_zones") ||
        defaults["default_safety_profile"].toString().isEmpty())
        return;
    for (const auto *key : {"task_speed_mps", "task_turn_speed_radps", "goal_tolerance_m", "angle_tolerance_rad"})
        if (!defaults[key].isDouble() || !std::isfinite(defaults[key].toDouble()) || defaults[key].toDouble() <= 0)
            return;
    const double linear = defaults["task_speed_mps"].toDouble();
    const double angular = defaults["task_turn_speed_radps"].toDouble();
    const double goal = defaults["goal_tolerance_m"].toDouble();
    const double angle = defaults["angle_tolerance_rad"].toDouble();
    const auto timeoutValue = defaults["task_turn_timeout_ms"];
    const double timeout = timeoutValue.toDouble();
    if (!timeoutValue.isDouble() || !std::isfinite(timeout) || timeout != std::floor(timeout) ||
        timeout < 1000 || timeout > 86400000) return;
    if (linear < VehicleParameters::minOperatingSpeedMps || linear > VehicleParameters::maxOperatingSpeedMps || angle > .5)
        return;
    speedLimit = linear;
    angularSpeedLimit = angular;
    goalToleranceMeters = goal;
    angleToleranceRad = angle;
    turnTimeoutMs = int(timeout);
    safetyProfileId = defaults["default_safety_profile"].toString();
    vehicleFootprintRotation = policy == "vehicle_footprint";
    shortPathEndpointCapture = vehicleFootprintRotation &&
        configuration["localizationPolicy"].toObject()["controlBasis"].toString() == "final_http";
    if (!vehicleFootprintRotation) rotationZoneId = defaults["default_rotation_zone"].toString();
}

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
            require(std::isfinite(number) && number > 0, "尚未读到有效的工控机导航参数，请在工控机维护中读取并应用参数后重试");
        require(options.turnTimeoutMs >= 1000 && options.turnTimeoutMs <= 86400000,
                "工控机未提供有效的任务转向期限，请更新导航服务并读取运行参数");
        require(options.speedLimit >= VehicleParameters::minOperatingSpeedMps &&
                    options.speedLimit <= VehicleParameters::maxOperatingSpeedMps,
                "任务行驶速度应在 0.10～1.50 m/s 内");
        require(!options.safetyProfileId.isEmpty(), "请先选择工控机已核实的作业通道");
        require(options.repeatCount > 0 && options.repeatCount <= 1000000, "循环次数必须在 1～1000000 次内");
    }
    QString nextId(const QString &prefix) { return prefix + "-" + QString::number(++counter); }
    void turn(double target, const QString &prefix) {
        require(options.vehicleFootprintRotation || !options.rotationZoneId.isEmpty(),
                "该路线需要原地转向，请先选择已核实的转向区域");
        require(std::isfinite(target), "目标航向无效");
        steps.append(QJsonObject{{"stepId", nextId(prefix)},
                                 {"type", "turn"},
                                 {"targetYaw", MapGeometry::normalizeAngle(target)},
                                 {"angularSpeedLimit", options.angularSpeedLimit},
                                 {"angleToleranceRad", options.angleToleranceRad},
                                 {"rotationZoneId", options.rotationZoneId},
                                 {"timeoutMs", options.turnTimeoutMs}});
        lastHeading = target;
    }
    void wait(int duration, const QString &checkpoint, const QString &prefix) {
        require(duration >= 0 && duration <= 3600000, "停留时长超出范围");
        steps.append(QJsonObject{{"stepId", nextId(prefix)},
                                 {"type", "wait"},
                                 {"checkpointId", checkpoint},
                                 {"completion", QJsonObject{{"type", "timer"}, {"durationMs", duration}}}});
    }
    void path(QList<QPointF> points, const QString &prefix, bool endpointCapture = false) {
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
        if (!endpointCapture && lastHeading &&
            std::abs(MapGeometry::normalizeAngle(startHeading - *lastHeading)) > .05)
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
                    {"speedLimit", options.speedLimit},
                    {"goalToleranceMeters", options.goalToleranceMeters},
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
    void leg(const RowWorkPlan &row, bool backwards, const QString &prefix) {
        require(RowWorkGeometry::hasUsableLine(row), "单垄起点或终点无效");
        for (const auto &cp : row.checkpoints)
            require(!cp.pendingMapPosition, "中间点尚未确认，请重新记录终点");
        require(RowWorkGeometry::hasUsablePath(row), "示教路径坐标或里程无效，请重新记录中间点");
        require(row.frameBinding.isUsable() &&
                    row.frameBinding.context.sameCoordinates(frame.binding().context) &&
                    row.frameBinding.context.mapId == frame.binding().context.mapId &&
                    row.frameBinding.context.controllerProfileRevision ==
                        frame.binding().context.controllerProfileRevision,
                "单垄文件尚未确认当前地图、原点和标定，请重新确认坐标或示教");
        const double length = row.lineLength();
        QList<RowCheckpoint> selected;
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
        bool checkpointAligned = false;
        auto emitTo = [&](double target) {
            if (std::abs(target - progress) > 1e-7) {
                path(mapped(RowWorkGeometry::pathBetweenProgress(row, progress, target)),
                     prefix);
                checkpointAligned = false;
            }
            progress = target;
        };
        for (const auto &cp : selected) {
            emitTo(cp.progress);
            if (!checkpointAligned) {
                // Face the local departure direction before dwelling, rather
                // than assuming the actual stopped vehicle matches lastHeading.
                // At the end of a leg retain its arrival direction; the return
                // turn remains after that leg's final checkpoint actions.
                auto local = mapped(RowWorkGeometry::pathBetweenProgress(row, cp.progress, finish));
                double target;
                if (local.size() >= 2) {
                    target = heading(local[0], local[1]);
                } else {
                    local = mapped(RowWorkGeometry::pathBetweenProgress(row,
                        backwards ? length : 0, cp.progress));
                    require(local.size() >= 2, "检查点路径朝向无效");
                    target = heading(local[local.size() - 2], local.back());
                }
                const auto previous = steps.isEmpty() ? QJsonObject{} : steps.last().toObject();
                // A leading wait is position-checked against the first path's
                // planned start by the IPC. Establish that anchor before any
                // leading turn, which otherwise anchors at the actual pose.
                if (steps.isEmpty())
                    wait(0, {}, prefix + "-start-position");
                if (previous["type"] != "turn" ||
                    std::abs(MapGeometry::normalizeAngle(previous["targetYaw"].toDouble() - target)) > 1e-10)
                    turn(target, prefix + "-checkpoint-heading");
                checkpointAligned = true;
            }
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
        emitTo(finish);
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
        QSet<qsizetype> capturePaths;
        bool firstSection = true;
        const auto shortPath = [&](const QJsonObject &step) {
            const auto points = step["points"].toArray();
            double length = 0;
            for (qsizetype i = 1; i < points.size(); ++i) {
                const auto a = points[i - 1].toArray(), b = points[i].toArray();
                length += std::hypot(b[0].toDouble() - a[0].toDouble(),
                                     b[1].toDouble() - a[1].toDouble());
            }
            return length > 1e-7 && length < options.goalToleranceMeters;
        };
        for (const auto &section : sections) {
            require(!section.goalMapYaw || std::isfinite(*section.goalMapYaw), "目标航向无效");
            require(section.dwellMs >= 0 && section.dwellMs <= 3600000, "停留时长超出范围");
            require(section.startDwellMs >= 0 && section.startDwellMs <= 3600000,
                    "首点停留时长超出范围");
            require(firstSection || section.startDwellMs == 0, "首点停留只能配置在第一段");
            require(!section.startMapYaw || std::isfinite(*section.startMapYaw), "起点航向无效");
            // In a closed repeat body the arrival back at the first point owns
            // its dwell. Do not count that same stop again at the next lap.
            if (section.startDwellMs > 0 && !options.repeatUntilStopped && options.repeatCount == 1) {
                const auto points = compiler.mapped(section.points);
                require(points.size() >= 2, "路径至少需要两个不同点");
                const auto next = std::find_if(points.begin() + 1, points.end(), [&](const QPointF &point) {
                    return QLineF(points.front(), point).length() > 1e-7;
                });
                require(next != points.end(), "路径长度为零");
                const double target = section.startMapYaw ? compiler.frame.yawToEnu(*section.startMapYaw)
                                                         : heading(points.front(), *next);
                compiler.wait(0, {}, section.id + "-start-position");
                compiler.turn(target, section.id + "-start-heading");
                compiler.wait(section.startDwellMs, section.id + "-start", section.id + "-start-dwell");
            }
            // A two-point edge shorter than the actual arrival tolerance has
            // an unreliable tangent. Preserve its independent stop path and
            // explicit endpoint heading; the IPC still checks live capture,
            // corridor and measured standstill before omitting entry alignment.
            const bool endpointCapture = options.shortPathEndpointCapture && options.vehicleFootprintRotation &&
                section.goalMapYaw && std::isfinite(*section.goalMapYaw) && section.points.size() == 2 &&
                QLineF(section.points.front(), section.points.back()).length() > 1e-7 &&
                QLineF(section.points.front(), section.points.back()).length() < options.goalToleranceMeters;
            const auto sectionBegin = compiler.steps.size();
            compiler.path(compiler.mapped(section.points), section.id, endpointCapture);
            if (options.shortPathEndpointCapture && options.vehicleFootprintRotation) {
                // An internal two-point microsegment has the same capture
                // contract as a separate tiny edge. Keep its stop and following
                // heading; omit only its entry turn. Do not cross section bounds
                // or change consecutive tiny paths, whose shared turn is needed
                // to keep each path's existing endpoint-capture precondition.
                for (qsizetype i = sectionBegin + 2; i + 2 < compiler.steps.size(); ++i) {
                    const auto before = compiler.steps[i - 2].toObject();
                    const auto path = compiler.steps[i].toObject();
                    const auto after = compiler.steps[i + 2].toObject();
                    if (before["type"] != "follow_path" || path["type"] != "follow_path" ||
                        after["type"] != "follow_path" ||
                        compiler.steps[i - 1].toObject()["type"] != "turn" ||
                        compiler.steps[i + 1].toObject()["type"] != "turn" ||
                        path["points"].toArray().size() != 2 || !shortPath(path) ||
                        shortPath(before) || shortPath(after))
                        continue;
                    compiler.steps.removeAt(i - 1);
                    // Earlier capture indices precede this removal; future
                    // sections have not been emitted, so only this index moves.
                    capturePaths.insert(--i);
                }
            }
            if (endpointCapture)
                capturePaths.insert(compiler.steps.size() - 1);
            if (section.goalMapYaw) {
                const double target = compiler.frame.yawToEnu(*section.goalMapYaw);
                if (section.dwellMs > 0 || endpointCapture || std::abs(MapGeometry::normalizeAngle(target - *compiler.lastHeading)) >
                    options.angleToleranceRad)
                    compiler.turn(target, section.id + "-heading");
            } else if (section.dwellMs > 0) {
                compiler.turn(*compiler.lastHeading, section.id + "-heading");
            }
            if (section.dwellMs > 0)
                compiler.wait(section.dwellMs, section.id, section.id + "-dwell");
            firstSection = false;
        }
        // Straight, equally constrained logical sections can flow continuously.
        for (qsizetype i = 0; i + 1 < compiler.steps.size(); ++i) {
            auto a = compiler.steps[i].toObject();
            const auto b = compiler.steps[i + 1].toObject();
            if (a["type"].toString() == "follow_path" && b["type"].toString() == "follow_path" &&
                !capturePaths.contains(i) && !capturePaths.contains(i + 1)) {
                a["endBehavior"] = "blend";
                compiler.steps[i] = a;
            }
        }
        return compiler.finish(name);
    });
}
CompiledTask TaskCompiler::row(const RowWorkPlan &row, const MapFrameBinding &binding,
                               const TaskCompileOptions &options, bool returnTrip, bool returnToStart) {
    return build([&] {
        Compiler compiler(binding, options);
        compiler.leg(row, false, "out");
        const auto taughtPath = compiler.mapped(RowWorkGeometry::pathBetweenProgress(row, 0, row.lineLength()));
        if (returnTrip) {
            compiler.turn(heading(taughtPath.last(), taughtPath[taughtPath.size() - 2]), "turn-end");
            compiler.leg(row, true, "return");
            compiler.turn(heading(taughtPath[0], taughtPath[1]), "turn-start");
        }
        if (returnToStart) {
            // Return preparation is one IPC path step. Unlike the formal body,
            // it cannot insert explicit turns at sharp teaching vertices.
            QList<QPointF> unique;
            for (const auto &point : taughtPath)
                if (unique.isEmpty() || QLineF(unique.last(), point).length() > 1e-8)
                    unique.append(point);
            for (qsizetype i = 1; i + 1 < unique.size(); ++i)
                require(std::abs(MapGeometry::normalizeAngle(heading(unique[i], unique[i + 1]) -
                                                             heading(unique[i - 1], unique[i]))) <= .40,
                        "示教路线含较大转角，当前自动返回起点不支持（转角超过 0.40 rad），本次未下发任务");
            QJsonArray returning;
            for (auto it = taughtPath.crbegin(); it != taughtPath.crend(); ++it)
                returning.append(QJsonArray{it->x(), it->y()});
            // The IPC enters this full B-to-A taught path using its fresh local pose.
            // Keep it separate from the formal steps so checkpoint actions are
            // preserved and repeat cycles can skip this one-time preparation.
            compiler.steps.prepend(QJsonObject{
                {"stepId", compiler.nextId("return-to-start")},
                {"type", "return_to_start"},
                {"points", returning},
                {"speedLimit", options.speedLimit},
                {"goalToleranceMeters", options.goalToleranceMeters},
                {"safetyProfileId", options.safetyProfileId},
                {"endBehavior", "stop"}});
        }
        return compiler.finish(QStringLiteral("单垄作业"));
    });
}

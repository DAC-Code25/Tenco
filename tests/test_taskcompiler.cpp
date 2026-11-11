#include "taskcompiler.h"
#include <QtTest>
#include <QSet>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

class TaskCompilerTest : public QObject {
    Q_OBJECT
    MapFrameBinding binding() const {
        MapFrameBinding b;
        b.confirmed = true;
        b.context = {"map", "origin", "transform", "calibration", "profile", 1};
        return b;
    }
    TaskCompileOptions options() const {
        TaskCompileOptions o;
        o.speedLimit = .10;
        o.goalToleranceMeters = .03;
        o.angularSpeedLimit = .35;
        o.angleToleranceRad = .05;
        o.turnTimeoutMs = 30000;
        o.safetyProfileId = "open";
        o.rotationZoneId = "open";
        return o;
    }
  private slots:
    void unfinishedCheckpointDraftCannotBecomeATask() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = true;
        RowCheckpoint checkpoint{"draft", 0, 1500};
        QVERIFY(RowWorkGeometry::setCheckpointPosition(row, {0, 1}, &checkpoint));
        row.checkpoints = {checkpoint};
        QVERIFY(!TaskCompiler::row(row, binding(), options(), false).ok());
        row.hasEndPose = true;
        row.endPose = {0, 2, 0};
        for (bool enabled : {true, false}) {
            row.checkpoints[0].enabled = enabled;
            const auto rejected = TaskCompiler::row(row, binding(), options(), false);
            QVERIFY(!rejected.ok());
            QVERIFY(rejected.error.contains(QStringLiteral("中间点尚未确认")));
        }
        row.checkpoints[0].enabled = true;
        QVERIFY(RowWorkGeometry::finalizeCheckpoints(&row));
        const auto result = TaskCompiler::row(row, binding(), options(), false);
        QVERIFY2(result.ok(), qPrintable(result.error));
        bool foundCheckpoint = false;
        for (const auto &step : result.plan["steps"].toArray())
            foundCheckpoint = foundCheckpoint || step.toObject()["checkpointId"] == "draft";
        QVERIFY(foundCheckpoint);
    }
    void controllerDefaultsPermitOrdinaryTurnsWithoutPerTaskSetup() {
        const QList<TaskRouteSection> corner{{"corner", {{0, 0}, {0, 2}, {-2, 2}}, {}}};
        TaskCompileOptions settings;
        QVERIFY(!TaskCompiler::route(corner, binding(), settings).ok());
        const QJsonObject configuration{
            {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"},
                                                {"default_safety_profile", "field-low-speed"},
                                                {"task_parameters_version", 1}, {"task_speed_mps", .1},
                                                {"task_turn_speed_radps", .2}, {"goal_tolerance_m", .03},
                                                {"angle_tolerance_rad", .05}, {"task_turn_timeout_ms", 43000}}},
            {"control", QJsonObject{{"max_linear_speed_mps", .1}, {"max_angular_speed_radps", .2}}}};
        settings.safetyProfileId = "obsolete-profile";
        settings.rotationZoneId = "obsolete-zone";
        settings.speedLimit = .8;
        settings.angularSpeedLimit = 1.5;
        settings.goalToleranceMeters = .5;
        settings.angleToleranceRad = .4;
        settings.applyControllerDefaults(configuration);
        const auto result = TaskCompiler::route(corner, binding(), settings);
        QVERIFY2(result.ok(), qPrintable(result.error));
        const auto steps = result.plan["steps"].toArray();
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps[0].toObject()["safetyProfileId"].toString(), "field-low-speed");
        QCOMPARE(steps[1].toObject()["rotationZoneId"].toString(), QString());
        QCOMPARE(steps[1].toObject()["angularSpeedLimit"].toDouble(), .2);
        QCOMPARE(steps[0].toObject()["speedLimit"].toDouble(), .1);
        QCOMPARE(steps[0].toObject()["goalToleranceMeters"].toDouble(), .03);
        QCOMPARE(steps[1].toObject()["angleToleranceRad"].toDouble(), .05);
        QCOMPARE(steps[1].toObject()["timeoutMs"].toInt(), 43000);
        TaskCompileOptions legacy;
        legacy.safetyProfileId = "open";
        legacy.applyControllerDefaults({{"navigation_defaults", QJsonObject{{"rotation_policy", "unknown"}}}});
        QVERIFY(!TaskCompiler::route(corner, binding(), legacy).ok());
    }
    void drivingSpeedRange() {
        TaskRouteSection line;
        line.id = "line";
        line.points = {{0, 0}, {0, 2}};
        for (double speed : {.10, .20, 1.50}) {
            auto settings = options(); settings.speedLimit = speed;
            const auto route = TaskCompiler::route({line}, binding(), settings);
            QVERIFY2(route.ok(), qPrintable(route.error));
            QCOMPARE(route.plan["steps"].toArray()[0].toObject()["speedLimit"].toDouble(), speed);
        }
        for (double speed : {.099, 1.501}) {
            auto settings = options(); settings.speedLimit = speed;
            QVERIFY(!TaskCompiler::route({line}, binding(), settings).ok());
        }
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {0, 2, 0};
        auto settings = options(); settings.speedLimit = .20;
        for (double speed : {.10, .20}) {
            settings.speedLimit = speed;
            auto legacy = RowWorkJson::planToJson(row);
            legacy["params"] = QJsonObject{{"baseLinearSpeed", .8}, {"maxLinearSpeed", 1.5},
                {"turnAngularSpeed", 2.0}, {"endpointArrivalDistance", .5}, {"checkpointArrivalTolerance", .4}};
            RowWorkPlan restored;
            QVERIFY(RowWorkJson::planFromJson(legacy, &restored));
            const auto compiled = TaskCompiler::row(restored, binding(), settings, false);
            QVERIFY2(compiled.ok(), qPrintable(compiled.error));
            QCOMPARE(compiled.plan["steps"].toArray()[0].toObject()["speedLimit"].toDouble(), speed);
        }
        settings.speedLimit = .099;
        QVERIFY(!TaskCompiler::row(row, binding(), settings, false).ok());
    }
    void slowTurnsUseTheControllerDeadlineAndRejectInvalidDeadlines() {
        const QList<TaskRouteSection> corner{{"corner", {{0, 0}, {0, 1}, {0, 0}}, {}}};
        auto defaults = QJsonObject{{"rotation_policy", "vehicle_footprint"}, {"default_safety_profile", "open"},
            {"task_parameters_version", 1}, {"task_speed_mps", .1}, {"task_turn_speed_radps", .05},
            {"goal_tolerance_m", .03}, {"angle_tolerance_rad", .05}, {"task_turn_timeout_ms", 82000}};
        TaskCompileOptions settings; settings.applyControllerDefaults({{"navigation_defaults", defaults}});
        const auto result = TaskCompiler::route(corner, binding(), settings);
        QVERIFY2(result.ok(), qPrintable(result.error));
        bool foundTurn = false;
        for (const auto &step : result.plan["steps"].toArray()) {
            if (step.toObject()["type"] != "turn") continue;
            foundTurn = true;
            QCOMPARE(step.toObject()["timeoutMs"].toInt(), 82000);
        }
        QVERIFY(foundTurn);
        for (const auto &invalid : {QJsonValue(), QJsonValue(true), QJsonValue(999), QJsonValue(86400001), QJsonValue(30000.5)}) {
            defaults["task_turn_timeout_ms"] = invalid;
            settings.applyControllerDefaults({{"navigation_defaults", defaults}});
            QVERIFY(!TaskCompiler::route(corner, binding(), settings).ok());
        }
    }
    void missingOrInvalidControllerParametersNeverReuseEarlierValues() {
        const QList<TaskRouteSection> line{{"line", {{0, 0}, {0, 2}}, {}}};
        TaskCompileOptions settings;
        QVERIFY(!TaskCompiler::route(line, binding(), settings).ok());
        auto defaults = QJsonObject{{"rotation_policy", "vehicle_footprint"}, {"default_safety_profile", "open"},
            {"task_parameters_version", 1}, {"task_speed_mps", .2}, {"task_turn_speed_radps", .3},
            {"goal_tolerance_m", .04}, {"angle_tolerance_rad", .06}, {"task_turn_timeout_ms", 30000}};
        for (const auto *key : {"task_parameters_version", "task_speed_mps", "task_turn_speed_radps",
                                 "goal_tolerance_m", "angle_tolerance_rad", "task_turn_timeout_ms", "default_safety_profile"}) {
            settings.applyControllerDefaults({{"navigation_defaults", defaults}});
            QVERIFY(TaskCompiler::route(line, binding(), settings).ok());
            auto broken = defaults;
            broken.remove(key);
            settings.applyControllerDefaults({{"navigation_defaults", broken}});
            QVERIFY(!TaskCompiler::route(line, binding(), settings).ok());
            broken[key] = true;
            settings.applyControllerDefaults({{"navigation_defaults", broken}});
            QVERIFY(!TaskCompiler::route(line, binding(), settings).ok());
        }
    }
    void axesAndRoundtrip() {
        auto b = binding();
        MapFrameAdapter identity(b);
        QCOMPARE(identity.toEnu({0, 1}), QPointF(1, 0));
        QCOMPARE(identity.toEnu({-1, 0}), QPointF(0, 1));
        b.enuTranslation = {10, 20};
        b.enuYawOffsetRad = .7;
        MapFrameAdapter transform(b);
        const auto back = transform.fromEnu(transform.toEnu({3, -2}));
        QVERIFY(QLineF(back, {3, -2}).length() < 1e-10);
        QVERIFY(std::abs(transform.yawFromEnu(transform.yawToEnu(.6)) - .6) < 1e-10);
        QVERIFY(MapFrameBinding::fromJson(b.toJson()).isUsable());
    }
    void cornersAndBlend() {
        QList<TaskRouteSection> route{{"corner", {{0, 0}, {0, 2}, {-2, 2}}, std::nullopt}};
        auto r = TaskCompiler::route(route, binding(), options());
        QVERIFY2(r.ok(), qPrintable(r.error));
        auto steps = r.plan["steps"].toArray();
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps[1].toObject()["type"].toString(), QString("turn"));
        QVERIFY(std::abs(steps[1].toObject()["targetYaw"].toDouble() - std::acos(-1.) / 2) < 1e-10);
        route = {{"a", {{0, 0}, {0, 1}}, std::nullopt}, {"b", {{0, 1}, {0, 2}}, std::nullopt}};
        r = TaskCompiler::route(route, binding(), options());
        QVERIFY(r.ok());
        QCOMPARE(r.plan["steps"].toArray()[0].toObject()["endBehavior"].toString(), QString("blend"));
        auto unbound = binding();
        unbound.confirmed = false;
        QVERIFY(!TaskCompiler::route(route, unbound, options()).ok());
        route[1].points[0] = {9, 9};
        QVERIFY(!TaskCompiler::route(route, binding(), options()).ok());
    }
    void shortPathCaptureIsDerivedFromTheAppliedFixedModeOnly() {
        QJsonObject defaults{{"rotation_policy", "vehicle_footprint"}, {"default_safety_profile", "open"},
            {"task_parameters_version", 1}, {"task_speed_mps", .2}, {"task_turn_speed_radps", .2},
            {"goal_tolerance_m", .1}, {"angle_tolerance_rad", .05}, {"task_turn_timeout_ms", 30000}};
        QJsonObject cfg{{"navigation_defaults", defaults},
            {"localizationPolicy", QJsonObject{{"controlBasis", "final_http"}}}};
        TaskCompileOptions settings;
        QVERIFY(!settings.shortPathEndpointCapture);
        settings.applyControllerDefaults(cfg);
        QVERIFY(settings.vehicleFootprintRotation);
        QVERIFY(settings.shortPathEndpointCapture);
        for (const QString basis : {QString(), QString("strict"), QString("unknown")}) {
            cfg["localizationPolicy"] = QJsonObject{{"controlBasis", basis}};
            settings.applyControllerDefaults(cfg);
            QVERIFY(!settings.shortPathEndpointCapture);
        }
        cfg["localizationPolicy"] = QJsonObject{{"controlBasis", "final_http"}};
        defaults["rotation_policy"] = "verified_zones";
        defaults["default_rotation_zone"] = "open";
        cfg["navigation_defaults"] = defaults;
        settings.applyControllerDefaults(cfg);
        QVERIFY(!settings.shortPathEndpointCapture);
        defaults["rotation_policy"] = "vehicle_footprint";
        cfg["navigation_defaults"] = defaults;
        settings.applyControllerDefaults(cfg);
        QVERIFY(settings.shortPathEndpointCapture);
        cfg.remove("navigation_defaults");
        settings.applyControllerDefaults(cfg);
        QVERIFY(!settings.shortPathEndpointCapture);
    }
    void recordedRegularRouteRetainsAllTwelveEdgesAndEndpointHeadings() {
        // Exact point coordinates/headings from the 2026-11-03-2 map, in ID order.
        const QList<QPointF> points{{-11.842, 8.123}, {-13.046, 10.265}, {-15.058, 13.803},
            {-16.616, 16.531}, {-16.604, 16.549}, {-18.316, 15.630}, {-18.302, 15.621},
            {-17.149, 13.527}, {-15.320, 10.356}, {-14.181, 8.273}, {-13.435, 6.911}, {-13.400, 6.927}};
        const QList<double> yaw{2.162796326794897, 1.989796326794897, 2.1397963267948965,
            2.132796326794897, -2.6133889803846895, -2.6343889803846903, -0.9592036732051032,
            -1.0532036732051036, -0.9152036732051037, -0.9542036732051034, -1.0712036732051033,
            0.5497963267948967};
        QList<TaskRouteSection> route;
        for (qsizetype i = 0; i < points.size(); ++i) {
            const auto next = (i + 1) % points.size();
            route.append({QString("route-%1-%2").arg(i + 1).arg(next + 1), {points[i], points[next]}, yaw[next]});
        }
        auto settings = options();
        settings.speedLimit = settings.angularSpeedLimit = .2;
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = true;
        const auto baseline = TaskCompiler::route(route, binding(), settings);
        QVERIFY2(baseline.ok(), qPrintable(baseline.error));
        QCOMPARE(baseline.plan["steps"].toArray().size(), 29);
        settings.shortPathEndpointCapture = true;
        const auto compiled = TaskCompiler::route(route, binding(), settings);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto steps = compiled.plan["steps"].toArray();
        QCOMPARE(steps.size(), 26);
        QCOMPARE(compiled.plan["context"], baseline.plan["context"]);
        QCOMPARE(compiled.plan["repeat"], baseline.plan["repeat"]);
        const MapFrameAdapter frame(binding());
        qsizetype paths = 0, turns = 0;
        QMap<QString, QJsonObject> shortHeadings;
        for (const auto &entry : steps) {
            const auto step = entry.toObject();
            if (step["type"] == "follow_path") {
                const auto &section = route[paths++];
                const auto a = frame.toEnu(section.points.front()), b = frame.toEnu(section.points.back());
                QCOMPARE(step["points"].toArray(), QJsonArray({QJsonArray{a.x(), a.y()}, QJsonArray{b.x(), b.y()}}));
                QCOMPARE(step["goalToleranceMeters"].toDouble(), .1);
                QCOMPARE(step["speedLimit"].toDouble(), .2);
                for (const auto &prefix : {QString("route-4-5"), QString("route-6-7"), QString("route-11-12")})
                    if (step["stepId"].toString().startsWith(prefix + "-"))
                        QCOMPARE(step["endBehavior"].toString(), "stop");
            } else {
                QCOMPARE(step["type"].toString(), "turn");
                ++turns;
                for (const auto &prefix : {QString("route-4-5"), QString("route-6-7"), QString("route-11-12")}) {
                    QVERIFY(!step["stepId"].toString().startsWith(prefix + "-align-"));
                    if (step["stepId"].toString().startsWith(prefix + "-heading-"))
                        shortHeadings.insert(prefix, step);
                }
            }
        }
        QCOMPARE(paths, 12);
        QCOMPARE(turns, 14);
        QCOMPARE(shortHeadings.size(), 3);
        for (const auto index : {3, 5, 10})
            QVERIFY(std::abs(std::remainder(shortHeadings[route[index].id]["targetYaw"].toDouble() -
                frame.yawToEnu(*route[index].goalMapYaw), 2 * std::acos(-1.))) < 1e-12);
        QCOMPARE(steps.last().toObject()["type"].toString(), "turn");
        QVERIFY(std::abs(steps.last().toObject()["targetYaw"].toDouble() - frame.yawToEnu(yaw.front())) < 1e-12);
    }
    void shortPathCaptureKeepsSeparateStopsEvenWhenNeighboringPathsAreCollinear() {
        const double mapEast = std::acos(-1.) / 2;
        const QList<TaskRouteSection> route{{"incoming", {{0, 0}, {0, 1}}, mapEast},
            {"short", {{0, 1}, {0, 1.02}}, mapEast},
            {"outgoing", {{0, 1.02}, {0, 2}}, mapEast}};
        auto settings = options();
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = settings.shortPathEndpointCapture = true;
        const auto compiled = TaskCompiler::route(route, binding(), settings);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto steps = compiled.plan["steps"].toArray();
        QCOMPARE(steps.size(), 4);
        QCOMPARE(steps[0].toObject()["type"].toString(), "follow_path");
        QCOMPARE(steps[0].toObject()["endBehavior"].toString(), "stop");
        QCOMPARE(steps[1].toObject()["type"].toString(), "follow_path");
        QCOMPARE(steps[1].toObject()["endBehavior"].toString(), "stop");
        QCOMPARE(steps[2].toObject()["type"].toString(), "turn");
        QCOMPARE(steps[2].toObject()["targetYaw"].toDouble(), 0.);
        QCOMPARE(steps[3].toObject()["type"].toString(), "follow_path");
    }
    void shortPathCaptureDoesNotChangeStrictLongCurvedOrUnheadedSections() {
        const double mapEast = std::acos(-1.) / 2;
        auto settings = options();
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = settings.shortPathEndpointCapture = true;
        const TaskRouteSection incoming{"incoming", {{0, 0}, {0, 1}}, mapEast};
        auto hasAlignment = [&](const TaskRouteSection &section, const TaskCompileOptions &cfg) {
            const auto result = TaskCompiler::route({incoming, section}, binding(), cfg);
            if (!result.ok()) return false;
            for (const auto &entry : result.plan["steps"].toArray())
                if (entry.toObject()["stepId"].toString().startsWith("short-align-")) return true;
            return false;
        };
        const TaskRouteSection shortPath{"short", {{0, 1}, {-.02, 1}}, mapEast};
        QVERIFY(!hasAlignment(shortPath, settings));
        auto strict = settings;
        strict.shortPathEndpointCapture = false;
        QVERIFY(hasAlignment(shortPath, strict));
        auto zones = settings;
        zones.vehicleFootprintRotation = false;
        QVERIFY(hasAlignment(shortPath, zones));
        auto tight = settings;
        tight.goalToleranceMeters = .01;
        QVERIFY(hasAlignment(shortPath, tight));
        QVERIFY(hasAlignment({"short", {{0, 1}, {-.1, 1}}, mapEast}, settings));
        QVERIFY(hasAlignment({"short", {{0, 1}, {-.2, 1}}, mapEast}, settings));
        QVERIFY(hasAlignment({"short", {{0, 1}, {-.01, 1}, {-.02, 1}}, mapEast}, settings));
        QVERIFY(hasAlignment({"short", {{0, 1}, {-.02, 1}}, std::nullopt}, settings));
        auto invalid = shortPath;
        invalid.goalMapYaw = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!TaskCompiler::route({incoming, invalid}, binding(), settings).ok());
        invalid = shortPath;
        invalid.points[1] = invalid.points[0];
        QVERIFY(!TaskCompiler::route({incoming, invalid}, binding(), settings).ok());
    }
    void recordedInternalTinyPathRetainsItsGeometryStopsAndFinalHeading() {
        // The 2026-11-05 16:20 execution split this one section into five paths.
        // Its 20.59 mm internal path was already within arrival tolerance.
        const QList<QPointF> enu{{38.658, 48.416}, {40.507, 48.687}, {42.911, 48.680},
            {45.093, 48.888}, {47.057, 49.048}, {49.380, 49.222}, {51.484, 49.373},
            {54.136, 49.652}, {56.644, 49.849}, {59.640, 50.113}, {61.846, 50.291},
            {63.895, 50.444}, {67.232, 50.782}, {68.683, 50.916}, {70.861, 51.114},
            {71.201, 50.409}, {71.305, 49.199}, {71.547, 47.443}, {71.719, 45.800},
            {71.988, 43.865}, {72.242, 41.816}, {72.260, 41.826}, {71.987, 44.410},
            {71.751, 47.263}, {71.317, 50.792}, {71.132, 52.389}};
        const MapFrameAdapter frame(binding());
        TaskRouteSection section{"recorded", {}, std::nullopt};
        for (const auto &point : enu) section.points.append(frame.fromEnu(point));
        auto settings = options();
        settings.speedLimit = settings.angularSpeedLimit = .2;
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = true;
        const auto baseline = TaskCompiler::route({section}, binding(), settings);
        QVERIFY2(baseline.ok(), qPrintable(baseline.error));
        auto expected = baseline.plan["steps"].toArray();
        QCOMPARE(expected.size(), 9);
        QCOMPARE(expected[5].toObject()["type"].toString(), "turn");
        QCOMPARE(expected[6].toObject()["points"].toArray(),
                 QJsonArray({QJsonArray{72.242, 41.816}, QJsonArray{72.260, 41.826}}));
        expected.removeAt(5); // Only the turn into the tiny tangent may disappear.
        settings.shortPathEndpointCapture = true;
        const auto compiled = TaskCompiler::route({section}, binding(), settings);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        QCOMPARE(compiled.plan["steps"].toArray(), expected);
        QCOMPARE(compiled.plan["context"], baseline.plan["context"]);
        QCOMPARE(compiled.plan["repeat"], baseline.plan["repeat"]);
    }
    void internalTinyPathCaptureKeepsConsecutiveAndOtherIneligiblePathsUnchanged() {
        const MapFrameAdapter frame(binding());
        auto settings = options();
        settings.goalToleranceMeters = .125; // Exactly representable equality boundary.
        settings.vehicleFootprintRotation = settings.shortPathEndpointCapture = true;
        auto unchanged = [&](const QList<QPointF> &enu, TaskCompileOptions cfg) {
            TaskRouteSection section{"guard", {}, std::nullopt};
            for (const auto &point : enu) section.points.append(frame.fromEnu(point));
            const auto compiled = TaskCompiler::route({section}, binding(), cfg);
            cfg.shortPathEndpointCapture = false;
            const auto baseline = TaskCompiler::route({section}, binding(), cfg);
            return compiled.ok() && baseline.ok() && compiled.plan["steps"] == baseline.plan["steps"];
        };
        const QList<QPointF> isolated{{0, 0}, {1, 0}, {1, .02}, {2, .02}};
        auto disabled = settings; disabled.shortPathEndpointCapture = false;
        QVERIFY(unchanged(isolated, disabled));
        auto strict = settings; strict.vehicleFootprintRotation = false;
        QVERIFY(unchanged(isolated, strict));
        auto tight = settings; tight.goalToleranceMeters = .01;
        QVERIFY(unchanged(isolated, tight));
        QVERIFY(unchanged({{0, 0}, {1, 0}, {1, .125}, {2, .125}}, settings));
        QVERIFY(unchanged({{0, 0}, {1, 0}, {1, .2}, {2, .2}}, settings));
        QVERIFY(unchanged({{0, 0}, {1, 0}, {1, .01}, {1.001, .02}, {2, .02}}, settings));
        QVERIFY(unchanged({{0, 0}, {1, 0}, {1, .02}}, settings)); // No following heading.
        QVERIFY(unchanged({{0, 0}, {0, .02}, {1, .02}}, settings)); // First path, not internal.
        QVERIFY(unchanged({{0, 0}, {1, 0}, {1, .02}, {1.02, .02}, {1.02, 1.02}}, settings));

        // Never reach across logical section boundaries to manufacture eligibility.
        const QList<TaskRouteSection> separate{{"in", {{0, 0}, {0, 1}}, std::nullopt},
            {"tiny", {{0, 1}, {-.02, 1}}, std::nullopt},
            {"out", {{-.02, 1}, {-.02, 2}}, std::nullopt}};
        const auto compiled = TaskCompiler::route(separate, binding(), settings);
        settings.shortPathEndpointCapture = false;
        const auto baseline = TaskCompiler::route(separate, binding(), settings);
        QVERIFY(compiled.ok() && baseline.ok());
        QCOMPARE(compiled.plan["steps"], baseline.plan["steps"]);
    }
    void internalTinyPathRemovalPreservesCaptureIndicesAcrossSections() {
        const MapFrameAdapter frame(binding());
        auto makeSection = [&](const QString &id, const QList<QPointF> &enu, bool headed) {
            TaskRouteSection section{id, {}, headed ? std::optional<double>(frame.yawFromEnu(0)) : std::nullopt};
            for (const auto &point : enu) section.points.append(frame.fromEnu(point));
            return section;
        };
        const QList<TaskRouteSection> route{
            makeSection("incoming", {{-1, 0}, {0, 0}}, true),
            makeSection("prefix", {{0, 0}, {.02, 0}}, true),
            makeSection("internal", {{.02, 0}, {1.02, 0}, {1.02, .02}, {2.02, .02}}, false),
            makeSection("suffix", {{2.02, .02}, {2.04, .02}}, true),
            makeSection("outgoing", {{2.04, .02}, {3.04, .02}}, false)};
        auto settings = options();
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = settings.shortPathEndpointCapture = true;
        const auto compiled = TaskCompiler::route(route, binding(), settings);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto steps = compiled.plan["steps"].toArray();
        QCOMPARE(steps.size(), 10);
        qsizetype paths = 0;
        for (const auto &entry : steps) {
            const auto step = entry.toObject();
            if (step["type"] == "follow_path") {
                ++paths;
                QCOMPARE(step["endBehavior"].toString(), "stop");
            }
        }
        QCOMPARE(paths, 7);
        // Two independent internal tiny paths in one section keep both stops.
        const auto twice = TaskCompiler::route({makeSection("twice",
            {{0, 0}, {1, 0}, {1, .02}, {2, .02}, {2, .04}, {3, .04}}, false)}, binding(), settings);
        QVERIFY2(twice.ok(), qPrintable(twice.error));
        QCOMPARE(twice.plan["steps"].toArray().size(), 7);
        for (const auto &entry : twice.plan["steps"].toArray())
            if (entry.toObject()["type"] == "follow_path")
                QCOMPARE(entry.toObject()["endBehavior"].toString(), "stop");
    }
    void internalTinyPathKeepsThePlannedTurnAcrossPi() {
        const MapFrameAdapter frame(binding());
        auto settings = options();
        settings.goalToleranceMeters = .1;
        settings.vehicleFootprintRotation = true;
        for (double yaw : {-1e-6, 0., 1e-6}) {
            TaskRouteSection section{"uturn", {}, std::nullopt};
            for (const QPointF &p : {QPointF{0, 0}, QPointF{1, 0}, QPointF{1, .02}, QPointF{0, .02}}) {
                const QPointF rotated{std::cos(yaw) * p.x() - std::sin(yaw) * p.y(),
                                      std::sin(yaw) * p.x() + std::cos(yaw) * p.y()};
                section.points.append(frame.fromEnu(rotated));
            }
            settings.shortPathEndpointCapture = false;
            const auto baseline = TaskCompiler::route({section}, binding(), settings);
            QVERIFY(baseline.ok());
            auto expected = baseline.plan["steps"].toArray();
            QCOMPARE(expected.size(), 5);
            expected.removeAt(1);
            settings.shortPathEndpointCapture = true;
            const auto compiled = TaskCompiler::route({section}, binding(), settings);
            QVERIFY(compiled.ok());
            QCOMPARE(compiled.plan["steps"].toArray(), expected);
            const double target = expected[2].toObject()["targetYaw"].toDouble();
            QVERIFY(std::isfinite(target) && target >= -std::acos(-1.) && target < std::acos(-1.));
        }
    }
    void routeDwellRequiresAnActualHeadingStepBeforeWaiting() {
        const MapFrameAdapter frame(binding());
        QList<TaskRouteSection> route{{"one", {{0, 0}, {0, 1}}, frame.yawFromEnu(0), 1250},
                                     {"two", {{0, 1}, {0, 2}}, std::nullopt}};
        auto compiled = TaskCompiler::route(route, binding(), options());
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        auto steps = compiled.plan["steps"].toArray();
        QCOMPARE(steps.size(), 4);
        QCOMPARE(steps[0].toObject()["type"].toString(), "follow_path");
        QCOMPARE(steps[0].toObject()["endBehavior"].toString(), "stop");
        QCOMPARE(steps[1].toObject()["type"].toString(), "turn");
        QCOMPARE(steps[1].toObject()["targetYaw"].toDouble(), 0.);
        QCOMPARE(steps[1].toObject()["angleToleranceRad"].toDouble(), options().angleToleranceRad);
        QCOMPARE(steps[2].toObject()["type"].toString(), "wait");
        QCOMPARE(steps[2].toObject()["checkpointId"].toString(), "one");
        QCOMPARE(steps[2].toObject()["completion"].toObject()["durationMs"].toInt(), 1250);
        QCOMPARE(steps[3].toObject()["type"].toString(), "follow_path");

        // No requested yaw still needs a real turn to the arrival tangent.
        route[0].goalMapYaw.reset();
        compiled = TaskCompiler::route(route, binding(), options());
        QVERIFY(compiled.ok());
        QCOMPARE(compiled.plan["steps"].toArray()[1].toObject()["type"].toString(), "turn");
        QCOMPARE(compiled.plan["steps"].toArray()[1].toObject()["targetYaw"].toDouble(), 0.);
        for (int invalid : {-1, 3600001}) {
            route[0].dwellMs = invalid;
            const auto rejected = TaskCompiler::route(route, binding(), options());
            QVERIFY(!rejected.ok());
            QVERIFY(rejected.plan.isEmpty());
        }
        route[0].dwellMs = 0;
        compiled = TaskCompiler::route(route, binding(), options());
        QVERIFY(compiled.ok());
        QCOMPARE(compiled.plan["steps"].toArray().size(), 2);
        QCOMPARE(compiled.plan["steps"].toArray()[0].toObject()["endBehavior"].toString(), "blend");
        route[0].dwellMs = 3600000;
        QVERIFY(TaskCompiler::route(route, binding(), options()).ok());
    }
    void routeStartDwellHasAPlannedPositionGuardBeforeTurning() {
        auto transformed = binding();
        transformed.enuTranslation = {7, -4};
        transformed.enuYawOffsetRad = .3;
        const MapFrameAdapter frame(transformed);
        QList<TaskRouteSection> route{{"one", {{0, 0}, {0, 1}}, std::nullopt, 500, 1500, .8},
                                     {"two", {{0, 1}, {0, 2}}, std::nullopt}};
        const auto result = TaskCompiler::route(route, transformed, options());
        QVERIFY2(result.ok(), qPrintable(result.error));
        const auto steps = result.plan["steps"].toArray();
        QCOMPARE(steps[0].toObject()["type"].toString(), "wait");
        QCOMPARE(steps[0].toObject()["completion"].toObject()["durationMs"].toInt(), 0);
        QVERIFY(steps[0].toObject()["checkpointId"].toString().isEmpty());
        QCOMPARE(steps[1].toObject()["type"].toString(), "turn");
        QCOMPARE(steps[1].toObject()["targetYaw"].toDouble(), frame.yawToEnu(.8));
        QCOMPARE(steps[2].toObject()["type"].toString(), "wait");
        QCOMPARE(steps[2].toObject()["completion"].toObject()["durationMs"].toInt(), 1500);
        QCOMPARE(steps[2].toObject()["checkpointId"].toString(), "one-start");
        int paths = 0, businessWaits = 0;
        for (const auto &entry : steps) {
            const auto step = entry.toObject();
            if (step["type"] == "follow_path") {
                ++paths;
                if (paths == 1) {
                    const auto start = frame.toEnu({0, 0});
                    QCOMPARE(step["points"].toArray().first().toArray(), QJsonArray({start.x(), start.y()}));
                }
            }
            if (step["type"] == "wait" && !step["checkpointId"].toString().isEmpty()) ++businessWaits;
        }
        QCOMPARE(paths, 2);
        QCOMPARE(businessWaits, 2);
        route[0].startMapYaw.reset();
        auto fallback = TaskCompiler::route(route, transformed, options());
        QVERIFY(fallback.ok());
        QVERIFY(std::abs(fallback.plan["steps"].toArray()[1].toObject()["targetYaw"].toDouble() - .3) < 1e-12);
        for (int invalid : {-1, 3600001}) {
            route[0].startDwellMs = invalid;
            QVERIFY(!TaskCompiler::route(route, transformed, options()).ok());
        }
        route[0].startDwellMs = 0;
        route[1].startDwellMs = 1;
        QVERIFY(!TaskCompiler::route(route, transformed, options()).ok());
        route[1].startDwellMs = 0;
        route[0].startMapYaw = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!TaskCompiler::route(route, transformed, options()).ok());

        // A closed repeat stops at A on its arrival; re-entering the same body
        // must not perform an extra startup dwell on that same point.
        route = {{"out", {{0, 0}, {0, 1}}, std::nullopt, 200, 1500, .8},
                 {"back", {{0, 1}, {0, 0}}, std::nullopt, 1500}};
        for (bool untilStopped : {false, true}) {
            auto repeat = options();
            repeat.repeatCount = 2;
            repeat.repeatUntilStopped = untilStopped;
            const auto repeated = TaskCompiler::route(route, transformed, repeat);
            QVERIFY2(repeated.ok(), qPrintable(repeated.error));
            int waits = 0;
            for (const auto &entry : repeated.plan["steps"].toArray()) {
                const auto step = entry.toObject();
                QVERIFY(!step["stepId"].toString().contains("start-dwell"));
                QVERIFY(!step["stepId"].toString().contains("start-position"));
                if (step["type"] == "wait") ++waits;
            }
            QCOMPARE(waits, 2);
        }
    }
    void rowDwellFacesTheLocalTravelDirectionBeforeEachAction() {
        auto transformed = binding();
        transformed.enuTranslation = {7, -4};
        transformed.enuYawOffsetRad = .3;
        const MapFrameAdapter frame(transformed);
        RowWorkPlan row;
        row.frameBinding = transformed;
        row.hasStartPose = row.hasEndPose = true;
        // Recorded yaw is deliberately unrelated: return travel must never use
        // the outbound teaching yaw as its parking target.
        row.startPose = {0, 0, 1.1};
        row.endPose = {6, 0, -.9};
        row.checkpoints = {{"start", 0, 100}, {"end", 6, 200}};
        RowCheckpoint middle{"middle", 0, 300, true, true, true, true, 12000};
        QVERIFY(RowWorkGeometry::setCheckpointPosition(row, {2, .2}, &middle));
        row.checkpoints.append(middle);
        middle.name = "same-position";
        middle.capturePhoto = false;
        row.checkpoints.append(middle);
        RowCheckpoint shape{"shape", 0, 0, false};
        QVERIFY(RowWorkGeometry::setCheckpointPosition(row, {4, -.1}, &shape));
        row.checkpoints.append(shape);
        const auto compiled = TaskCompiler::row(row, transformed, options(), true, true);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto steps = compiled.plan["steps"].toArray();
        QCOMPARE(steps[0].toObject()["type"].toString(), "return_to_start");
        QCOMPARE(steps[1].toObject()["type"].toString(), "wait");
        QCOMPARE(steps[1].toObject()["completion"].toObject()["durationMs"].toInt(), 0);
        QCOMPARE(steps[2].toObject()["type"].toString(), "turn");
        bool alignedSincePath = false;
        double actualTarget = 0;
        int businessActions = 0, middleTurns = 0;
        const auto enuHeading = [&](QPointF a, QPointF b) {
            a = frame.toEnu(a); b = frame.toEnu(b);
            return std::atan2(b.y() - a.y(), b.x() - a.x());
        };
        for (const auto &entry : steps) {
            const auto step = entry.toObject();
            const auto type = step["type"].toString();
            const auto id = step["stepId"].toString();
            if (type == "follow_path") alignedSincePath = false;
            if (type == "turn") {
                alignedSincePath = true;
                actualTarget = step["targetYaw"].toDouble();
                if (id.contains("checkpoint-heading") &&
                    std::abs(std::remainder(actualTarget - enuHeading({2, .2}, {4, -.1}), 2 * std::acos(-1.))) < 1e-12)
                    ++middleTurns;
            }
            if (type != "wait" || step["checkpointId"].toString().isEmpty()) continue;
            ++businessActions;
            QVERIFY(alignedSincePath);
            const bool backwards = id.startsWith("return-");
            const auto point = step["checkpointId"].toString();
            double expected;
            if (point == "start") expected = backwards ? enuHeading({2, .2}, {0, 0}) : enuHeading({0, 0}, {2, .2});
            else if (point == "end") expected = backwards ? enuHeading({6, 0}, {4, -.1}) : enuHeading({4, -.1}, {6, 0});
            else expected = backwards ? enuHeading({2, .2}, {0, 0}) : enuHeading({2, .2}, {4, -.1});
            QVERIFY(std::abs(std::remainder(actualTarget - expected, 2 * std::acos(-1.))) < 1e-12);
        }
        QCOMPARE(businessActions, 10); // Four dwell points and one capture, both directions.
        QCOMPARE(middleTurns, 1); // Both co-located outbound actions share their alignment.
        auto missingZone = options();
        missingZone.rotationZoneId.clear();
        QVERIFY(!TaskCompiler::row(row, transformed, missingZone, false).ok());
    }
    void rowForwardAndReturn() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.startPose = {0, 0, 0};
        row.endPose = {0, 2, 0};
        RowCheckpoint forward;
        forward.name = "forward";
        forward.progress = .5;
        forward.triggerOnBackward = false;
        RowCheckpoint back;
        back.name = "back";
        back.progress = 1.5;
        back.triggerOnForward = false;
        row.checkpoints = {forward, back};
        auto r = TaskCompiler::row(row, binding(), options(), true);
        QVERIFY2(r.ok(), qPrintable(r.error));
        const auto steps = r.plan["steps"].toArray();
        int turns = 0;
        QStringList checkpoints;
        for (const auto &v : steps) {
            const auto s = v.toObject();
            if (s["type"].toString() == "turn")
                ++turns;
            if (s["type"].toString() == "wait")
                checkpoints << s["checkpointId"].toString();
            QVERIFY(!s.contains("reverse"));
        }
        QCOMPARE(turns, 4);
        QCOMPARE(checkpoints, QStringList({"forward", "back"}));
        auto single = TaskCompiler::row(row, binding(), options(), false);
        QVERIFY(single.ok());
        QCOMPARE(single.plan["steps"].toArray().size(), 4);
        auto repeat = options();
        repeat.repeatCount = 2;
        QVERIFY(!TaskCompiler::row(row, binding(), repeat, false).ok());
        QVERIFY(TaskCompiler::row(row, binding(), repeat, true).ok());
    }
    void returnToStartPreservesFormalActionsAndRepeatBody() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {0, 4, 0};
        row.checkpoints = {{"start-photo", 0, 250, true, true, true, true, 12000},
                           {"middle", 2, 500}, {"end", 4, 750}};
        const auto original = RowWorkJson::planToJson(row);
        for (bool untilStopped : {false, true}) {
            auto settings = options();
            settings.speedLimit = .2;
            settings.goalToleranceMeters = .04;
            settings.safetyProfileId = "controller-profile";
            settings.repeatCount = 3;
            settings.repeatUntilStopped = untilStopped;
            const auto legacy = TaskCompiler::row(row, binding(), settings, true);
            const auto result = TaskCompiler::row(row, binding(), settings, true, true);
            QVERIFY2(legacy.ok(), qPrintable(legacy.error));
            QVERIFY2(result.ok(), qPrintable(result.error));
            auto steps = result.plan["steps"].toArray();
            const auto preparation = steps.takeAt(0).toObject();
            QCOMPARE(preparation["type"].toString(), "return_to_start");
            QCOMPARE(preparation["points"].toArray(), QJsonArray({QJsonArray{4, 0}, QJsonArray{0, 0}}));
            QCOMPARE(preparation["speedLimit"].toDouble(), .2);
            QCOMPARE(preparation["goalToleranceMeters"].toDouble(), .04);
            QCOMPARE(preparation["safetyProfileId"].toString(), "controller-profile");
            QCOMPARE(preparation["endBehavior"].toString(), "stop");
            QVERIFY(!preparation.contains("checkpointId"));
            QVERIFY(!preparation.contains("completion"));
            QCOMPARE(steps, legacy.plan["steps"].toArray());
            QCOMPARE(steps.first().toObject()["type"].toString(), "wait");
            QCOMPARE(steps.first().toObject()["completion"].toObject()["durationMs"].toInt(), 0);
            QVERIFY(steps.first().toObject()["checkpointId"].toString().isEmpty());
            QCOMPARE(steps[1].toObject()["type"].toString(), "turn");
            QCOMPARE(steps[2].toObject()["checkpointId"].toString(), "start-photo");
            QCOMPARE(result.plan["repeat"], legacy.plan["repeat"]);
            for (const auto &step : steps) {
                QVERIFY(step.toObject()["type"] != "return_to_start");
                QVERIFY(step.toObject()["stepId"] != preparation["stepId"]);
            }
            // Preparation cannot make an open A-to-B body into a valid loop.
            QVERIFY(!TaskCompiler::row(row, binding(), settings, false, true).ok());
        }
        QCOMPARE(RowWorkJson::planToJson(row), original);
        const auto forwardOnly = TaskCompiler::row(row, binding(), options(), false, true);
        QVERIFY2(forwardOnly.ok(), qPrintable(forwardOnly.error));
        QCOMPARE(forwardOnly.plan["steps"].toArray().first().toObject()["type"].toString(), "return_to_start");
    }
    void returnToStartUsesTheBoundEnuLine() {
        auto transformed = binding();
        transformed.enuTranslation = {8, -3};
        transformed.enuYawOffsetRad = .7;
        RowWorkPlan row;
        row.frameBinding = transformed;
        row.hasStartPose = row.hasEndPose = true;
        row.startPose = {-1, 2, .3};
        row.endPose = {-1, 6, .3};
        const auto result = TaskCompiler::row(row, transformed, options(), false, true);
        QVERIFY2(result.ok(), qPrintable(result.error));
        const auto steps = result.plan["steps"].toArray();
        const auto returning = steps.first().toObject()["points"].toArray();
        const auto outbound = steps[1].toObject()["points"].toArray();
        QCOMPARE(returning.first(), outbound.last());
        QCOMPARE(returning.last(), outbound.first());
        const MapFrameAdapter frame(transformed);
        const auto end = frame.toEnu(row.endPose.toPointF());
        const auto start = frame.toEnu(row.startPose.toPointF());
        QCOMPARE(returning, QJsonArray({QJsonArray{end.x(), end.y()}, QJsonArray{start.x(), start.y()}}));
        row.frameBinding.context.originRevision = "previous-origin";
        QVERIFY(!TaskCompiler::row(row, transformed, options(), false, true).ok());
        row.frameBinding = transformed;
        row.endPose = row.startPose;
        QVERIFY(!TaskCompiler::row(row, transformed, options(), false, true).ok());
    }
    void taughtPolylineIsIdenticalForOutboundReturnAndPreparation() {
        auto transformed = binding();
        transformed.enuTranslation = {8, -3};
        transformed.enuYawOffsetRad = .7;
        RowWorkPlan row;
        row.frameBinding = transformed;
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {8, 0, 0};
        for (const auto &point : QList<QPointF>{{2, .2}, {4, -.1}, {6, .1}}) {
            RowCheckpoint checkpoint{QString("P%1").arg(row.checkpoints.size() + 1), 0, 750};
            checkpoint.enabled = false;
            QVERIFY(RowWorkGeometry::setCheckpointPosition(row, point, &checkpoint));
            row.checkpoints.append(checkpoint);
        }
        const MapFrameAdapter frame(transformed);
        QList<QPointF> expected;
        QJsonArray expectedJson;
        for (const auto &point : QList<QPointF>{{0, 0}, {2, .2}, {4, -.1}, {6, .1}, {8, 0}}) {
            const auto mapped = frame.toEnu(point);
            expected.append(mapped);
            expectedJson.append(QJsonArray{mapped.x(), mapped.y()});
        }
        auto reverseExpected = expected;
        std::reverse(reverseExpected.begin(), reverseExpected.end());
        QJsonArray reverseJson;
        for (const auto &point : reverseExpected) reverseJson.append(QJsonArray{point.x(), point.y()});
        for (bool actions : {false, true}) {
            row.checkpoints[1].enabled = actions;
            row.checkpoints[1].capturePhoto = actions;
            auto settings = options();
            settings.speedLimit = .2;
            settings.repeatCount = 2;
            RowWorkPlan loaded;
            QVERIFY(RowWorkJson::planFromJson(RowWorkJson::planToJson(row), &loaded));
            const auto result = TaskCompiler::row(loaded, transformed, settings, true, true);
            QVERIFY2(result.ok(), qPrintable(result.error));
            const auto steps = result.plan["steps"].toArray();
            QCOMPARE(steps.first().toObject()["type"].toString(), "return_to_start");
            QCOMPARE(steps.first().toObject()["points"].toArray(), reverseJson);
            QList<QPointF> outbound, returning;
            QList<double> endpointTurns;
            int businessActions = 0;
            for (const auto &entry : steps) {
                const auto step = entry.toObject();
                const auto id = step["stepId"].toString();
                if (step["type"] == "follow_path") {
                    QCOMPARE(step["speedLimit"].toDouble(), .2);
                    auto &points = id.startsWith("out-") ? outbound : returning;
                    for (const auto &value : step["points"].toArray()) {
                        const auto xy = value.toArray();
                        const QPointF point(xy[0].toDouble(), xy[1].toDouble());
                        if (points.isEmpty() || points.last() != point) points.append(point);
                    }
                }
                if (id.startsWith("turn-end-") || id.startsWith("turn-start-"))
                    endpointTurns.append(step["targetYaw"].toDouble());
                if (step["type"] == "wait") {
                    ++businessActions;
                    QCOMPARE(step["checkpointId"].toString(), "P2");
                }
            }
            QCOMPARE(outbound, expected);
            QCOMPARE(returning, reverseExpected);
            QCOMPARE(businessActions, actions ? 4 : 0); // Dwell and capture on both formal legs only.
            QCOMPARE(endpointTurns.size(), 2);
            const auto backwards = reverseExpected[1] - reverseExpected[0];
            const auto forwards = expected[1] - expected[0];
            QVERIFY(std::abs(std::remainder(endpointTurns[0] - std::atan2(backwards.y(), backwards.x()),
                                           2 * std::acos(-1.))) < 1e-12);
            QVERIFY(std::abs(std::remainder(endpointTurns[1] - std::atan2(forwards.y(), forwards.x()),
                                           2 * std::acos(-1.))) < 1e-12);
            if (!actions) QCOMPARE(steps[1].toObject()["points"].toArray(), expectedJson);
        }
    }
    void invalidTaughtGeometryCannotBecomeAFlattenedTask() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {8, 0, 0};
        RowCheckpoint first{"P1", 0, 1000};
        QVERIFY(RowWorkGeometry::setCheckpointPosition(row, {2, .2}, &first));
        row.checkpoints = {first, first};
        row.checkpoints[1].enabled = false;
        row.checkpoints[1].recordedMapPosition = QPointF(2, -.2);
        for (bool preparation : {false, true}) {
            const auto rejected = TaskCompiler::row(row, binding(), options(), true, preparation);
            QVERIFY(!rejected.ok());
            QVERIFY(rejected.plan.isEmpty());
        }
        row.checkpoints[1].recordedMapPosition = QPointF(8.1, .2);
        QVERIFY(!TaskCompiler::row(row, binding(), options(), false, true).ok());
        row.checkpoints[1].recordedMapPosition = QPointF(2, std::numeric_limits<double>::quiet_NaN());
        QVERIFY(!TaskCompiler::row(row, binding(), options(), false, true).ok());
    }
    void returnPreparationRejectsSharpTaughtCornersBeforeUpload() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {4, 0, 0};
        for (double cornerAngle : {.39, .41}) {
            row.checkpoints.clear();
            RowCheckpoint checkpoint{"bend", 0, 1000};
            checkpoint.enabled = false;
            QVERIFY(RowWorkGeometry::setCheckpointPosition(row, {2, 2 * std::tan(cornerAngle / 2)}, &checkpoint));
            row.checkpoints.append(checkpoint);
            // Formal legs retain explicit stop/turn boundaries for either bend.
            QVERIFY(TaskCompiler::row(row, binding(), options(), true, false).ok());
            const auto prepared = TaskCompiler::row(row, binding(), options(), true, true);
            if (cornerAngle < .40) {
                QVERIFY2(prepared.ok(), qPrintable(prepared.error));
            } else {
                QVERIFY(!prepared.ok());
                QVERIFY(prepared.plan.isEmpty());
                QVERIFY(prepared.error.contains(QStringLiteral("自动返回起点")));
                QVERIFY(prepared.error.contains(QStringLiteral("0.40 rad")));
            }
        }
    }
    void persistedMultiStopRoundTripPreservesEveryBusinessAction() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {0, 4, 0};
        row.checkpoints = {
            {"end", 4, 1000},
            {"shared-zero", 2, 0},
            {"out-only", 1, 1500, true, true, false},
            {"disabled", .5, 9876, false},
            {"photo", 2, 2300, true, true, true, true, 12000},
            {"back-only", 3, 4500, true, false, true},
            {"start", 0, 750},
            {"never", .75, 9999, true, false, false}};
        RowWorkPlan restored;
        QVERIFY(RowWorkJson::planFromJson(RowWorkJson::planToJson(row), &restored));
        auto settings = options();
        settings.repeatCount = 7;
        const auto result = TaskCompiler::row(restored, binding(), settings, true);
        QVERIFY2(result.ok(), qPrintable(result.error));
        QCOMPARE(result.plan["repeat"].toObject(), QJsonObject({{"mode", "count"}, {"count", 7}}));
        const auto steps = result.plan["steps"].toArray();
        QCOMPARE(steps.size(), 28);
        QStringList checkpoints;
        QList<int> dwell;
        QList<QPointF> pathEnds;
        QList<double> turns;
        QSet<QString> ids;
        int captures = 0;
        for (qsizetype i = 0; i < steps.size(); ++i) {
            const auto step = steps[i].toObject();
            const QString id = step["stepId"].toString();
            QVERIFY(!id.isEmpty());
            QVERIFY(!ids.contains(id));
            ids.insert(id);
            const auto type = step["type"].toString();
            if (type == "follow_path") {
                const auto points = step["points"].toArray();
                const auto end = points.last().toArray();
                pathEnds.append(QPointF(end[0].toDouble(), end[1].toDouble()));
                QCOMPARE(step["endBehavior"].toString(), "stop");
                QVERIFY(points[0].toArray() != points.last().toArray());
            } else if (type == "turn") {
                turns.append(step["targetYaw"].toDouble());
            } else {
                QCOMPARE(type, "wait");
                const auto completion = step["completion"].toObject();
                if (completion["type"] == "timer") {
                    if (step["checkpointId"].toString().isEmpty()) {
                        QCOMPARE(i, 0);
                        QCOMPARE(completion["durationMs"].toInt(), 0);
                        continue;
                    }
                    checkpoints.append(step["checkpointId"].toString());
                    dwell.append(completion["durationMs"].toInt());
                } else {
                    ++captures;
                    QCOMPARE(completion["type"].toString(), "external_ack");
                    QCOMPARE(completion["eventType"].toString(), "capture");
                    QCOMPARE(completion["timeoutMs"].toInt(), 12000);
                    QVERIFY(i > 0);
                    const auto previous = steps[i - 1].toObject();
                    QCOMPARE(previous["checkpointId"].toString(), "photo");
                    QCOMPARE(previous["completion"].toObject()["durationMs"].toInt(), 2300);
                }
            }
        }
        QCOMPARE(checkpoints, QStringList({"start", "out-only", "shared-zero", "photo", "end",
                                          "end", "back-only", "shared-zero", "photo", "start"}));
        QCOMPARE(dwell, QList<int>({750, 1500, 0, 2300, 1000, 1000, 4500, 0, 2300, 750}));
        QCOMPARE(captures, 2);
        QCOMPARE(pathEnds, QList<QPointF>({{1, 0}, {2, 0}, {4, 0}, {3, 0}, {2, 0}, {0, 0}}));
        QCOMPARE(turns.size(), 9);
        for (qsizetype i = 0; i < turns.size(); ++i) {
            if (i >= 4 && i <= 7)
                QVERIFY(std::abs(std::abs(turns[i]) - std::acos(-1.)) < 1e-10);
            else
                QVERIFY(std::abs(turns[i]) < 1e-10);
        }

        settings.repeatCount = 1;
        const auto forward = TaskCompiler::row(restored, binding(), settings, false);
        QVERIFY2(forward.ok(), qPrintable(forward.error));
        QCOMPARE(forward.plan["steps"].toArray().size(), 14);
        settings.repeatUntilStopped = true;
        const auto looping = TaskCompiler::row(restored, binding(), settings, true);
        QVERIFY2(looping.ok(), qPrintable(looping.error));
        QCOMPARE(looping.plan["repeat"].toObject()["mode"].toString(), "until_stopped");
        QCOMPARE(looping.plan["steps"], result.plan["steps"]);
        QVERIFY(!TaskCompiler::row(restored, binding(), settings, false).ok());
    }
    void rowInvalidBusinessDataCannotBecomeACompiledTask() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {0, 2, 0};
        RowCheckpoint checkpoint{"point", 1, 0};
        for (int duration : {0, 1, 3600000}) {
            checkpoint.dwellMs = duration;
            row.checkpoints = {checkpoint};
            const auto result = TaskCompiler::row(row, binding(), options(), false);
            QVERIFY2(result.ok(), qPrintable(result.error));
            QCOMPARE(result.plan["steps"].toArray()[2].toObject()["completion"].toObject()["durationMs"].toInt(), duration);
        }
        for (int duration : {-1, 3600001}) {
            checkpoint.dwellMs = duration;
            row.checkpoints = {checkpoint};
            QVERIFY(!TaskCompiler::row(row, binding(), options(), false).ok());
        }
        checkpoint.dwellMs = 1000;
        for (double progress : {-.001, 2.001, std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity()}) {
            checkpoint.progress = progress;
            row.checkpoints = {checkpoint};
            QVERIFY(!TaskCompiler::row(row, binding(), options(), false).ok());
        }
        checkpoint.progress = 1;
        checkpoint.capturePhoto = true;
        for (int timeout : {999, 3600001}) {
            checkpoint.actionTimeoutMs = timeout;
            row.checkpoints = {checkpoint};
            QVERIFY(!TaskCompiler::row(row, binding(), options(), false).ok());
        }
        checkpoint.actionTimeoutMs = 1000;
        row.checkpoints = {checkpoint};
        QVERIFY(TaskCompiler::row(row, binding(), options(), false).ok());
        for (int count : {0, -1, 1000001}) {
            auto settings = options();
            settings.repeatCount = count;
            QVERIFY(!TaskCompiler::row(row, binding(), settings, true).ok());
        }
        auto settings = options();
        settings.repeatCount = 1000000;
        QVERIFY(TaskCompiler::row(row, binding(), settings, true).ok());
    }
    void tooManyCheckpointsFailWithoutPartialTask() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.endPose = {0, 2, 0};
        for (int i = 0; i < 496; ++i)
            row.checkpoints.append({QString::number(i), 0, 0});
        row.checkpoints.append({"out-only-limit", 0, 0, true, true, false});
        const auto maximum = TaskCompiler::row(row, binding(), options(), true);
        QVERIFY2(maximum.ok(), qPrintable(maximum.error));
        QCOMPARE(maximum.plan["steps"].toArray().size(), 1000);
        const auto excessivePreparation = TaskCompiler::row(row, binding(), options(), true, true);
        QVERIFY(!excessivePreparation.ok());
        QVERIFY(excessivePreparation.plan.isEmpty());
        row.checkpoints.append({"over-limit", 0, 0});
        const auto excessive = TaskCompiler::row(row, binding(), options(), true);
        QVERIFY(!excessive.ok());
        QVERIFY(excessive.plan.isEmpty());
    }
    void sequencePrecision() {
        quint64 seq = 0;
        QVERIFY(TrackingJson::sequence(QString("18446744073709551615"), &seq));
        QCOMPARE(seq, UINT64_MAX);
        QVERIFY(!TrackingJson::sequence(42, &seq));
        QVERIFY(!TrackingJson::sequence(QString("-1"), &seq));
        QVERIFY(!TrackingJson::sequence(QString("18446744073709551616"), &seq));
    }
    void persistedBindingAndExplicitTolerance() {
        RowWorkPlan row;
        row.frameBinding = binding();
        row.hasStartPose = row.hasEndPose = true;
        row.startPose = {0, 0, 0};
        row.endPose = {0, 2, 0};
        RowCheckpoint photo;
        photo.name = "camera";
        photo.progress = 1;
        photo.capturePhoto = true;
        photo.actionTimeoutMs = 12000;
        row.checkpoints = {photo};
        auto oldFile = RowWorkJson::planToJson(row);
        oldFile["params"] = QJsonObject{{"endpointArrivalDistance", .15}, {"checkpointArrivalTolerance", .5}};
        RowWorkPlan loaded;
        QVERIFY(RowWorkJson::planFromJson(oldFile, &loaded));
        auto result = TaskCompiler::row(loaded, binding(), options(), false);
        QVERIFY2(result.ok(), qPrintable(result.error));
        const auto steps = result.plan["steps"].toArray();
        QCOMPARE(steps.last().toObject()["goalToleranceMeters"].toDouble(), .03);
        QCOMPARE(steps.first().toObject()["goalToleranceMeters"].toDouble(), .03);
        QCOMPARE(steps[3].toObject()["completion"].toObject()["type"].toString(), "external_ack");
        QCOMPARE(steps[3].toObject()["completion"].toObject()["timeoutMs"].toInt(), 12000);
        loaded.frameBinding.context.originRevision = "old";
        QVERIFY(!TaskCompiler::row(loaded, binding(), options(), false).ok());
        auto legacy = RowWorkJson::planToJson(row);
        legacy.remove("schemaVersion");
        legacy.remove("frameBinding");
        QVERIFY(RowWorkJson::planFromJson(legacy, &loaded));
        QVERIFY(!TaskCompiler::row(loaded, binding(), options(), false).ok());
    }
};
QTEST_APPLESS_MAIN(TaskCompilerTest)
#include "test_taskcompiler.moc"

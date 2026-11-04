#include <QtWidgets>
#include "rowworktypes.h"
#include "taskcompiler.h"
#include "jsonhttpclient.h"
#define private public
#include "map.h"
#undef private
#include "ui_mainwindow.h"
#include "configmanager.h"
#include "controlsessioncoordinator.h"
#include <cmath>

// Uses the real Qt planner/client over an IPC tunnel and the IPC's simulated
// chassis. Deliberately has no manual session/profile/rotation-zone setup.
int runNavigationDefaultsProbe(QApplication &app) {
    QMainWindow window;
    Ui::MainWindow ui;
    ui.setupUi(&window);
    Map map(&ui);
    PoseClient pose;
    TrackingClient tracking;
    const auto token = ConfigManager::instance().network().authToken;
    pose.configure(QUrl("http://127.0.0.1:19131/api/v1"), token);
    tracking.configure(QUrl("http://127.0.0.1:19130/api/v1"), token);
    ControlSessionCoordinator coordinator(&tracking, &pose);
    map.setControlCoordinator(&coordinator);
    int phase = 0, maxLoop = 0, turns = 0;
    QString execution;
    QJsonObject plan;
    QPointF start;
    QElapsedTimer deadline, stage;
    deadline.start();
    bool failed = false, success = false;
    auto fail = [&](const QString &message) {
        if (failed || success) return;
        failed = true;
        QTextStream(stderr) << "FAIL defaults phase " << phase << ": " << message << Qt::endl;
        coordinator.emergencyStop();
        QTimer::singleShot(250, &app, [&app] { app.exit(1); });
    };
    QObject::connect(&coordinator, &ControlSessionCoordinator::message, &app,
                     [](const QString &message) { QTextStream(stdout) << message << Qt::endl; });
    QObject::connect(&tracking, &TrackingClient::commandFinished, &app,
                     [&](const QString &, const QJsonObject &result) {
        if (result["state"].toString() != "applied")
            fail(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    });
    QTimer confirmDialog;
    QObject::connect(&confirmDialog, &QTimer::timeout, &app, [&] {
        auto *dialog = window.findChild<QDialog *>("remoteTaskReviewDialog");
        if (!dialog || !dialog->isVisible()) return;
        auto *verified = dialog->findChild<QCheckBox *>("remoteTaskVerified");
        if (!verified || !verified->isEnabled()) {
            dialog->reject(); fail("remote plan review unavailable"); return;
        }
        verified->setChecked(true);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    confirmDialog.start(20);
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (failed || success) return;
        const auto status = tracking.snapshot();
        maxLoop = std::max(maxLoop, status.loopIndex);
        if (deadline.elapsed() > 360000) {
            fail(QString("deadline state=%1 pause=%2 fault=%3 step=%4 loop=%5")
                 .arg(status.state, status.pauseReason, status.faultCode, status.stepId).arg(status.loopIndex));
            return;
        }
        if (tracking.fresh() && (status.state == "Fault" || status.state == "Aborted")) {
            fail(QString("unexpected terminal state %1/%2").arg(status.state, status.faultCode)); return;
        }
        if (phase == 0 && coordinator.coordinatesReady() && pose.fresh()) {
            if (!map.m_binding.isUsable() || tracking.hasSession()) {
                fail("new map binding/session precondition"); return;
            }
            const auto config = tracking.configuration();
            if (config["navigation_defaults"].toObject()["rotation_policy"].toString() != "vehicle_footprint") {
                fail("controller did not advertise fixed navigation defaults"); return;
            }
            const auto p = pose.snapshot(); start = p.position;
            MapFrameAdapter frame(map.m_binding);
            QList<QPointF> points;
            for (const auto &offset : QList<QPointF>{{0, 0}, {.4, 0}, {.4, .3}, {0, .3}, {0, 0}})
                points.append(frame.fromEnu(start + offset));
            // Establish the map revision before compiling the submitted task.
            for (int i = 0; i < 4; ++i)
                if (!map.addPointInternal(points[i].x(), points[i].y(), 0, i + 1)) {
                    fail("map point creation failed"); return;
                }
            map.refreshPlanningRevision();
            map.syncCommittedMapState();
            auto options = map.taskOptions();
            options.repeatCount = 2;
            auto compiled = TaskCompiler::route({{"rectangle", points, frame.yawFromEnu(0)}},
                                                 map.m_binding, options, "标准导航双圈矩形联调");
            if (!compiled.ok()) { fail(compiled.error); return; }
            plan = compiled.plan;
            if (plan["context"].toObject() != coordinator.binding().context.toJson()) {
                fail("compiled plan context differs from current map"); return;
            }
            auto steps = plan["steps"].toArray();
            for (const auto &value : steps) {
                const auto step = value.toObject();
                if (step["type"].toString() == "turn") {
                    ++turns;
                    if (!step["rotationZoneId"].toString().isEmpty()) {
                        fail("compiler unexpectedly requires named rotation zone"); return;
                    }
                }
            }
            if (turns != 4 || !options.vehicleFootprintRotation || options.safetyProfileId.isEmpty()) {
                fail("automatic controller defaults were not used"); return;
            }
            steps.append(QJsonObject{{"stepId", "settled-wait"}, {"type", "wait"},
                {"completion", QJsonObject{{"type", "timer"}, {"durationMs", 200}}}});
            plan["steps"] = steps;
            if (!coordinator.uploadAndStart(plan)) { fail("one-click deployment intent not accepted"); return; }
            phase = 1;
            QTextStream(stdout) << "PASS automatic empty-map binding, defaults, 4 turn compilation, one-click deployment intent" << Qt::endl;
        } else if (phase == 1 && status.state == "Tracking" && status.measuredV > .025) {
            execution = status.executionId;
            coordinator.pauseTask(); phase = 4;
        } else if (phase == 4 && status.state == "Paused" && !tracking.busy() &&
                   std::abs(status.measuredV) < .002 && std::abs(status.measuredOmega) < .002) {
            tracking.stop(); pose.stop();
            tracking.start(); pose.start();
            stage.start(); phase = 5;
        } else if (phase == 5 && coordinator.coordinatesReady() && stage.elapsed() > 1300) {
            if (status.state != "Paused" || status.executionId != execution ||
                !tracking.preparedPlan().isEmpty() || tracking.hasSession()) {
                fail("reconnection replayed operation or changed execution"); return;
            }
            tracking.readTask(status.taskId, status.taskRevision); phase = 6;
        } else if (phase == 6 && !tracking.preparedPlan().isEmpty()) {
            if (status.state != "Paused" || status.executionId != execution) {
                fail("readback confirmation started motion"); return;
            }
            stage.start(); phase = 7;
        } else if (phase == 7 && stage.elapsed() > 400) {
            if (status.state != "Paused" || std::abs(status.measuredV) > .002 ||
                std::abs(status.measuredOmega) > .002) { fail("confirmed readback moved vehicle"); return; }
            coordinator.resumeTask(); phase = 8;
            QTextStream(stdout) << "PASS one-click prepare/Ready/start, pause, reconnect no replay, confirmed readback no motion, explicit resume" << Qt::endl;
        } else if (phase == 8 && status.state == "Completed") {
            const auto endpoint = pose.snapshot();
            const double error = QLineF(start, endpoint.position).length();
            if (maxLoop < 1 || error > .03 || std::abs(status.measuredV) > .02 ||
                std::abs(status.measuredOmega) > .03) {
                fail(QString("completion mismatch loop=%1 error=%2").arg(maxLoop).arg(error)); return;
            }
            success = true;
            QTextStream(stdout) << "PASS Qt/IPC defaults rectangle Follow+Turn+Wait 2 loops, same execution="
                << execution << ", endpointError=" << error << "m elapsed=" << deadline.elapsed() << "ms" << Qt::endl;
            coordinator.shutdown();
            app.exit(0);
        }
    });
    pose.start(); tracking.start(); poll.start();
    return app.exec();
}

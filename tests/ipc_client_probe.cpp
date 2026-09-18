#include "controlsessioncoordinator.h"
#include "chassisclient.h"
#include "externaleventcoordinator.h"
#include "taskcompiler.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>

// Talks to the real IPC services and a loopback chassis simulator. No control law here.
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    PoseClient pose;
    TrackingClient tracking;
    ChassisClient chassis;
    chassis.setUrl(QUrl("ws://127.0.0.1:19132"));
    pose.configure(QUrl("http://127.0.0.1:19131/api/v1"), {});
    tracking.configure(QUrl("http://127.0.0.1:19130/api/v1"), {});
    ControlSessionCoordinator coordinator(&tracking, &pose);
    QTemporaryDir journal;
    ExternalEventCoordinator events(&tracking);
    events.configure(QUrl("http://127.0.0.1:19133"), {}, journal.path(), 1000);
    MapFrameBinding binding;
    binding.confirmed = true;
    binding.context = {"test-map",       "simulation-origin", "test-transform",
                       "simulation-cal", "simulation-1",      1};
    coordinator.setBinding(binding);
    int phase = 0;
    QElapsedTimer deadline;
    deadline.start();
    auto fail = [&](const QString &error) {
        QTextStream(stderr) << "FAIL phase " << phase << ": " << error << Qt::endl;
        app.exit(1);
    };
    QObject::connect(&coordinator, &ControlSessionCoordinator::message, &app,
                     [](const QString &m) { QTextStream(stdout) << m << Qt::endl; });
    QObject::connect(&tracking, &TrackingClient::commandFinished, &app,
                     [&](const QString &op, const QJsonObject &result) {
                         if (result["state"].toString() != "applied")
                             fail(QString::fromUtf8(QJsonDocument(result).toJson()));
                     });
    QObject::connect(&pose, &PoseClient::captureFinished, &app, [&](const QJsonObject &capture) {
        if (capture["state"].toString() != "succeeded") {
            fail("capture failed");
            return;
        }
        TaskCompileOptions options;
        options.safetyProfileId = "open";
        options.rotationZoneId = "open";
        auto task = TaskCompiler::route({{"out", {{0, 0}, {0, .6}}, {}}}, binding, options);
        auto steps = task.plan["steps"].toArray();
        steps.append(QJsonObject{
            {"stepId", "photo"},
            {"type", "wait"},
            {"completion",
             QJsonObject{{"type", "external_ack"}, {"eventType", "capture"}, {"timeoutMs", 5000}}}});
        task.plan["steps"] = steps;
        if (!task.ok() || !coordinator.upload(task.plan)) {
            fail("upload not sent: " + task.error);
            return;
        }
        phase = 3;
    });
    QObject::connect(&events, &ExternalEventCoordinator::message, &app,
                     [](const QString &m) { QTextStream(stdout) << m << Qt::endl; });
    QObject::connect(&tracking, &TrackingClient::eventReceived, &app, [](const QJsonObject &event) {
        QTextStream(stdout) << QJsonDocument(event).toJson(QJsonDocument::Compact) << Qt::endl;
    });
    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &app, [&] {
        if (deadline.elapsed() > 40000) {
            fail(QString("deadline status=%1 pose=%2 tracking=%3 chassis=%4 reason=%5")
                     .arg(tracking.snapshot().state)
                     .arg(pose.fresh())
                     .arg(tracking.fresh())
                     .arg(chassis.isConnected())
                     .arg(tracking.snapshot().pauseReason));
            return;
        }
        const auto status = tracking.snapshot();
        if (phase == 0 && coordinator.coordinatesReady()) {
            coordinator.acquireSession();
            phase = 1;
        } else if (phase == 1 && tracking.hasSession() && pose.fresh()) {
            if (!pose.capture(binding, 300))
                fail("capture not sent");
            else
                phase = 2;
        } else if (phase == 3 && status.state == "Ready" && !tracking.busy() && tracking.motionReady()) {
            coordinator.startTask();
            phase = 4;
        } else if (phase == 4 && status.state == "Tracking" && status.measuredV > .04) {
            chassis.connectToHost();
            coordinator.pauseTask();
            phase = 5;
        } else if (phase == 5 && chassis.isConnected() && status.state == "Paused" && !tracking.busy() && tracking.motionReady()) {
            chassis.sendVelocityCommand(0, 0);
            coordinator.resumeTask();
            phase = 6;
        } else if (phase == 6 && status.state == "Completed") {
            const auto endpoint = pose.snapshot();
            if (std::hypot(endpoint.position.x() - .6, endpoint.position.y()) > .03 ||
                std::abs(status.measuredV) > .02) {
                fail("endpoint not settled");
                return;
            }
            QTextStream(stdout) << "PASS Qt pose/capture/compiler/upload/start/manual "
                                   "independent socket/pause/resume/endpoint/photo acknowledgement"
                                << Qt::endl;
            TaskCompileOptions options;
            options.safetyProfileId = "open";
            options.rotationZoneId = "open";
            const auto next = TaskCompiler::route({{"offline", {{0, .6}, {0, 1.2}}, {}}}, binding, options);
            if (!next.ok() || !coordinator.upload(next.plan)) {
                fail("offline task upload failed");
                return;
            }
            phase = 7;
        } else if (phase == 7 && status.state == "Ready" && !tracking.busy() && tracking.motionReady()) {
            coordinator.startTask();
            phase = 8;
        } else if (phase == 8 && status.state == "Tracking" && status.measuredV > .04) {
            coordinator.shutdown();
            chassis.disconnectFromHost();
            QTextStream(stdout) << "PASS Qt shutdown while autonomous task is running" << Qt::endl;
            app.exit(0);
        }
    });
    pose.start();
    tracking.start();
    poll.start();
    return app.exec();
}

#include <QtWidgets>
#include <optional>
#include "rowworktypes.h"
#include "taskcompiler.h"
#include "jsonhttpclient.h"
#define private public
#include "map.h"
#undef private
#include "ui_mainwindow.h"
#include "configmanager.h"
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
int runNavigationDefaultsProbe(QApplication &app);
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    if (argc > 1) { ConfigManager::instance().setConfigFilePath(argv[1]); ConfigManager::instance().reload(); }
    if (argc > 2 && QString::fromLocal8Bit(argv[2]) == "--navigation-defaults")
        return runNavigationDefaultsProbe(app);
    QMainWindow window;
    Ui::MainWindow ui; ui.setupUi(&window);
    Map map(&ui);
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
    QObject::connect(&events, &ExternalEventCoordinator::manualConfirmationChanged,
                     &map, &Map::setExternalActionReviewEvent);
    QObject::connect(&map, &Map::externalActionResolved, &events,
                     [&events](const QString &id, bool success) { events.resolveCurrent(id, success); });
    MapFrameBinding binding;
    binding.confirmed = true;
    binding.context = {"test-map",       "simulation-origin", "test-transform",
                       "simulation-cal-20261014", "simulation-vehicle-20261014",      1};
    binding.enuTranslation = {10, -4};
    binding.enuYawOffsetRad = M_PI_2;
    map.m_binding = binding;
    map.setControlCoordinator(&coordinator);
    map.addPointInternal(-10, 4, 0, 1);
    map.addPointInternal(-9.4, 4, 0, 2);
    map.addPointInternal(-8.8, 4, 0, 3);
    map.addLinePath(2, 3, 1);
    Map::RouteStep route; route.fromId = 2; route.toId = 3;
    map.m_routeQueue.append(route);
    map.refreshPlanningRevision();
    map.syncCommittedMapState();
    binding = map.m_binding;
    int phase = 0;
    QString originalExecution;
    QElapsedTimer reopen;
    double maxMapModelError = 0, maxSceneError = 0;
    QElapsedTimer deadline;
    deadline.start();
    auto fail = [&](const QString &error) {
        QTextStream(stderr) << "FAIL phase " << phase << ": " << error
            << " | busy=" << tracking.busy() << " actionPending=" << coordinator.actionPending()
            << " editable=" << map.canEditRowWorkPlan() << " queue=" << map.m_routeQueue.size()
            << " notice=" << (map.m_routeStatusLabel ? map.m_routeStatusLabel->text() : QString()) << Qt::endl;
        app.exit(1);
    };
    QObject::connect(&pose, &PoseClient::poseChanged, &app, [&](const ControlPoseSnapshot &p) {
        if (!p.validForControl || !map.m_hasVehiclePose) return;
        // Independent fixture math: with R=90deg,t=(10,-4), map=(east-10,north+4).
        const QPointF expected(p.position.x()-10, p.position.y()+4);
        maxMapModelError = std::max(maxMapModelError, QLineF(expected, {map.m_vehiclePoseX, map.m_vehiclePoseY}).length());
        const QPointF expectedScene(expected.y()*50, expected.x()*50);
        maxSceneError = std::max(maxSceneError, QLineF(expectedScene, map.m_vehicleItem->scenePos()).length()/50);
        if (maxMapModelError > 1e-8 || maxSceneError > .01) fail("pose -> map model/graphics mismatch");
    });
    QTimer confirmDialog;
    QObject::connect(&confirmDialog, &QTimer::timeout, &app, [&] {
        auto *dialog = window.findChild<QDialog*>("remoteTaskReviewDialog");
        if (!dialog || !dialog->isVisible()) return;
        auto *verified = dialog->findChild<QCheckBox*>("remoteTaskVerified");
        if (!verified || !verified->isEnabled()) { dialog->reject(); fail("remote plan confirmation unavailable"); return; }
        verified->setChecked(true);
        dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
    });
    confirmDialog.start(20);
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
        options.applyControllerDefaults(tracking.configuration());
        auto task = TaskCompiler::route({{"out", {{-10, 4}, {-9.4, 4}}, {}}}, binding, options);
        if (!task.ok()) { fail(task.error); return; }
        const auto xy = task.plan["steps"].toArray()[0].toObject()["points"].toArray();
        if (std::abs(xy[0].toArray()[0].toDouble()) > 1e-8 ||
            std::abs(xy[1].toArray()[0].toDouble()-.6) > 1e-8 ||
            std::abs(xy[1].toArray()[1].toDouble()) > 1e-8) { fail("planner ENU points mismatch"); return; }
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
            auto *pause = window.findChild<QPushButton *>("mapTaskPauseButton");
            if (!pause || !pause->isEnabled()) { fail("actual pause button unavailable"); return; }
            pause->click();
            pause->click(); // A rapid second click must not queue another motion intent.
            phase = 5;
        } else if (phase == 5 && chassis.isConnected() && status.state == "Paused" && !tracking.busy() && tracking.motionReady()) {
            chassis.sendVelocityCommand(0, 0);
            originalExecution = status.executionId;
            tracking.stop(); pose.stop();
            tracking.start(); pose.start(); reopen.start(); phase = 9;
        } else if (phase == 9 && reopen.elapsed() > 1300 && coordinator.coordinatesReady()) {
            coordinator.acquireSession(); phase = 10;
        } else if (phase == 10 && tracking.hasSession() && tracking.motionReady()) {
            if (status.executionId != originalExecution || !tracking.preparedPlan().isEmpty()) { fail("client reopen changed execution"); return; }
            auto *resume = window.findChild<QPushButton *>("mapTaskResumeButton");
            if (!resume || !resume->isEnabled()) { fail("reopen review button unavailable"); return; }
            phase = 11;
            resume->click(); // First click reads and reviews; it must not resume motion.
            resume->click(); // Repeated clicks while reading must not queue motion.
        } else if (phase == 11 && !tracking.preparedPlan().isEmpty() && tracking.motionReady()) {
            if (status.state != "Paused" || status.executionId != originalExecution ||
                std::abs(status.measuredV) > .02) { fail("task review resumed motion"); return; }
            const auto revision = map.m_binding.context.mapRevision;
            map.handleRotationSpinChanged(30);
            auto *resume = window.findChild<QPushButton *>("mapTaskResumeButton");
            if (!resume || !resume->isEnabled()) { fail("actual resume button unavailable"); return; }
            resume->click();
            if (revision != map.m_binding.context.mapRevision) { fail("view rotation invalidated plan"); return; }
            QTextStream(stdout) << "PASS client reopen, remote plan preview/explicit confirmation, same execution resume" << Qt::endl;
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
            QTextStream(stdout) << "PASS pose -> actual map, model error=" << maxMapModelError
                                << "m displayed error=" << maxSceneError << "m" << Qt::endl;
            phase = 12;
        } else if (phase == 12 && status.state == "Completed" && tracking.commandFresh() &&
                   !tracking.busy() && !coordinator.actionPending() && map.m_routeStartButton->isEnabled()) {
            // Completion can arrive before the photo ack's command-result query.
            // Match an actual enabled UI click instead of bypassing its guard and
            // silently consuming the test's one dispatch while that query is pending.
            map.m_routeStartButton->click();
            if (!coordinator.actionPending() && !tracking.busy()) {
                fail("enabled route execution action did not queue a request");
                return;
            }
            phase = 7;
        } else if (phase == 7 && status.state == "Tracking" && status.measuredV > .04) {
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

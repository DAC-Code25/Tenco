#include <QtWidgets>
#include <QtNetwork>
#include <QtTest>
#include <optional>
#include <memory>
#include "rowworktypes.h"
#include "taskcompiler.h"
#include "mapgeometry.h"
#include "jsonhttpclient.h"
// Exercise the actual page and graphics items without a device or test-only production API.
#define private public
#include "map.h"
#include "trackingclient.h"
#include "poseclient.h"
#undef private
#include "controlsessioncoordinator.h"
#include "ui_mainwindow.h"
#include "mapgraphicsview.h"

static MapFrameBinding testBinding() {
    MapFrameBinding b;
    b.confirmed = true;
    b.context = {"map", "origin", "transform", "cal", "profile", 1};
    return b;
}

class MapCaptureFixture : public QTcpServer {
public:
    int requests = 0;
    QPointer<QTcpSocket> pending;
    QJsonObject body;
    MapCaptureFixture() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                buffer->append(socket->readAll());
                const auto end = buffer->indexOf("\r\n\r\n");
                if (end < 0) return;
                int length = 0;
                for (const auto &line : buffer->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                if (buffer->size() < end + 4 + length) return;
                socket->disconnect(this);
                body = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
                pending = socket;
                ++requests;
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    void reply(const QString &state = QStringLiteral("succeeded"), const QString &boot = QStringLiteral("fusion")) {
        if (!pending) return;
        const auto payload = QJsonDocument(QJsonObject{
            {"apiVersion", 1}, {"fusionBootId", boot}, {"state", state},
            {"originRevision", "origin"}, {"calibrationId", "cal"},
            {"sampleCount", 12}, {"positionScatterMeters", .004},
            {"reason", state == "succeeded" ? "" : "capture_scatter_exceeded"},
            {"pose", QJsonObject{{"x", 1.123456789}, {"y", -2.234567891}, {"yaw", .345678912}}}
        }).toJson(QJsonDocument::Compact);
        pending->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                       QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
        pending->disconnectFromHost();
        pending.clear();
    }
    static void seed(PoseClient &pose) {
        pose.m_running = true;
        pose.m_pose.bootId = "fusion";
        pose.m_pose.validForControl = true;
        pose.m_pose.originRevision = "origin";
        pose.m_pose.calibrationId = "cal";
        pose.m_pose.stateAgeMs = 0;
        pose.m_age.start();
    }
};

class MapPoseStreamFixture : public QTcpServer {
public:
    int streams = 0, captures = 0, nextStreamStatus = 200;
    QPointer<QTcpSocket> stream, capture;
    QElapsedTimer clock;
    quint64 sequence = 0;
    MapPoseStreamFixture() {
        clock.start();
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto *socket = nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const auto end = buffer->indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    int length = 0;
                    for (const auto &line : buffer->left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    if (buffer->size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    if (buffer->left(end).split(' ').value(1).endsWith("/pose/stream")) {
                        ++streams;
                        const int status = nextStreamStatus;
                        nextStreamStatus = 200;
                        if (status != 200) {
                            socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            socket->disconnectFromHost();
                            return;
                        }
                        stream = socket;
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                                      "Transfer-Encoding: chunked\r\n\r\n");
                    } else {
                        capture = socket;
                        ++captures;
                    }
                });
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1").arg(serverPort())); }
    void writeLine(const QByteArray &line) {
        if (stream) stream->write(QByteArray::number(line.size(), 16) + "\r\n" + line + "\r\n");
    }
    void publish(bool valid = true) {
        writeLine(QJsonDocument(QJsonObject{
            {"apiVersion", 1}, {"seq", QString::number(++sequence)}, {"bootId", "fusion"},
            {"frameId", "map"}, {"baseFrameId", "base_link"}, {"originRevision", "origin"},
            {"calibrationId", "cal"}, {"freshnessBasis", "http_receipt"},
            {"sendMonotonicSeconds", clock.elapsed() / 1000.0}, {"stateAgeMs", 0},
            {"positionObservationAgeMs", 0}, {"headingObservationAgeMs", 0},
            {"pose", QJsonObject{{"x", 0}, {"y", 0}, {"yaw", 0}}},
            {"estimatedTwist", QJsonObject{{"v", 0}, {"omega", 0}}}, {"validForControl", valid}
        }).toJson(QJsonDocument::Compact) + '\n');
    }
    void disconnectStream() {
        if (stream) stream->disconnectFromHost();
        stream.clear();
    }
};

class PoseMapChainTest : public QObject {
    Q_OBJECT
private slots:
    void localizationTrailKeepsReceivedCoordinatesWithRotatedView_data() {
        QTest::addColumn<double>("bindingYaw");
        QTest::addColumn<double>("viewRotation");
        QTest::newRow("identity") << 0.0 << 0.0;
        QTest::newRow("rotated-display") << 0.0 << -52.3;
        QTest::newRow("transformed-display") << .73 << 107.0;
    }
    void localizationTrailKeepsReceivedCoordinatesWithRotatedView() {
        QFETCH(double, bindingYaw); QFETCH(double, viewRotation);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.setControlCoordinator(&coordinator);
        map.m_binding = testBinding(); map.m_binding.enuTranslation = {7, -3};
        map.m_binding.enuYawOffsetRad = bindingYaw;
        map.handleRotationSpinChanged(viewRotation);
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.startPose = {0, 0, 0}; map.m_rowWorkPlan.endPose = {0, 10, 0};
        map.refreshRowWorkGraphics();
        auto content = [&map] {
            auto document = map.serializeMap();
            document.remove("savedAt"); // Saving time may cross a second during queued painting.
            return document;
        };
        const auto document = content();
        const MapFrameAdapter adapter(map.m_binding);
        ControlPoseSnapshot sample;
        sample.bootId = "fusion"; sample.originRevision = "origin"; sample.calibrationId = "cal";
        sample.validForControl = true; sample.yaw = .4;
        const QList<QPointF> received{{8.125, -1.75}, {8.85, -1.1}, {9.2, -1.55}};
        for (const auto &point : received) {
            sample.position = point; ++sample.seq;
            emit pose.poseChanged(sample);
        }
        QCOMPARE(map.m_localizationTrail.size(), received.size());
        QTRY_COMPARE(map.m_localizationTrailItem->path().elementCount(), received.size());
        const auto path = map.m_localizationTrailItem->path();
        QCOMPARE(path.elementCount(), received.size());
        for (int i = 0; i < received.size(); ++i) {
            QCOMPARE(map.m_localizationTrail[i].enuPosition, received[i]);
            QCOMPARE(map.m_localizationTrail[i].yaw, .4);
            const auto expected = map.mapToScene(adapter.fromEnu(received[i]));
            QVERIFY(QLineF(QPointF(path.elementAt(i).x, path.elementAt(i).y), expected).length() < 1e-9);
            const auto shown = map.m_view->viewportTransform().map(expected);
            const auto recovered = map.m_view->viewportTransform().inverted().map(shown);
            QVERIFY(QLineF(recovered, expected).length() < 1e-9);
        }
        QCOMPARE(content(), document); // Monitoring never edits the saved plan.
        QVERIFY(map.m_localizationTrailItem->pen().color() != map.m_rowWorkLineItem->pen().color());
        auto *clear = ui.mapPage->findChild<QPushButton *>("mapClearLocalizationTrailButton");
        QVERIFY(clear); clear->click();
        QVERIFY(map.m_localizationTrail.isEmpty());
        QVERIFY(map.m_localizationTrailItem->path().isEmpty());
        QCOMPARE(content(), document);
    }
    void localizationTrailBreaksAtUnavailablePoseAndProducerChanges() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.setControlCoordinator(&coordinator); map.m_binding = testBinding();
        ControlPoseSnapshot sample;
        sample.bootId = "first"; sample.originRevision = "origin"; sample.calibrationId = "cal";
        sample.validForControl = true;
        auto publish = [&](QPointF point) { sample.position = point; ++sample.seq; emit pose.poseChanged(sample); };
        publish({0, 0}); publish({1, .2});
        emit pose.availabilityChanged(false, "lost");
        publish({2, .5}); publish({3, .7});
        sample.validForControl = false; publish({4, .9});
        sample.validForControl = true; publish({5, 1});
        sample.bootId = "second"; sample.seq = 0; publish({6, 1.3}); publish({7, 1.5});
        QCOMPARE(map.m_localizationTrail.size(), 7);
        QTRY_COMPARE(map.m_localizationTrailItem->path().elementCount(), 6);
        const auto path = map.m_localizationTrailItem->path();
        // Qt drops an isolated moveTo when the next sample starts another
        // segment; the original sample stays in history without a false line.
        QCOMPARE(path.elementCount(), 6);
        const QList<QPair<QPointF, QPointF>> segments{
            {{0, 0}, {1, .2}}, {{2, .5}, {3, .7}}, {{6, 1.3}, {7, 1.5}}};
        const MapFrameAdapter adapter(map.m_binding);
        for (int i = 0; i < segments.size(); ++i) {
            const auto from = path.elementAt(i * 2), to = path.elementAt(i * 2 + 1);
            QCOMPARE(from.type, QPainterPath::MoveToElement);
            QCOMPARE(to.type, QPainterPath::LineToElement);
            QCOMPARE(QPointF(from.x, from.y), map.mapToScene(adapter.fromEnu(segments[i].first)));
            QCOMPARE(QPointF(to.x, to.y), map.mapToScene(adapter.fromEnu(segments[i].second)));
        }
        sample.originRevision = "other"; publish({8, 1.7});
        QCOMPARE(map.m_localizationTrail.size(), 7);
        map.m_binding.context.originRevision = "other";
        publish({9, 1.9});
        QCOMPARE(map.m_localizationTrail.size(), 1);
        QVERIFY(map.m_localizationTrail.front().startsSegment);
        map.m_binding.enuTranslation = {1, 0};
        map.applyPresentationUpdates(false);
        QVERIFY(map.m_localizationTrail.isEmpty());
    }
    void localizationTrailBoundsRecentSamplesAndClearsOnMapLoad() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding();
        ControlPoseSnapshot sample;
        sample.bootId = "fusion"; sample.validForControl = true; sample.seq = 1;
        map.appendLocalizationTrail(sample);
        while (map.m_localizationTrail.size() < Map::kMaxLocalizationTrailSamples)
            map.m_localizationTrail.append({{1, 1}, .2, false});
        sample.position = {2, 3}; ++sample.seq;
        map.appendLocalizationTrail(sample);
        QCOMPARE(map.m_localizationTrail.size(), Map::kMaxLocalizationTrailSamples);
        QVERIFY(map.m_localizationTrail.front().startsSegment);
        QCOMPARE(map.m_localizationTrail.back().enuPosition, QPointF(2, 3));
        const auto saved = map.serializeMap();
        QVERIFY(map.deserializeMap(saved));
        QVERIFY(map.m_localizationTrail.isEmpty());
        QVERIFY(map.m_localizationTrailItem->path().isEmpty());
    }
    void localizationTrailCoalescesPaintWithoutDroppingSamples() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding();
        ControlPoseSnapshot sample;
        sample.bootId = "fusion"; sample.validForControl = true;
        for (int i = 0; i < 100; ++i) {
            sample.seq = i + 1; sample.position = {i * .01, i * .003};
            map.appendLocalizationTrail(sample);
        }
        QCOMPARE(map.m_localizationTrail.size(), 100);
        QVERIFY(map.m_localizationTrailRefreshTimer->isActive());
        QVERIFY(map.m_localizationTrailItem->path().isEmpty());
        QTRY_COMPARE(map.m_localizationTrailItem->path().elementCount(), 100);
        QVERIFY(!map.m_localizationTrailRefreshTimer->isActive());
        const auto last = map.m_localizationTrailItem->path().currentPosition();
        QCOMPARE(last, map.mapToScene(MapFrameAdapter(map.m_binding).fromEnu(sample.position)));
        map.clearLocalizationTrail();
        QVERIFY(!map.m_localizationTrailRefreshTimer->isActive());
        QVERIFY(map.m_localizationTrailItem->path().isEmpty());
    }
    void recordedRowGraphicsAndCompiledReturnKeepTheSameTaughtVertices() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding(); map.m_binding.enuTranslation = {4, -7};
        map.m_binding.enuYawOffsetRad = .43;
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        const MapFrameAdapter adapter(map.m_binding);
        auto capture = [&](Map::RowWorkPendingCaptureTarget target, const QPointF &point) {
            const auto enu = adapter.toEnu(point);
            map.m_rowWorkPendingCaptureTarget = target;
            map.applyCapture({{"state", "succeeded"}, {"originRevision", "origin"}, {"calibrationId", "cal"},
                {"pose", QJsonObject{{"x", enu.x()}, {"y", enu.y()}, {"yaw", .7}}}});
        };
        const QList<QPointF> vertices{{0, 0}, {.3, 2}, {-.2, 6}, {.1, 8}, {0, 10}};
        capture(Map::RowWorkPendingCaptureTarget::StartPose, vertices.front());
        for (int i = 1; i + 1 < vertices.size(); ++i)
            capture(Map::RowWorkPendingCaptureTarget::CheckpointFromVehicle, vertices[i]);
        capture(Map::RowWorkPendingCaptureTarget::EndPose, vertices.back());
        QVERIFY(map.m_rowWorkPlan.isValid());
        map.m_rowWorkPlan.checkpoints[1].enabled = false; // Geometry survives a disabled action.
        map.refreshRowWorkUi(); map.handleRotationSpinChanged(-52.3);
        QVERIFY(map.m_rowWorkPlanLabel->text().contains(QStringLiteral("沿记录点行驶")));
        QVERIFY(!map.m_rowWorkPlanLabel->text().contains(QStringLiteral("坐标缺失")));
        const auto original = RowWorkJson::planToJson(map.m_rowWorkPlan);
        auto *table = map.m_rowWorkCheckpointTable;
        QVERIFY(!(table->item(0, 1)->flags() & Qt::ItemIsEditable));
        table->item(0, 1)->setText("3.0"); // Even a programmatic edit cannot detach station from XY.
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), original);
        const auto path = map.m_rowWorkLineItem->path();
        QCOMPARE(path.elementCount(), vertices.size());
        for (int i = 0; i < vertices.size(); ++i)
            QVERIFY(QLineF(QPointF(path.elementAt(i).x, path.elementAt(i).y), map.mapToScene(vertices[i])).length() < 1e-8);
        for (int i = 0; i < map.m_rowWorkCheckpointMarkers.size(); ++i)
            QVERIFY(QLineF(map.m_rowWorkCheckpointMarkers[i]->scenePos(), map.mapToScene(vertices[i + 1])).length() < 1e-8);
        QVERIFY(map.m_rowWorkLineItem->childItems().isEmpty());
        TaskCompileOptions options; options.safetyProfileId = "open"; options.rotationZoneId = "open";
        options.speedLimit = .2; options.goalToleranceMeters = .1;
        options.angularSpeedLimit = .2; options.angleToleranceRad = .05; options.turnTimeoutMs = 30000;
        const auto compiled = TaskCompiler::row(map.m_rowWorkPlan, map.m_binding, options, false, true);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto reverse = compiled.plan["steps"].toArray().first().toObject()["points"].toArray();
        QCOMPARE(reverse.size(), vertices.size());
        for (int i = 0; i < reverse.size(); ++i) {
            const auto xy = reverse[i].toArray();
            QVERIFY(QLineF(QPointF(xy[0].toDouble(), xy[1].toDouble()), adapter.toEnu(vertices[vertices.size() - 1 - i])).length() < 1e-8);
        }
        capture(Map::RowWorkPendingCaptureTarget::EndPose, {0, 5});
        QCOMPARE(RowWorkJson::planToJson(map.m_rowWorkPlan), original);
        QCOMPARE(map.m_rowWorkLineItem->path(), path);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("taught-bends.json");
        QVERIFY(map.saveMapToFile(file)); QVERIFY(map.loadMapFromFile(file));
        QCOMPARE(map.m_rowWorkLineItem->path(), path);
        for (int i = 0; i < map.m_rowWorkPlan.checkpoints.size(); ++i) {
            QVERIFY(map.m_rowWorkPlan.checkpoints[i].recordedMapPosition);
            QVERIFY(QLineF(*map.m_rowWorkPlan.checkpoints[i].recordedMapPosition, vertices[i + 1]).length() < 1e-8);
        }
    }
    void legacyRowShowsMissingRecordedPositionsWithoutRewritingMap() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding();
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.startPose = {0, 0, 0}; map.m_rowWorkPlan.endPose = {0, 10, 0};
        map.m_rowWorkPlan.checkpoints = {{"P1", 2, 2000}, {"P2", 7, 2000}};
        const auto original = map.serializeMap();
        map.refreshRowWorkUi();
        QVERIFY(map.m_rowWorkPlanLabel->text().contains(QStringLiteral("旧地图仅保存直线站距")));
        QVERIFY(map.m_rowWorkPlanLabel->text().contains(QStringLiteral("需重新采样")));
        QCOMPARE(map.m_rowWorkLineItem->path().elementCount(), 2);
        QVERIFY(map.m_rowWorkCheckpointTable->item(0, 1)->flags() & Qt::ItemIsEditable);
        QCOMPARE(map.serializeMap(), original);
    }
    void returnToStartPresentationKeepsPauseFaultAndDelayVisible() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.m_running = true; tracking.m_statusUsable = true; tracking.m_statusAge.start();
        tracking.m_status.bootId = "boot"; tracking.m_status.state = "Tracking";
        tracking.m_status.taskId = "task"; tracking.m_status.returningToStart = true;
        map.setControlCoordinator(&coordinator);
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(map.m_trackingStatusLabel->text().contains(QStringLiteral("正在返回起点")));
        tracking.m_status.state = "Paused"; tracking.m_status.pauseReason = "operator_pause";
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(map.m_trackingStatusLabel->text().contains(QStringLiteral("已暂停")));
        QVERIFY(!map.m_trackingStatusLabel->text().contains(QStringLiteral("正在返回")));
        tracking.m_status.state = "Fault"; tracking.m_status.faultCode = "chassis_feedback_lost";
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(map.m_trackingStatusLabel->text().contains(QStringLiteral("故障")));
        QVERIFY(map.m_taskProgressLabel->text().contains(QStringLiteral("底盘反馈中断")));
        tracking.m_status.state = "Tracking"; tracking.m_statusAge.invalidate();
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(map.m_trackingStatusLabel->text().contains(QStringLiteral("状态延迟")));
        tracking.m_running = false;
    }
    void taskMotionSettingsComeOnlyFromAppliedControllerConfiguration() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        const QList<TaskRouteSection> sections{{"route", {{0, 0}, {1, 0}}, {}}};
        auto compiled = TaskCompiler::route(sections, testBinding(), map.taskOptions());
        QVERIFY(!compiled.ok());
        QVERIFY(compiled.error.contains(QStringLiteral("工控机")));

        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.m_configuration = {{"navigation_defaults", QJsonObject{
            {"rotation_policy", "vehicle_footprint"}, {"default_safety_profile", "field-low-speed"},
            {"task_parameters_version", 1}, {"task_speed_mps", .17},
            {"task_turn_speed_radps", .19}, {"goal_tolerance_m", .024},
            {"angle_tolerance_rad", .041}, {"task_turn_timeout_ms", 43000}}}};
        map.setControlCoordinator(&coordinator);
        const auto options = map.taskOptions();
        QCOMPARE(options.speedLimit, .17);
        QCOMPARE(options.angularSpeedLimit, .19);
        QCOMPARE(options.goalToleranceMeters, .024);
        QCOMPARE(options.angleToleranceRad, .041);
        QCOMPARE(options.turnTimeoutMs, 43000);
        compiled = TaskCompiler::route(sections, testBinding(), options);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        QCOMPARE(compiled.plan["steps"].toArray()[0].toObject()["speedLimit"].toDouble(), .17);
        auto defaults = tracking.m_configuration["navigation_defaults"].toObject();
        defaults.remove("goal_tolerance_m");
        tracking.m_configuration["navigation_defaults"] = defaults;
        QVERIFY(!TaskCompiler::route(sections, testBinding(), map.taskOptions()).ok());
    }
    void routeCycleWaitCannotExceedTheCompiledStepLimit() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        MapCaptureFixture server;
        QVERIFY(server.isListening());
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.configure(server.url(), {});
        tracking.m_configuration = {{"profile_revision", "profile"},
            {"calibration", QJsonObject{{"calibrationId", "cal"}}},
            {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
            {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"},
                {"default_safety_profile", "field-low-speed"}, {"task_parameters_version", 1},
                {"task_speed_mps", .1}, {"task_turn_speed_radps", .2}, {"goal_tolerance_m", .03},
                {"angle_tolerance_rad", .05}, {"task_turn_timeout_ms", 30000}}}};
        map.m_binding = testBinding();
        map.m_newMapMayAutoBind = false;
        map.setControlCoordinator(&coordinator);
        map.m_routeLoopCheck->setChecked(true);
        map.m_routeLoopIntervalSpin->setValue(1);

        // A closed directed line has 333 stopped legs: 166 outbound and 167
        // returning. Each has path/heading/dwell, plus one turnaround = 1000.
        const MapFrameAdapter frame(map.m_binding);
        constexpr int turnaroundId = 167;
        for (int id = 1; id <= 333; ++id) {
            const double x = id <= turnaroundId ? id - 1.0
                : 166.0 * (334 - id) / 167.0;
            const auto point = frame.fromEnu({x, 0});
            const double yaw = frame.yawFromEnu(id > 1 && id <= turnaroundId ? 0 : std::acos(-1.));
            QVERIFY(map.addPointInternal(point.x(), point.y(), yaw, id, 2000));
        }
        for (int id = 1; id <= 333; ++id)
            QVERIFY(map.addLinePath(id, id == 333 ? 1 : id + 1, id));
        map.m_routeQueue = {{1, turnaroundId, {}}, {turnaroundId, 1, {}}};
        map.refreshPlanningRevision();
        QString error;
        const auto sections = map.buildRouteSections(&error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(sections.size(), 333);
        auto options = map.taskOptions();
        options.repeatUntilStopped = true;
        const auto compiled = TaskCompiler::route(sections, map.m_binding, options);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        QCOMPARE(compiled.plan["steps"].toArray().size(), 1000);

        // Make every ordinary submit gate eligible, so an early configuration
        // or state rejection cannot conceal a missing post-compile size guard.
        tracking.m_running = true;
        tracking.m_statusUsable = true;
        tracking.m_statusAge.start();
        tracking.m_status.bootId = "boot";
        tracking.m_status.state = "Idle";
        QVERIFY(tracking.commandFresh());
        QVERIFY(coordinator.coordinatesReady());
        QVERIFY(map.canEditRowWorkPlan());
        QSignalSpy actionChanges(&coordinator, &ControlSessionCoordinator::actionPendingChanged);
        QSignalSpy pendingChanges(&tracking, &TrackingClient::pendingChanged);
        map.submitRouteTask();
        QVERIFY(!coordinator.actionPending());
        QCOMPARE(actionChanges.count(), 0);
        QVERIFY(!tracking.busy());
        QCOMPARE(pendingChanges.count(), 0);
        QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("1000")));
        QTest::qWait(20);
        QCOMPARE(server.requests, 0);
        tracking.m_running = false;
    }
    void staleMonitoringAndBusyWriteDoNotDisableStopsForKnownExecution() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.configure(QUrl("http://127.0.0.1:1/api/v1"), {});
        tracking.m_running = true;
        tracking.m_status = {};
        tracking.m_status.bootId = "boot";
        tracking.m_status.taskId = "task";
        tracking.m_status.taskRevision = 1;
        tracking.m_status.executionId = "execution";
        tracking.m_status.state = "Turning";
        // Deliberately no current monitor snapshot or session, plus an unrelated write.
        tracking.m_pending.insert("prepare", {"prepare", {}, "prepare", "session"});
        map.setControlCoordinator(&coordinator);
        map.updateRouteControlState(); map.refreshRowWorkControlState();
        QVERIFY(!tracking.commandFresh()); QVERIFY(tracking.busy());
        QVERIFY(map.m_taskPauseButton->isEnabled()); QVERIFY(map.m_taskStopButton->isEnabled());
        QVERIFY(!map.m_taskResumeButton->isEnabled()); QVERIFY(!map.m_routeStartButton->isEnabled());
        tracking.m_status.state = "Paused";
        tracking.m_status.autoResumePending = true;
        map.updateRouteControlState();
        QVERIFY(map.m_taskPauseButton->isEnabled()); // Convert a sensor hold into an explicit manual hold.
        tracking.m_status.autoResumePending = false;
        map.updateRouteControlState();
        QVERIFY(!map.m_taskPauseButton->isEnabled());
        tracking.m_status.executionId.clear();
        map.updateRouteControlState(); map.refreshRowWorkControlState();
        QVERIFY(!map.m_taskPauseButton->isEnabled()); QVERIFY(!map.m_taskStopButton->isEnabled());
        tracking.m_pending.clear(); tracking.m_running = false;
    }
    void taskButtonsRefreshAfterCommandResultAndExposeFaultReset() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.configure(QUrl("http://127.0.0.1:1/api/v1"), {});
        tracking.m_running = true;
        tracking.m_statusUsable = true;
        tracking.m_statusAge.start();
        tracking.m_status.bootId = "boot";
        tracking.m_status.taskId = "task";
        tracking.m_status.taskRevision = 1;
        tracking.m_status.executionId = "execution";
        tracking.m_status.state = "Paused";
        tracking.m_status.chassisConnected = true;
        tracking.m_status.measuredValid = true;
        tracking.m_status.measuredStopped = true;
        tracking.m_pending.insert("pause", {"pause", {}, "pause", "safety", 0, 0, true, false});
        map.setControlCoordinator(&coordinator);
        map.updateRouteControlState();
        QVERIFY(!map.m_taskResumeButton->isEnabled());
        tracking.m_pending.clear();
        emit tracking.pendingChanged(false);
        QVERIFY(map.m_taskResumeButton->isEnabled());
        QVERIFY(!map.m_taskResetButton->isEnabled());

        tracking.m_status.state = "Fault";
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(!map.m_taskResumeButton->isEnabled());
        QVERIFY(map.m_taskResetButton->isVisibleTo(map.m_mapPage));
        QVERIFY(map.m_taskResetButton->isEnabled());
        QVERIFY(map.m_taskProgressLabel->text().contains(QStringLiteral("复位")));
        tracking.m_status.measuredValid = false;
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(!map.m_taskResetButton->isEnabled());
        tracking.m_status.state = "Idle";
        map.applyTrackingStatus(tracking.m_status);
        QVERIFY(!map.m_taskResumeButton->isEnabled());
        QVERIFY(!map.m_taskResetButton->isEnabled());
        tracking.m_running = false;
    }
    void updatingPointDoesNotRoundUntouchedCoordinates() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        const QPointF original(-2.123456789, 3.987654321);
        QVERIFY(map.addPointInternal(original.x(), original.y(), .123456789, 1));
        map.refreshPointUi();
        const double yaw = map.pointById(1)->theta;
        map.m_pointTable->selectRow(0);
        map.handlePointSelectionChanged();
        map.handleUpdatePoint();
        QCOMPARE(map.pointById(1)->mapPosition, original);
        QCOMPARE(map.pointById(1)->theta, yaw);
        map.m_pointXSpin->setValue(-1.111);
        map.handleUpdatePoint();
        QCOMPARE(map.pointById(1)->mapPosition.x(), -1.111);
        QCOMPARE(map.pointById(1)->mapPosition.y(), original.y());
        QCOMPARE(map.pointById(1)->theta, yaw);
    }
    void editingCheckpointMetadataKeepsGeometryPrecision() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.startPose = {0, 0, 0}; map.m_rowWorkPlan.endPose = {4, 0, 0};
        RowCheckpoint checkpoint; checkpoint.name = "P1"; checkpoint.progress = 1.23456789;
        map.m_rowWorkPlan.checkpoints = {checkpoint}; map.refreshRowWorkUi();
        map.m_rowWorkCheckpointTable->item(0, 2)->setText("1234");
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, checkpoint.progress);
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].dwellMs, 1234);
        map.m_rowWorkCheckpointTable->item(0, 1)->setText("2.34");
        QCOMPARE(map.m_rowWorkPlan.checkpoints[0].progress, 2.34);
    }
    void clickTeachSaveLoadCompilePreservesEnu_data() {
        QTest::addColumn<double>("bindingYaw");
        QTest::addColumn<double>("displayRotation");
        QTest::newRow("identity-rotated-view") << 0.0 << 37.7;
        QTest::newRow("transformed-negative-view") << .731 << -123.4;
        QTest::newRow("transformed-pi-crossing") << -2.4 << 90.0;
    }
    void clickTeachSaveLoadCompilePreservesEnu() {
        QFETCH(double, bindingYaw); QFETCH(double, displayRotation);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        auto binding = testBinding(); binding.enuTranslation = {12.3456789, -6.7891234};
        binding.enuYawOffsetRad = bindingYaw;
        map.m_binding = binding;
        MapFrameAdapter frame(binding);
        const QPointF start(-1.234567891, 4.987654321), end = start + QPointF(2.3, .8);
        const double yaw = -2.987654321;
        map.handleRotationSpinChanged(displayRotation);
        int id = 1;
        for (const auto &enu : {start, end}) {
            const QPointF local = frame.fromEnu(enu), scene = map.mapToScene(local);
            const auto viewport = map.m_view->viewportTransform().map(scene);
            const auto back = map.m_view->viewportTransform().inverted().map(viewport);
            QVERIFY(QLineF(scene, back).length() < 1e-10);
            map.m_waitingForClickPlacement = true;
            map.handleSceneClick(back, Qt::LeftButton, Qt::NoModifier);
            QVERIFY(map.pointById(id));
            QVERIFY(QLineF(frame.toEnu(map.pointById(id)->mapPosition), enu).length() < 1e-10);
            ++id;
        }
        Map::MapPath path; path.id = 1; path.startId = 1; path.endId = 2;
        path.polyline = {map.pointById(1)->mapPosition, map.pointById(2)->mapPosition};
        QVERIFY(map.addPathInternal(std::move(path)));
        auto capture = [&](Map::RowWorkPendingCaptureTarget target, const QPointF &position) {
            map.m_rowWorkPendingCaptureTarget = target;
            map.applyCapture({{"state", "succeeded"}, {"originRevision", "origin"}, {"calibrationId", "cal"},
                {"pose", QJsonObject{{"x", position.x()}, {"y", position.y()}, {"yaw", yaw}}}});
        };
        capture(Map::RowWorkPendingCaptureTarget::StartPose, start);
        capture(Map::RowWorkPendingCaptureTarget::EndPose, end);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("roundtrip.json");
        QVERIFY(map.saveMapToFile(file));
        binding = map.m_binding; // Saving commits the current planning revision.
        QVERIFY(map.loadMapFromFile(file));
        QVERIFY(map.m_binding.isUsable());
        QCOMPARE(map.m_binding.toJson(), binding.toJson());
        QVERIFY(QLineF(frame.toEnu(map.m_rowWorkPlan.startPose.toPointF()), start).length() < 1e-10);
        QVERIFY(QLineF(frame.toEnu(map.m_rowWorkPlan.endPose.toPointF()), end).length() < 1e-10);
        QVERIFY(std::abs(MapGeometry::normalizeAngle(frame.yawToEnu(map.m_rowWorkPlan.startPose.yaw) - yaw)) < 1e-10);
        TaskCompileOptions options; options.safetyProfileId = "open"; options.rotationZoneId = "open";
        options.speedLimit = .1; options.goalToleranceMeters = .03;
        options.angularSpeedLimit = .35; options.angleToleranceRad = .05; options.turnTimeoutMs = 30000;
        const auto route = TaskCompiler::route({{"route", map.composePolyline({1}), {}}}, binding, options);
        const auto row = TaskCompiler::row(map.m_rowWorkPlan, binding, options, false);
        for (const auto &compiled : {route, row}) {
            QVERIFY2(compiled.ok(), qPrintable(compiled.error));
            const auto points = compiled.plan["steps"].toArray()[0].toObject()["points"].toArray();
            const auto first = points.first().toArray(), last = points.last().toArray();
            QVERIFY(QLineF({first[0].toDouble(), first[1].toDouble()}, start).length() < 1e-10);
            QVERIFY(QLineF({last[0].toDouble(), last[1].toDouble()}, end).length() < 1e-10);
        }
    }
    void idleMapRefreshesOnlyControllerRevisionWithSameCoordinates() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.m_binding.enuTranslation = {10, -4};
        map.m_binding.enuYawOffsetRad = .7;
        map.m_rowWorkPlan.frameBinding = map.m_binding;
        const auto previous = map.m_binding;
        tracking.m_running = true;
        tracking.m_statusUsable = true;
        tracking.m_statusAge.start();
        tracking.m_status.state = "Idle";
        tracking.m_configuration = {{"profile_revision", "new-profile"},
            {"calibration", QJsonObject{{"calibrationId", "cal"}}},
            {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
            {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"}}}};
        map.setControlCoordinator(&coordinator);
        QCOMPARE(map.m_binding.context.controllerProfileRevision, "new-profile");
        QCOMPARE(map.m_rowWorkPlan.frameBinding.context.controllerProfileRevision, "new-profile");
        QCOMPARE(map.m_binding.context.originRevision, previous.context.originRevision);
        QCOMPARE(map.m_binding.context.frameTransformRevision, previous.context.frameTransformRevision);
        QCOMPARE(map.m_binding.enuTranslation, previous.enuTranslation);
        QCOMPARE(map.m_binding.enuYawOffsetRad, previous.enuYawOffsetRad);
        auto savedMap = map.serializeMap();
        savedMap["frameBinding"] = previous.toJson();
        QVERIFY(map.deserializeMap(savedMap));
        QCOMPARE(map.m_binding.context.controllerProfileRevision, "new-profile");
        tracking.m_status.state = "Ready";
        tracking.m_preparedPlan = {{"taskId", "task"}};
        tracking.m_configuration["profile_revision"] = "another-profile";
        emit tracking.configurationChanged(tracking.m_configuration);
        QCOMPARE(map.m_binding.context.controllerProfileRevision, "new-profile");
        tracking.m_status.state = "Idle";
        tracking.invalidatePreparedPlan();
        emit tracking.configurationChanged(tracking.m_configuration);
        QCOMPARE(map.m_binding.context.controllerProfileRevision, "another-profile");
        tracking.m_configuration["profile_revision"] = "do-not-bind";
        tracking.m_configuration["origin"] = QJsonObject{{"active", QJsonObject{{"originRevision", "different-origin"}}}};
        emit tracking.configurationChanged(tracking.m_configuration);
        QCOMPARE(map.m_binding.context.controllerProfileRevision, "another-profile");
    }
    void onlyNewEmptyMapUsesCurrentControllerOriginAutomatically() {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.m_running = true;
        tracking.m_statusUsable = true;
        tracking.m_statusAge.start();
        tracking.m_configuration = {{"profile_revision", "profile"},
            {"calibration", QJsonObject{{"calibrationId", "cal"}}},
            {"origin", QJsonObject{{"active", QJsonObject{{"originRevision", "origin"}}}}},
            {"navigation_defaults", QJsonObject{{"rotation_policy", "vehicle_footprint"},
                                               {"default_safety_profile", "field-low-speed"},
                                               {"task_parameters_version", 1}, {"task_speed_mps", .1},
                                               {"task_turn_speed_radps", .2}, {"goal_tolerance_m", .03},
                                               {"angle_tolerance_rad", .05}, {"task_turn_timeout_ms", 30000}}},
            {"control", QJsonObject{{"max_angular_speed_radps", .2}}}};
        map.setControlCoordinator(&coordinator);
        QVERIFY(map.m_binding.isUsable());
        QCOMPARE(map.m_binding.context.originRevision, "origin");
        QCOMPARE(map.m_binding.enuTranslation, QPointF());
        QCOMPARE(map.m_binding.enuYawOffsetRad, 0.0);
        QCOMPARE(map.taskOptions().safetyProfileId, "field-low-speed");
        QVERIFY(map.taskOptions().vehicleFootprintRotation);
        QCOMPARE(map.taskOptions().angularSpeedLimit, .2);
        const auto bound = map.m_binding.toJson();
        tracking.m_configuration["origin"] = QJsonObject{{"active", QJsonObject{{"originRevision", "new-origin"}}}};
        emit tracking.configurationChanged(tracking.m_configuration);
        QCOMPARE(map.m_binding.toJson(), bound); // A new base station never silently moves an existing map.
        map.resetToBlankMap();
        QVERIFY(map.m_binding.isUsable());
        QCOMPARE(map.m_binding.context.originRevision, "new-origin");

        // A saved legacy blank map is still an existing map and needs an explicit binding.
        auto legacy = map.serializeMap();
        legacy["frameBinding"] = MapFrameBinding{}.toJson();
        QVERIFY(map.deserializeMap(legacy));
        emit tracking.configurationChanged(tracking.m_configuration);
        QVERIFY(!map.m_binding.isUsable());

        // Planning begun offline must also retain its unconfirmed coordinate identity.
        tracking.m_running = false;
        map.resetToBlankMap();
        QVERIFY(map.addPointInternal(1, 2, 0));
        tracking.m_running = true;
        tracking.m_statusAge.restart();
        emit tracking.configurationChanged(tracking.m_configuration);
        QVERIFY(!map.m_binding.isUsable());
    }
    void presentationDoesNotInvalidatePlanButGeometryDoes() {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.setControlCoordinator(&coordinator);
        map.refreshPlanningRevision();
        tracking.m_preparedPlan = {{"taskId", "task"}, {"revision", 1}};
        map.handleRotationSpinChanged(30);
        map.refreshPlanningRevision();
        QCOMPARE(map.m_binding.context.mapRevision, 1);
        QVERIFY(!tracking.preparedPlan().isEmpty());
        QVERIFY(map.addPointInternal(-2, -3, 0));
        map.refreshPlanningRevision();
        QCOMPARE(map.m_binding.context.mapRevision, 2);
        QVERIFY(tracking.preparedPlan().isEmpty());
    }
    void locatingVehicleCreatesSequentialPointsFromFreshFullPrecisionPose() {
        MapCaptureFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        pose.configure(server.url(), {}, 3000);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        QVERIFY(map.addPointInternal(0, 0, 0, 1));
        QVERIFY(map.addPointInternal(1, 0, 0, 2));
        QVERIFY(map.addLinePath(1, 2, 10, true));
        map.refreshPointUi(); map.refreshSelectors(); map.refreshPlanningRevision();
        map.syncCommittedMapState();
        tracking.m_preparedPlan = {{"taskId", "prepared-task"}, {"revision", 7}};
        const auto revision = map.m_binding.context.mapRevision;
        map.m_pointDwellSpin->setValue(1750);
        MapCaptureFixture::seed(pose);
        map.updateVehiclePose(.123456789, -.234567891, .345678912);
        map.handleLocateCurrentPosition();
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.requests, 1);
        QCOMPARE(map.m_points.size(), 2); // A live frame is not a validated teaching result.
        QVERIFY(!map.m_pointDwellSpin->isEnabled());
        QVERIFY(!map.m_autoConnectPointsCheck->isEnabled());
        QVERIFY(!tracking.preparedPlan().isEmpty());
        QCOMPARE(server.body["durationMs"].toInt(), 1000);
        server.reply();
        QTRY_COMPARE(map.m_points.size(), 3);
        MapCaptureFixture::seed(pose);
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.requests, 2);
        server.reply();
        QTRY_COMPARE(map.m_points.size(), 4);
        map.refreshPlanningRevision();
        QCOMPARE(map.m_points.size(), 4);
        const MapFrameAdapter frame(map.m_binding);
        const auto expected = frame.fromEnu({1.123456789, -2.234567891});
        QCOMPARE(map.pointById(3)->mapPosition, expected);
        QCOMPARE(map.pointById(4)->mapPosition, expected);
        QCOMPARE(map.pointById(3)->dwellMs, 1750);
        QCOMPARE(map.pointById(4)->dwellMs, 1750);
        QVERIFY(std::abs(map.pointById(3)->theta - frame.yawFromEnu(.345678912)) < 1e-12);
        QCOMPARE(map.m_pendingPointSelection.value(), 4);
        QCOMPARE(map.m_binding.context.mapRevision, revision + 2);
        QVERIFY(tracking.preparedPlan().isEmpty());
        QVERIFY(map.hasUnsavedMapChanges());
        pose.m_running = false;
    }
    void delayedMapPointCaptureNeverEditsChangedMap_data() {
        QTest::addColumn<QString>("change");
        for (const auto *change : {"failed", "boot", "missing-id", "edit-off", "edit-off-on", "transform", "new-map", "load-map", "task"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void editingSampledPointDwellKeepsItsCapturedCoordinates() {
        MapCaptureFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        pose.configure(server.url(), {}, 3000);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        QVERIFY(map.addPointInternal(9, 8, .7));
        map.m_pendingPointSelection = 1;
        map.refreshPointUi(); map.handlePointSelectionChanged();
        MapCaptureFixture::seed(pose);
        map.updateVehiclePose(.1, -.2, .3);
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.requests, 1);
        server.reply();
        QTRY_COMPARE(map.m_points.size(), 2);
        const auto position = map.pointById(2)->mapPosition;
        const auto yaw = map.pointById(2)->theta;
        map.m_pointDwellSpin->setValue(2500);
        map.handleUpdatePoint();
        QCOMPARE(map.pointById(2)->dwellMs, 2500);
        QCOMPARE(map.pointById(2)->mapPosition, position);
        QCOMPARE(map.pointById(2)->theta, yaw);
        QCOMPARE(map.pointById(1)->mapPosition, QPointF(9, 8));
        pose.stop();
    }
    void stoppingPoseCaptureUnlocksEditing_data() {
        QTest::addColumn<bool>("reconfigure");
        QTest::newRow("stop") << false;
        QTest::newRow("reconfigure") << true;
    }
    void stoppingPoseCaptureUnlocksEditing() {
        QFETCH(bool, reconfigure);
        MapCaptureFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        pose.configure(server.url(), {}, 3000);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        MapCaptureFixture::seed(pose);
        map.updateVehiclePose(.1, -.2, .3);
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.requests, 1);
        QVERIFY(!map.canEditRowWorkPlan());
        if (reconfigure) pose.configure(QUrl("http://127.0.0.1:1/api/v1"), {}, 3000);
        else pose.stop();
        QVERIFY(map.canEditRowWorkPlan());
        QVERIFY(map.m_pointDwellSpin->isEnabled());
        QCOMPARE(map.m_rowWorkPendingCaptureTarget, Map::RowWorkPendingCaptureTarget::None);
        QCOMPARE(map.m_points.size(), 0);
        QVERIFY(map.m_routeStatusLabel->text().contains(QStringLiteral("采样已取消")));
        pose.stop();
    }
    void delayedMapPointCaptureNeverEditsChangedMap() {
        QFETCH(QString, change);
        MapCaptureFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        pose.configure(server.url(), {}, 3000);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        MapCaptureFixture::seed(pose);
        map.updateVehiclePose(.1, -.2, .3);
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.requests, 1);
        if (change == "edit-off") map.handleEditModeToggled(false);
        if (change == "edit-off-on") {
            map.handleEditModeToggled(false);
            map.handleEditModeToggled(true);
        }
        if (change == "transform") map.m_binding.enuYawOffsetRad = .7;
        if (change == "new-map") map.resetToBlankMap();
        if (change == "load-map") QVERIFY(map.deserializeMap(map.serializeMap()));
        if (change == "task") {
            tracking.m_running = true; tracking.m_statusUsable = true; tracking.m_statusAge.start();
            tracking.m_status.state = "Tracking";
        }
        const auto before = map.buildComparableMapState();
        QSignalSpy completed(&pose, &PoseClient::captureFinished);
        QSignalSpy errors(&pose, &PoseClient::errorOccurred);
        server.reply(change == "failed" ? "failed" : change == "missing-id" ? "running" : "succeeded",
                     change == "boot" ? "other-boot" : "fusion");
        QTRY_VERIFY(completed.count() + errors.count() > 0);
        QCOMPARE(map.buildComparableMapState(), before);
        QCOMPARE(map.m_points.size(), 0);
        QVERIFY(map.m_rowWorkPendingCaptureTarget == Map::RowWorkPendingCaptureTarget::None);
        pose.m_running = false; tracking.m_running = false;
    }
    void locatingVehiclePreservesMapWhenEditingIsLockedAndRejectsStalePose() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        QVERIFY(map.addPointInternal(0, 0, 0));
        map.refreshPlanningRevision(); map.syncCommittedMapState();
        tracking.m_preparedPlan = {{"taskId", "prepared-task"}, {"revision", 7}};
        const auto prepared = tracking.preparedPlan();
        const auto before = map.buildComparableMapState();
        pose.m_running = true; pose.m_pose.validForControl = true;
        pose.m_pose.originRevision = "origin"; pose.m_pose.calibrationId = "cal";
        pose.m_pose.stateAgeMs = 0; pose.m_age.start();
        map.updateVehiclePose(.1, -.2, .3);
        map.handleEditModeToggled(false);
        map.handleLocateCurrentPosition();
        map.handleEditModeToggled(true);
        tracking.m_running = true; tracking.m_statusAge.start();
        for (const QString state : {QStringLiteral("Tracking"), QStringLiteral("Paused")}) {
            tracking.m_status.state = state;
            map.handleLocateCurrentPosition();
        }
        tracking.m_running = false;
        pose.m_pose.stateAgeMs = 1000;
        map.handleLocateCurrentPosition();
        QCOMPARE(map.buildComparableMapState(), before);
        QCOMPARE(tracking.preparedPlan(), prepared);
        QVERIFY(!map.hasUnsavedMapChanges());
        pose.m_pose.stateAgeMs = 0; pose.m_pose.originRevision = "different-origin";
        map.handleLocateCurrentPosition();
        QCOMPARE(map.buildComparableMapState(), before);
        pose.m_running = false;
    }
    void mapDirectoryUsesExecutableAndDateSuggestionsAvoidExistingFiles() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        const QString originalCwd = QDir::currentPath();
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString expected = QDir(QCoreApplication::applicationDirPath()).filePath("maps");
        QVERIFY(QDir(expected).exists());
        QVERIFY(QDir::setCurrent(directory.path()));
        const QString actual = Map::mapsDirectory();
        QVERIFY(QDir::setCurrent(originalCwd));
        QCOMPARE(actual, expected);
        const QString date = QDate::currentDate().toString("yyyy-MM-dd");
        const QString first = QDir(directory.path()).filePath(date + ".json");
        QCOMPARE(map.suggestedMapFilePath(directory.path()), first);
        QFile firstMap(first); QVERIFY(firstMap.open(QIODevice::WriteOnly));
        firstMap.write("existing map"); firstMap.close();
        const QString second = QDir(directory.path()).filePath(date + "-2.json");
        QCOMPARE(map.suggestedMapFilePath(directory.path()), second);
        QFile secondMap(second); QVERIFY(secondMap.open(QIODevice::WriteOnly)); secondMap.close();
        QCOMPARE(map.suggestedMapFilePath(directory.path()), QDir(directory.path()).filePath(date + "-3.json"));
        QVERIFY(firstMap.open(QIODevice::ReadOnly));
        QCOMPARE(firstMap.readAll(), QByteArray("existing map"));
    }
    void poseStreamNoticeClearsOnlyWhenMonitoringRecovers_data() {
        QTest::addColumn<QString>("failure");
        QTest::newRow("disconnect") << QStringLiteral("disconnect");
        QTest::newRow("timeout") << QStringLiteral("timeout");
        QTest::newRow("invalid-json") << QStringLiteral("json");
        QTest::newRow("invalid-fields-without-error-prefix") << QStringLiteral("fields");
        QTest::newRow("http-404-without-error-prefix") << QStringLiteral("404");
    }
    void poseStreamNoticeClearsOnlyWhenMonitoringRecovers() {
        QFETCH(QString, failure);
        MapPoseStreamFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.setControlCoordinator(&coordinator);
        auto *notice = ui.mapPage->findChild<QLabel *>("mapTaskNoticeLabel");
        QVERIFY(notice);
        QSignalSpy errors(&pose, &PoseClient::streamErrorOccurred);
        QSignalSpy genericErrors(&pose, &PoseClient::errorOccurred);
        if (failure == "404") server.nextStreamStatus = 404;
        pose.configure(server.url(), {}, 3000);
        pose.start();
        QTRY_COMPARE(server.streams, 1);
        if (failure == "disconnect") server.disconnectStream();
        if (failure == "json") server.writeLine("invalid json\n");
        if (failure == "fields") server.writeLine("{\"apiVersion\":1}\n");
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(genericErrors.count(), 1); // Existing observers still receive every failure.
        QVERIFY(!pose.isReportingStreamError());
        const auto error = errors.front().front().toString();
        QCOMPARE(notice->text(), TrackingJson::reasonText(error));
        if (failure == "disconnect")
            QCOMPARE(notice->text(), QStringLiteral("定位连接已断开，正在重连"));
        QVERIFY(!notice->text().contains(QStringLiteral("pose_stream")));
        QVERIFY(notice->styleSheet().contains("#b42318"));
        emit pose.monitoringAvailabilityChanged(false, "waiting");
        QCOMPARE(notice->text(), TrackingJson::reasonText(error));
        QTRY_COMPARE(server.streams, 2);
        QCOMPARE(notice->text(), TrackingJson::reasonText(error)); // Headers alone are insufficient.
        server.publish(false); // Communication can recover while the estimate is still invalid.
        QTRY_VERIFY(notice->text().isEmpty());
        QVERIFY(notice->toolTip().isEmpty());
        QVERIFY(notice->styleSheet().contains("#647a6c"));
        QVERIFY(!pose.fresh());
        QVERIFY(!map.m_hasVehiclePose);
        pose.stop();
    }
    void monitoringRecoveryPreservesNewerBusinessOrCaptureErrors_data() {
        QTest::addColumn<bool>("captureError");
        QTest::newRow("business-warning") << false;
        QTest::newRow("capture-error") << true;
    }
    void monitoringRecoveryPreservesNewerBusinessOrCaptureErrors() {
        QFETCH(bool, captureError);
        MapPoseStreamFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        QSignalSpy errors(&pose, &PoseClient::streamErrorOccurred);
        pose.configure(server.url(), {}, 3000); pose.start();
        QTRY_COMPARE(server.streams, 1);
        server.disconnectStream();
        QTRY_COMPARE(errors.count(), 1);
        if (captureError) emit pose.errorOccurred(QStringLiteral("capture_timeout"));
        else map.setRouteStatusText(QStringLiteral("地图坐标版本不匹配，请重新绑定"), true);
        const auto warning = map.m_routeStatusLabel->text();
        const auto tooltip = map.m_routeStatusLabel->toolTip();
        QTRY_COMPARE(server.streams, 2);
        server.publish();
        QTRY_VERIFY(pose.m_monitorOnline);
        QCOMPARE(map.m_routeStatusLabel->text(), warning);
        QCOMPARE(map.m_routeStatusLabel->toolTip(), tooltip);
        QVERIFY(map.m_routeStatusLabel->styleSheet().contains("#b42318"));
        pose.stop();
    }
    void disconnectedCaptureStaysCancelledAfterMonitoringRecovers() {
        MapPoseStreamFixture server;
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        pose.configure(server.url(), {}, 3000); pose.start();
        QTRY_COMPARE(server.streams, 1);
        server.publish();
        QTRY_VERIFY(pose.fresh());
        map.handleLocateCurrentPosition();
        QTRY_COMPARE(server.captures, 1);
        QVERIFY(!map.canEditRowWorkPlan());
        QSignalSpy errors(&pose, &PoseClient::streamErrorOccurred);
        server.disconnectStream();
        QTRY_COMPARE(errors.count(), 1);
        QCOMPARE(map.m_rowWorkPendingCaptureTarget, Map::RowWorkPendingCaptureTarget::None);
        QVERIFY(map.canEditRowWorkPlan());
        const auto cancelled = map.m_routeStatusLabel->text();
        QVERIFY(cancelled.contains(QStringLiteral("采样已取消")));
        QTRY_COMPARE(server.streams, 2);
        server.publish();
        QTRY_VERIFY(pose.m_monitorOnline);
        QCOMPARE(map.m_routeStatusLabel->text(), cancelled);
        QSignalSpy captures(&pose, &PoseClient::captureFinished);
        const auto payload = QJsonDocument(QJsonObject{
            {"apiVersion", 1}, {"fusionBootId", "fusion"}, {"state", "succeeded"},
            {"originRevision", "origin"}, {"calibrationId", "cal"},
            {"sampleCount", 12}, {"positionScatterMeters", .004},
            {"pose", QJsonObject{{"x", 1}, {"y", 2}, {"yaw", 0}}}
        }).toJson(QJsonDocument::Compact);
        QVERIFY(server.capture);
        server.capture->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
            QByteArray::number(payload.size()) + "\r\n\r\n" + payload);
        server.capture->disconnectFromHost();
        QTRY_COMPARE(captures.count(), 1);
        QVERIFY(map.m_points.isEmpty());
        QCOMPARE(map.m_routeStatusLabel->text(), cancelled);
        pose.stop();
    }
    void liveVehiclePoseDoesNotReplaceChosenRouteStart() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        QVERIFY(map.addPointInternal(0, 0, 0, 1));
        QVERIFY(map.addPointInternal(1, 0, 0, 2));
        map.refreshPointUi(); map.refreshSelectors();
        map.m_routeStartCombo->setCurrentIndex(map.m_routeStartCombo->findData(2));
        map.m_routeEndCombo->setCurrentIndex(map.m_routeEndCombo->findData(1));
        QSignalSpy selectionChanges(map.m_routeStartCombo, &QComboBox::currentIndexChanged);
        for (int i = 0; i < 20; ++i)
            map.updateVehiclePose(.001 * i, 0, .01 * i);
        QCOMPARE(map.m_routeStartCombo->currentData().toInt(), 2);
        QCOMPARE(map.m_routeEndCombo->currentData().toInt(), 1);
        QCOMPARE(selectionChanges.size(), 0);
    }
    void enuCardinalsMatchActualVehicleAndPointArrows_data() {
        QTest::addColumn<double>("yaw");
        QTest::addColumn<QPointF>("screenDirection");
        QTest::newRow("east") << 0.0 << QPointF(1, 0);
        QTest::newRow("north") << M_PI_2 << QPointF(0, -1);
        QTest::newRow("west") << M_PI << QPointF(-1, 0);
        QTest::newRow("south") << -M_PI_2 << QPointF(0, 1);
    }
    void enuCardinalsMatchActualVehicleAndPointArrows() {
        QFETCH(double, yaw);
        QFETCH(QPointF, screenDirection);
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding();
        map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {-3, 2};
        p.yaw = yaw;
        p.originRevision = "origin";
        p.calibrationId = "cal";
        p.validForControl = true;
        pose.poseChanged(p);
        QCOMPARE(map.m_vehicleItem->scenePos(), QPointF(-150, -100));
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, screenDirection).length() < 1e-9);
        QVERIFY(map.addPointInternal(-2, -3, MapGeometry::normalizeAngle(yaw + M_PI_2), 1));
        const auto *point = map.pointById(1);
        QCOMPARE(point->markerGroup->scenePos(), QPointF(-150, -100));
        const auto pointNose = point->arrow->mapToScene(QPointF(1, 0)) - point->arrow->mapToScene(QPointF(0, 0));
        QVERIFY(QLineF(pointNose, screenDirection).length() < 1e-9);
    }
    void nonzeroBindingCompilesMetersAndDisplaysSamePhysicalPoint() {
        QMainWindow window;
        Ui::MainWindow ui;
        ui.setupUi(&window);
        Map map(&ui);
        TrackingClient tracking;
        PoseClient pose;
        ControlSessionCoordinator coordinator(&tracking, &pose);
        auto b = testBinding();
        b.enuTranslation = {10, -4};
        b.enuYawOffsetRad = M_PI_2;
        map.m_binding = b;
        map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {8, -7}; p.yaw = M_PI_2;
        p.originRevision = "origin"; p.calibrationId = "cal"; p.validForControl = true;
        pose.poseChanged(p);
        QVERIFY(QLineF(map.m_vehicleItem->scenePos(), {-150, -100}).length() < 1e-9);
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, QPointF(1, 0)).length() < 1e-9);
        TaskCompileOptions options; options.safetyProfileId = "open";
        options.speedLimit = .1; options.goalToleranceMeters = .03;
        options.angularSpeedLimit = .35; options.angleToleranceRad = .05; options.turnTimeoutMs = 30000;
        const auto task = TaskCompiler::route({{"section", {{-2, -3}, {-2, -1}}, {}}}, b, options);
        QVERIFY2(task.ok(), qPrintable(task.error));
        const auto xy = task.plan["steps"].toArray()[0].toObject()["points"].toArray();
        QCOMPARE(xy, (QJsonArray{QJsonArray{8, -7}, QJsonArray{8, -5}}));
        map.handleRotationSpinChanged(90);
        const auto again = TaskCompiler::route({{"section", {{-2, -3}, {-2, -1}}, {}}}, b, options);
        QCOMPARE(again.plan["steps"], task.plan["steps"]);
    }
    void stalePoseKeepsLastPositionVisiblyUnavailable() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        map.m_binding = testBinding(); map.setControlCoordinator(&coordinator);
        ControlPoseSnapshot p;
        p.position = {-3, 2}; p.originRevision = "origin"; p.calibrationId = "cal"; p.validForControl = true;
        pose.poseChanged(p);
        pose.availabilityChanged(false, "stale");
        QVERIFY(!map.m_hasVehiclePose);
        QVERIFY(!map.m_vehicleItem->isVisible() || map.m_vehicleItem->opacity() < .6);
        p.validForControl = false; p.position = {99, 99};
        pose.poseChanged(p);
        QVERIFY(!map.m_hasVehiclePose);
        QCOMPARE(map.m_vehicleItem->scenePos(), QPointF(-150, -100));
    }
    void smallPoseUpdatesReachGraphicsWithoutWaitingForBindingRefresh() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.updateVehiclePose(0, 0, 0);
        map.updateVehiclePose(.005, -.003, .004);
        QVERIFY(QLineF(map.m_vehicleItem->scenePos(), QPointF(-.15, .25)).length() < 1e-9);
        const auto nose = map.m_vehicleItem->mapToScene(QPointF(1, 0)) - map.m_vehicleItem->scenePos();
        QVERIFY(QLineF(nose, QPointF(std::sin(.004), std::cos(.004))).length() < 1e-9);
    }
    void bidirectionalPathUsesTheSameGeometryInBothDirections_data() {
        QTest::addColumn<bool>("arc");
        QTest::newRow("line") << false;
        QTest::newRow("arc") << true;
    }
    void bidirectionalPathUsesTheSameGeometryInBothDirections() {
        QFETCH(bool, arc);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        QVERIFY(map.addPointInternal(-1.123456789, 2.234567891, 0, 1));
        QVERIFY(map.addPointInternal(2.876543211, 2.234567891, 0, 2));
        QVERIFY(arc ? map.addArcPath(1, 2, .75, 10, true)
                    : map.addLinePath(1, 2, 10, true));
        QCOMPARE(map.m_paths.size(), 1);
        QVERIFY(map.pathById(10)->bidirectional);
        const auto forwardIds = map.findRoutePathIds(1, 2);
        const auto reverseIds = map.findRoutePathIds(2, 1);
        QCOMPARE(forwardIds, QList<int>{10});
        QCOMPARE(reverseIds, forwardIds);
        const auto forward = map.composePolyline(forwardIds, 1);
        const auto reverse = map.composePolyline(reverseIds, 2);
        QVERIFY(forward.size() >= 2);
        QVERIFY(QLineF(forward.first(), map.pointById(1)->mapPosition).length() < 1e-9);
        QVERIFY(QLineF(forward.last(), map.pointById(2)->mapPosition).length() < 1e-9);
        QCOMPARE(reverse.size(), forward.size());
        for (qsizetype i = 0; i < forward.size(); ++i)
            QVERIFY(QLineF(reverse.at(i), forward.at(forward.size() - i - 1)).length() < 1e-9);
        if (arc) QVERIFY(forward.size() > 2);
    }
    void mixedBidirectionalPathsStayContinuousAfterSaveLoad() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding();
        QVERIFY(map.addPointInternal(0, 0, 0, 1));
        QVERIFY(map.addPointInternal(2, 0, 0, 2));
        QVERIFY(map.addPointInternal(4, 1, 0, 3));
        QVERIFY(map.addPointInternal(5, 1, 0, 4));
        QVERIFY(map.addLinePath(1, 2, 10, true));
        QVERIFY(map.addArcPath(2, 3, .35, 11, true));
        QVERIFY(map.addLinePath(3, 4, 12, false));
        QCOMPARE(map.findRoutePathIds(1, 3), (QList<int>{10, 11}));
        QCOMPARE(map.findRoutePathIds(3, 1), (QList<int>{11, 10}));
        QCOMPARE(map.findRoutePathIds(1, 4), (QList<int>{10, 11, 12}));
        QVERIFY(map.findRoutePathIds(4, 1).isEmpty());
        QVERIFY(map.composePolyline({12}, 4).isEmpty());
        const auto forward = map.composePolyline({10, 11}, 1);
        const auto reverse = map.composePolyline({11, 10}, 3);
        QCOMPARE(reverse.size(), forward.size());
        for (qsizetype i = 0; i < forward.size(); ++i)
            QVERIFY(QLineF(reverse.at(i), forward.at(forward.size() - i - 1)).length() < 1e-9);
        QCOMPARE(reverse.first(), map.pointById(3)->mapPosition);
        QCOMPARE(reverse.last(), map.pointById(1)->mapPosition);
        Map::RouteStep step; step.fromId = 3; step.toId = 1;
        QVERIFY(map.rebuildRouteStep(step));
        QCOMPARE(step.pathIds, (QList<int>{11, 10}));
        QCOMPARE(map.composePolyline(step.pathIds, step.fromId), reverse);
        TaskCompileOptions options; options.safetyProfileId = "open";
        options.rotationZoneId = "open"; options.speedLimit = .1;
        options.goalToleranceMeters = .03; options.angularSpeedLimit = .2;
        options.angleToleranceRad = .05; options.turnTimeoutMs = 30000;
        const auto compiled = TaskCompiler::route({{"reverse", reverse, {}}}, map.m_binding, options);
        QVERIFY2(compiled.ok(), qPrintable(compiled.error));
        const auto compiledPoints = compiled.plan["steps"].toArray().first().toObject()["points"].toArray();
        MapFrameAdapter adapter(map.m_binding);
        const auto expectedStart = adapter.toEnu(map.pointById(3)->mapPosition);
        const auto expectedEnd = adapter.toEnu(map.pointById(1)->mapPosition);
        const auto actualStart = compiledPoints.first().toArray();
        const auto actualEnd = compiledPoints.last().toArray();
        QVERIFY(QLineF(QPointF(actualStart[0].toDouble(), actualStart[1].toDouble()), expectedStart).length() < 1e-9);
        QVERIFY(QLineF(QPointF(actualEnd[0].toDouble(), actualEnd[1].toDouble()), expectedEnd).length() < 1e-9);

        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto file = directory.filePath("mixed-directions.json");
        QVERIFY(map.saveMapToFile(file));
        QVERIFY(map.loadMapFromFile(file));
        QVERIFY(map.pathById(10)->bidirectional);
        QVERIFY(map.pathById(11)->bidirectional);
        QVERIFY(!map.pathById(12)->bidirectional);
        QCOMPARE(map.m_paths.size(), 3);
        QCOMPARE(map.composePolyline(map.findRoutePathIds(1, 3), 1), forward);
        QCOMPARE(map.composePolyline(map.findRoutePathIds(3, 1), 3), reverse);
        QVERIFY(map.findRoutePathIds(4, 1).isEmpty());
    }
    void legacyDirectedPathDoesNotGainReverseAccessWhenOpened() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        QVERIFY(map.addPointInternal(0, 0, 0, 1));
        QVERIFY(map.addPointInternal(1, 0, 0, 2));
        QVERIFY(map.addLinePath(1, 2, 10));
        auto legacy = map.serializeMap();
        legacy["schemaVersion"] = 2;
        auto paths = legacy["paths"].toArray();
        auto path = paths[0].toObject(); path.remove("bidirectional"); paths[0] = path;
        legacy["paths"] = paths;
        QVERIFY(map.deserializeMap(legacy));
        QCOMPARE(map.findRoutePathIds(1, 2), QList<int>{10});
        QVERIFY(map.findRoutePathIds(2, 1).isEmpty());
        QVERIFY(!map.pathById(10)->bidirectional);
    }
    void fixedExecutionPanelKeepsRuntimeSeparateFromOperationNotices() {
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        TrackingClient tracking; PoseClient pose; ControlSessionCoordinator coordinator(&tracking, &pose);
        tracking.m_running = true;
        tracking.m_statusUsable = true;
        tracking.m_statusAge.start();
        tracking.m_status.bootId = "boot";
        tracking.m_status.state = "Tracking";
        tracking.m_status.taskId = "task";
        tracking.m_status.taskRevision = 2;
        tracking.m_status.executionId = "execution";
        tracking.m_status.stepId = "line-1";
        tracking.m_status.progressMeters = .25;
        tracking.m_status.lengthMeters = 1.5;
        map.setControlCoordinator(&coordinator);
        map.applyTrackingStatus(tracking.m_status);
        auto *panel = ui.mapPage->findChild<QWidget *>("mapTaskExecutionPanel");
        auto *tabs = ui.mapPage->findChild<QTabWidget *>("mapTaskTabWidget");
        auto *runtime = ui.mapPage->findChild<QLabel *>("mapTrackingStatusLabel");
        auto *progress = ui.mapPage->findChild<QLabel *>("mapTaskProgressLabel");
        auto *notice = ui.mapPage->findChild<QLabel *>("mapTaskNoticeLabel");
        QVERIFY(panel); QVERIFY(tabs); QVERIFY(runtime); QVERIFY(progress); QVERIFY(notice);
        QCOMPARE(ui.mapPage->findChildren<QWidget *>("mapTaskExecutionPanel").size(), 1);
        QVERIFY(!tabs->isAncestorOf(panel));
        QVERIFY(!runtime->text().isEmpty());
        QVERIFY(!progress->text().isEmpty());
        const auto before = runtime->text();
        const auto beforeProgress = progress->text();
        coordinator.message(QStringLiteral("测试：完整任务已受理"));
        QCOMPARE(notice->text(), QStringLiteral("测试：完整任务已受理"));
        QCOMPARE(runtime->text(), before);
        QCOMPARE(progress->text(), beforeProgress);
        map.setRouteStatusText(QStringLiteral("测试：路线已编辑"));
        QCOMPARE(notice->text(), QStringLiteral("测试：路线已编辑"));
        map.setRowWorkStatusText(QStringLiteral("测试：直线已示教"));
        QCOMPARE(notice->text(), QStringLiteral("测试：直线已示教"));
        QCOMPARE(runtime->text(), before);
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QVERIFY(panel->isVisibleTo(ui.mapPage));
            QCOMPARE(ui.mapPage->findChild<QWidget *>("mapTaskExecutionPanel"), panel);
            QCOMPARE(runtime->text(), before);
        }
        tracking.m_running = false;
    }
    void retiredMissionDocumentSurvivesMapEditing_data() {
        QTest::addColumn<QJsonObject>("legacy");
        QTest::newRow("empty-object") << QJsonObject{};
        QTest::newRow("disabled-and-future-fields") << QJsonObject{
            {"missionId", "preserved-old-mission"}, {"version", 5},
            {"name", QStringLiteral("原有多垄业务")},
            {"futureMetadata", QJsonObject{{"version", 99}, {"note", "preserve-verbatim"}}},
            {"steps", QJsonArray{QJsonObject{{"stepId", "disabled-turn"}, {"enabled", false},
                {"type", "turn"}, {"targetYawRad", .123456789012345},
                {"note", QStringLiteral("保留尚未启用的步骤")}}}}};
    }
    void retiredMissionDocumentSurvivesMapEditing() {
        QFETCH(QJsonObject, legacy);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_binding = testBinding();
        QVERIFY(map.addPointInternal(0, 0, 0, 1));
        QVERIFY(map.addPointInternal(1, 0, 0, 2));
        QVERIFY(map.addLinePath(1, 2, 10));
        auto document = map.serializeMap();
        document["rowMission"] = legacy;
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto input = directory.filePath("legacy-input.json");
        const auto output = directory.filePath("edited-output.json");
        QFile file(input); QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(document).toJson();
        QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
        QVERIFY(map.loadMapFromFile(input));
        QVERIFY(!ui.mapPage->findChild<QWidget *>("mapRowMissionStepTable"));
        QVERIFY(map.addPointInternal(2, 0, 0, 3));
        TaskCompileOptions options; options.safetyProfileId = "open";
        options.rotationZoneId = "open"; options.speedLimit = .1;
        options.goalToleranceMeters = .03; options.angularSpeedLimit = .2;
        options.angleToleranceRad = .05; options.turnTimeoutMs = 30000;
        const auto route = TaskCompiler::route({{"route", map.composePolyline({10}, 1), {}}},
                                               map.m_binding, options);
        QVERIFY2(route.ok(), qPrintable(route.error));
        QVERIFY(map.saveMapToFile(output));
        file.setFileName(output); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        QVERIFY(saved.contains("rowMission"));
        QCOMPARE(saved["rowMission"].toObject(), legacy);
        QVERIFY(map.loadMapFromFile(output));
        QCOMPARE(map.serializeMap()["rowMission"].toObject(), legacy);
    }
    void rowWorkLineFollowsDisplayedDirection_data() {
        QTest::addColumn<QPointF>("mapDelta");
        QTest::addColumn<QPointF>("screenDirection");
        QTest::newRow("south") << QPointF(1, 0) << QPointF(0, 1);
        QTest::newRow("north") << QPointF(-1, 0) << QPointF(0, -1);
        QTest::newRow("east") << QPointF(0, 1) << QPointF(1, 0);
        QTest::newRow("west") << QPointF(0, -1) << QPointF(-1, 0);
        QTest::newRow("north-east") << QPointF(-3, 4) << QPointF(.8, -.6);
    }
    void rowWorkLineFollowsDisplayedDirection() {
        QFETCH(QPointF, mapDelta);
        QFETCH(QPointF, screenDirection);
        QMainWindow window; Ui::MainWindow ui; ui.setupUi(&window); Map map(&ui);
        map.m_rowWorkPlan.hasStartPose = map.m_rowWorkPlan.hasEndPose = true;
        map.m_rowWorkPlan.startPose = {-2, -3, 0};
        map.m_rowWorkPlan.endPose = {-2 + mapDelta.x(), -3 + mapDelta.y(), 0};
        map.refreshRowWorkGraphics();
        QVERIFY(map.m_rowWorkLineItem);
        const auto path = map.m_rowWorkLineItem->path();
        const auto direction = path.pointAtPercent(1) - path.pointAtPercent(0);
        const auto unit = direction / std::hypot(direction.x(), direction.y());
        QVERIFY(QLineF(unit, screenDirection).length() < 1e-9);
        QVERIFY(map.m_rowWorkLineItem->childItems().isEmpty());
        QCOMPARE(map.m_rowWorkStartMarker->scenePos(), QPointF(-150, -100));
    }
};
QTEST_MAIN(PoseMapChainTest)
#include "test_pose_map_chain.moc"

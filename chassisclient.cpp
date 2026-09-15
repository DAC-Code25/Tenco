#include "chassisclient.h"

#include "networkpolicy.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QLoggingCategory>
#include <QtGlobal>
#include <cmath>

namespace {
Q_LOGGING_CATEGORY(lcChassisClient, "tenco.net.chassis")
}

ChassisClient::ChassisClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_reconnectTimer(new QTimer(this))
{
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->setMaxAllowedIncomingMessageSize(65536);
    m_stateTimer = new QTimer(this);
    m_stateTimer->setInterval(50);
    connect(m_stateTimer, &QTimer::timeout, this, &ChassisClient::pollControlState);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &ChassisClient::readControlState);

    connect(m_socket, &QWebSocket::connected, this, &ChassisClient::handleConnected);
    connect(m_socket, &QWebSocket::disconnected, this, &ChassisClient::handleDisconnected);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
        handleError(static_cast<int>(error));
    });
#else
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this,
            [this](QAbstractSocket::SocketError error) { handleError(static_cast<int>(error)); });
#endif

    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &ChassisClient::attemptReconnect);
}

ChassisClient::~ChassisClient()
{
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_socket) {
        m_socket->close();
    }
}

void ChassisClient::setUrl(const QUrl &url)
{
    m_url = url;
    m_manualDisconnect = false;
    m_reconnectAttempt = 0;
}

void ChassisClient::setAuthorizationToken(const QString &token)
{
    m_authToken = token.trimmed();
}

void ChassisClient::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
    if (!m_autoReconnect && m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
}

void ChassisClient::setReconnectIntervalMs(int intervalMs)
{
    m_reconnectIntervalMs = qMax(200, intervalMs);
    if (m_reconnectMaxIntervalMs < m_reconnectIntervalMs) {
        m_reconnectMaxIntervalMs = m_reconnectIntervalMs;
    }
    m_reconnectAttempt = 0;
}

void ChassisClient::setReconnectMaxIntervalMs(int intervalMs)
{
    m_reconnectMaxIntervalMs = qMax(m_reconnectIntervalMs, intervalMs);
    m_reconnectAttempt = 0;
}

void ChassisClient::connectToHost()
{
    m_manualDisconnect = false;
    openIfPossible();
}

void ChassisClient::disconnectFromHost()
{
    m_manualDisconnect = true;
    m_reconnectAttempt = 0;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_socket) {
        m_socket->close();
    }
}

bool ChassisClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void ChassisClient::openIfPossible()
{
    if (!m_socket) {
        return;
    }
    if (m_manualDisconnect) {
        return;
    }
    if (!m_url.isValid()) {
        qCWarning(lcChassisClient) << "Invalid WebSocket URL:" << m_url;
        emit errorOccurred(tr("WebSocket 地址无效"));
        return;
    }

    const auto state = m_socket->state();
    if (state == QAbstractSocket::ConnectedState || state == QAbstractSocket::ConnectingState) {
        return;
    }

    m_socket->setProxy(QNetworkProxy::NoProxy);
    qCInfo(lcChassisClient) << "Opening WebSocket to" << m_url << ", attempt" << m_reconnectAttempt;
    QNetworkRequest request(m_url);
    NetworkPolicy::applyBearerAuthorization(&request, m_authToken);
    m_socket->open(request);
}

void ChassisClient::sendJson(const QJsonObject &packetObj, const QJsonObject &msgObj)
{
    if (!m_socket) {
        return;
    }
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        openIfPossible();
        return;
    }

    const QJsonDocument doc(QJsonObject{{"packet", packetObj}, {"msg", msgObj}});
    m_socket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void ChassisClient::sendVelocityCommand(double xVel, double thetaVel)
{
    if (!manualPermission() || !std::isfinite(xVel) || !std::isfinite(thetaVel)) return;
    if (m_socket->bytesToWrite() > 4096) {
        revokeLocalPermission();
        m_socket->abort();
        emit errorOccurred(tr("手动链路发送积压，已撤销许可"));
        return;
    }
    const QJsonObject packetObj{{"cmd", "region"}, {"region", "cmd_vel"}, {"index", 1}};
    const QJsonObject msgObj{{"xvel", xVel}, {"yvel", 0.0}, {"thetavel", thetaVel}, {"isRemote", true}};
    sendJson(packetObj, msgObj);
}

void ChassisClient::sendRebootCommand()
{
    if (!controlFresh() || !controlState().stopped() || m_controlState.owner == "Auto") return;
    revokeLocalPermission();
    const QJsonObject packetObj{{"cmd", "reboot"}};
    sendJson(packetObj, QJsonObject{});
}

void ChassisClient::handleConnected()
{
    m_manualDisconnect = false;
    m_reconnectAttempt = 0;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }
    qCInfo(lcChassisClient) << "WebSocket connected";
    revokeLocalPermission();
    m_controlState = {};
    m_feedbackAge.invalidate();
    m_stateTimer->start();
    pollControlState();
    emit connected();
}

void ChassisClient::handleDisconnected()
{
    qCWarning(lcChassisClient) << "WebSocket disconnected, manual?" << m_manualDisconnect;
    revokeLocalPermission();
    m_stateTimer->stop();
    m_controlState = {};
    m_feedbackAge.invalidate();
    emit controlStateChanged(m_controlState);
    emit disconnected();
    if (!m_manualDisconnect && m_autoReconnect && m_reconnectTimer && m_url.isValid()) {
        if (!m_reconnectTimer->isActive()) {
            const int delayMs = currentReconnectDelayMs();
            qCInfo(lcChassisClient) << "Scheduling reconnect in" << delayMs << "ms";
            m_reconnectTimer->start(delayMs);
        }
    }
}

void ChassisClient::handleError(int error)
{
    Q_UNUSED(error);
    const QString errStr = m_socket ? m_socket->errorString() : QStringLiteral("unknown");
    qCWarning(lcChassisClient) << "WebSocket error:" << errStr;
    emit errorOccurred(errStr);
}

void ChassisClient::attemptReconnect()
{
    if (!m_autoReconnect) {
        return;
    }
    if (m_manualDisconnect) {
        return;
    }
    ++m_reconnectAttempt;
    openIfPossible();
}

int ChassisClient::currentReconnectDelayMs() const
{
    return NetworkPolicy::exponentialBackoffDelayMs(m_reconnectIntervalMs,
                                                    m_reconnectMaxIntervalMs,
                                                    m_reconnectAttempt);
}

bool ChassisClient::controlFresh() const {
    return isConnected() && m_controlState.valid && m_feedbackAge.isValid() && m_feedbackAge.elapsed() <= 150;
}
ChassisControlState ChassisClient::controlState() const {
    auto state = m_controlState;
    state.valid = controlFresh();
    state.measuredAgeMs += m_feedbackAge.isValid() ? m_feedbackAge.elapsed() : 1e12;
    return state;
}
bool ChassisClient::manualPermission() const {
    return m_manualGranted && controlFresh() && m_controlState.owner == "Manual" &&
           m_controlState.sessionId == m_manualSession && !m_controlState.estop;
}
void ChassisClient::revokeLocalPermission() {
    const bool granted = m_manualGranted || m_manualRequested;
    m_manualGranted = m_manualRequested = false;
    m_manualSession.clear(); m_stoppedAge.invalidate();
    if (granted) emit manualPermissionChanged(false);
}
void ChassisClient::sendControlOperation(const QString& operation) {
    if (!isConnected()) return;
    if (m_socket->bytesToWrite() > 4096) { revokeLocalPermission(); m_socket->abort(); return; }
    sendJson({{"cmd", "control"}}, {{"protocol", "tenco-control-v1"}, {"operation", operation},
        {"requestId", TrackingJson::newId()}, {"sessionId", m_manualSession},
        {"expectedChassisBootId", m_controlState.bootId}, {"expectedOwnerEpoch", QString::number(m_controlState.epoch)}});
}
void ChassisClient::requestManual() {
    if (manualPermission() || m_manualRequested) return;
    if (!controlFresh() || m_controlState.estop) {
        emit manualPermissionChanged(false);
        emit errorOccurred(tr("底盘控制权反馈无效，接管未确认")); return;
    }
    m_manualSession = TrackingJson::newId(); m_requestedBoot = m_controlState.bootId;
    m_requestedEpoch = m_controlState.epoch; m_manualRequested = true; m_handoffAge.restart();
    m_stoppedAge.invalidate(); sendControlOperation("request_manual");
}
void ChassisClient::releaseManual() {
    if (manualPermission()) sendVelocityCommand(0, 0);
    // Cancel also covers a grant delayed beyond the local handoff deadline.
    if (isConnected() && (m_manualRequested || m_controlState.sessionId == m_manualSession))
        sendControlOperation("release_manual");
    revokeLocalPermission();
}
void ChassisClient::stopLatched() {
    sendControlOperation("stop_latched"); revokeLocalPermission();
}
void ChassisClient::resetStopLatch() {
    if (controlFresh() && controlState().stopped() && m_controlState.estop) sendControlOperation("reset_estop");
}
void ChassisClient::pollControlState() {
    if (!isConnected()) return;
    if ((!controlFresh() && m_manualGranted) || (m_manualRequested && m_handoffAge.elapsed() > 2000)) {
        releaseManual(); emit errorOccurred(tr("底盘接管或反馈超时，手动许可已撤销"));
    }
    sendControlOperation("get_state");
}
void ChassisClient::readControlState(const QString& message) {
    if (message.size() > 65536) return;
    const auto object = QJsonDocument::fromJson(message.toUtf8()).object();
    ChassisControlState state;
    if (!TrackingJson::chassis(object["controlState"].toObject(), &state)) return;
    if (m_controlState.bootId == state.bootId && state.epoch < m_controlState.epoch) return;
    if ((!m_controlState.bootId.isEmpty() && m_controlState.bootId != state.bootId) || state.estop ||
        (m_manualGranted && (state.owner != "Manual" || state.sessionId != m_manualSession || state.epoch != m_controlState.epoch)))
        revokeLocalPermission();
    m_controlState = state; m_feedbackAge.restart();
    if (m_manualRequested && state.owner == "Manual" && state.sessionId == m_manualSession &&
        state.bootId == m_requestedBoot && state.epoch > m_requestedEpoch && state.stopped() && !state.estop) {
        if (!m_stoppedAge.isValid()) m_stoppedAge.start();
        if (m_stoppedAge.elapsed() >= 200) {
            m_manualGranted = true; m_manualRequested = false; emit manualPermissionChanged(true);
        }
    } else if (m_manualRequested) m_stoppedAge.invalidate();
    emit controlStateChanged(controlState());
}

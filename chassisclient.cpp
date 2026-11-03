#include "chassisclient.h"

#include "networkpolicy.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QPointer>
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
    , m_velocityTimer(new QTimer(this))
{
    m_velocityClock.start();
    m_velocityTimer->setTimerType(Qt::PreciseTimer);
    m_velocityTimer->setInterval(10);
    connect(m_velocityTimer, &QTimer::timeout, this, &ChassisClient::serviceVelocityQueue);
    connect(m_socket, &QWebSocket::bytesWritten, this, [this](qint64) { serviceVelocityQueue(); });
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->setMaxAllowedIncomingMessageSize(65536);
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
    blockSignals(true);
    m_manualDisconnect = true;
    clearVelocityQueue();
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
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
    clearVelocityQueue();
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    if (m_socket) {
        // Do not flush locally queued motion while explicitly disconnecting.
        m_socket->abort();
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
    if (!isConnected() || !std::isfinite(xVel) || !std::isfinite(thetaVel)) return;
    applyVelocityAction(m_velocityQueue.submit(m_velocityClock.elapsed(), m_socket->bytesToWrite(), {xVel, thetaVel}));
}

void ChassisClient::clearVelocityQueue()
{
    m_velocityTimer->stop();
    m_velocityQueue.clear();
}

void ChassisClient::failVelocityQueue(const QString &reason)
{
    clearVelocityQueue();
    // abort discards local buffers; it cannot retract data already in TCP or
    // confirm a chassis stop. disconnected clears Home's held manual inputs.
    const QPointer<ChassisClient> guard(this);
    m_socket->abort();
    if (guard) emit errorOccurred(reason);
}

void ChassisClient::serviceVelocityQueue()
{
    if (!isConnected()) { clearVelocityQueue(); return; }
    applyVelocityAction(m_velocityQueue.advance(m_velocityClock.elapsed(), m_socket->bytesToWrite()));
}

void ChassisClient::applyVelocityAction(const ChassisVelocityQueue::Action &action)
{
    if (action.kind == ChassisVelocityQueue::Kind::Disconnect) {
        failVelocityQueue(action.failure == ChassisVelocityQueue::Failure::StopBlocked ?
            tr("手动停止请求遇到发送积压，已断开连接；尚未确认底盘停稳") :
            tr("手动链路发送积压或样本超期，已断开连接；尚未确认底盘停稳"));
        return;
    }
    if (action.kind == ChassisVelocityQueue::Kind::Send) {
        const QJsonObject packetObj{{"cmd", "region"}, {"region", "cmd_vel"}, {"index", 1}};
        const QJsonObject msgObj{{"xvel", action.velocity.linear}, {"yvel", 0.0},
                                {"thetavel", action.velocity.angular}, {"isRemote", true}};
        const auto payload = QJsonDocument(QJsonObject{{"packet", packetObj}, {"msg", msgObj}})
                                 .toJson(QJsonDocument::Compact);
        const QPointer<ChassisClient> guard(this);
        const auto written = m_socket->sendTextMessage(QString::fromUtf8(payload));
        if (!guard) return;
        if (written <= 0) {
            failVelocityQueue(tr("手动速度发送失败，已断开连接；尚未确认底盘停稳"));
            return;
        }
    }
    if (m_velocityQueue.needsService()) m_velocityTimer->start();
    else m_velocityTimer->stop();
}

void ChassisClient::sendRebootCommand()
{
    if (!isConnected()) return;
    const QJsonObject packetObj{{"cmd", "reboot"}};
    sendJson(packetObj, QJsonObject{});
}

void ChassisClient::handleConnected()
{
    clearVelocityQueue();
    m_manualDisconnect = false;
    m_reconnectAttempt = 0;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }
    qCInfo(lcChassisClient) << "WebSocket connected";
    emit connected();
}

void ChassisClient::handleDisconnected()
{
    clearVelocityQueue();
    qCWarning(lcChassisClient) << "WebSocket disconnected, manual?" << m_manualDisconnect;
    const QPointer<ChassisClient> guard(this);
    emit disconnected();
    if (!guard) return;
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

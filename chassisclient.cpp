#include "chassisclient.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QLoggingCategory>
#include <QtGlobal>

namespace {
constexpr int kStartupMessageRepeat = 3;
Q_LOGGING_CATEGORY(lcChassisClient, "tenco.net.chassis")
}

ChassisClient::ChassisClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_reconnectTimer(new QTimer(this))
{
    m_socket->setProxy(QNetworkProxy::NoProxy);

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
    m_startupMessagesSent = false;
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
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    }
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
    const QJsonObject packetObj{{"cmd", "region"}, {"region", "cmd_vel"}, {"index", 1}};
    const QJsonObject msgObj{{"xvel", xVel}, {"yvel", 0.0}, {"thetavel", thetaVel}, {"isRemote", true}};
    sendJson(packetObj, msgObj);
}

void ChassisClient::sendRebootCommand()
{
    const QJsonObject packetObj{{"cmd", "reboot"}};
    sendJson(packetObj, QJsonObject{});
}

void ChassisClient::sendStopLocation()
{
    const QJsonObject packetObj{{"cmd", "region"}, {"region", "slam"}, {"index", 1}};
    const QJsonObject msgObj{{"talk", "stopLocation"}};
    sendJson(packetObj, msgObj);
}

void ChassisClient::sendStartupMessagesIfNeeded()
{
    if (m_startupMessagesSent || !isConnected()) {
        return;
    }

    const QJsonObject stopPacket{{"cmd", "region"}, {"region", "slam"}, {"index", 1}};
    const QJsonObject stopMsg{{"talk", "stopLocation"}};
    const QString stopPayload =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"packet", stopPacket}, {"msg", stopMsg}}).toJson(QJsonDocument::Compact));

    const QJsonObject scriptPacket{{"cmd", "region"}, {"region", "ScriptDeal"}, {"index", 1}};
    const QJsonObject scriptMsg{{"talk", "printScript"},
                                {"name", QStringLiteral("script/motor/\u6b65\u79d1\u7535\u673a-\u5dee\u901f\u8f6e/recmotor.lua")}};
    const QString scriptPayload =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"packet", scriptPacket}, {"msg", scriptMsg}}).toJson(QJsonDocument::Compact));

    for (int i = 0; i < kStartupMessageRepeat; ++i) {
        m_socket->sendTextMessage(stopPayload);
    }
    for (int i = 0; i < kStartupMessageRepeat; ++i) {
        m_socket->sendTextMessage(scriptPayload);
    }

    m_startupMessagesSent = true;
}

void ChassisClient::handleConnected()
{
    m_manualDisconnect = false;
    m_reconnectAttempt = 0;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }
    qCInfo(lcChassisClient) << "WebSocket connected";
    sendStartupMessagesIfNeeded();
    emit connected();
}

void ChassisClient::handleDisconnected()
{
    qCWarning(lcChassisClient) << "WebSocket disconnected, manual?" << m_manualDisconnect;
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
    const int boundedAttempt = qBound(0, m_reconnectAttempt, 6);
    const int base = m_reconnectIntervalMs;
    const int factor = 1 << boundedAttempt;
    const qint64 candidate = static_cast<qint64>(base) * factor;
    return static_cast<int>(qMin(candidate, static_cast<qint64>(m_reconnectMaxIntervalMs)));
}

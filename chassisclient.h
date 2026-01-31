#ifndef CHASSISCLIENT_H
#define CHASSISCLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QUrl>

class QTimer;
class QWebSocket;

// Wraps the chassis WebSocket protocol (cmd_vel, reboot, stopLocation, etc.).
class ChassisClient : public QObject
{
    Q_OBJECT

public:
    explicit ChassisClient(QObject *parent = nullptr);
    ~ChassisClient() override;

    void setUrl(const QUrl &url);
    QUrl url() const { return m_url; }

    void setAutoReconnect(bool enabled);
    bool autoReconnect() const { return m_autoReconnect; }

    void setReconnectIntervalMs(int intervalMs);
    int reconnectIntervalMs() const { return m_reconnectIntervalMs; }

    void connectToHost();
    void disconnectFromHost();
    bool isConnected() const;

    void sendVelocityCommand(double xVel, double thetaVel);
    void sendRebootCommand();
    void sendStopLocation();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &errorString);

private slots:
    void handleConnected();
    void handleDisconnected();
    void handleError(int error);
    void attemptReconnect();

private:
    void openIfPossible();
    void sendJson(const QJsonObject &packetObj, const QJsonObject &msgObj);
    void sendStartupMessagesIfNeeded();

    QUrl m_url;
    QWebSocket *m_socket = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    bool m_autoReconnect = true;
    int m_reconnectIntervalMs = 1000;
    bool m_startupMessagesSent = false;
};

#endif // CHASSISCLIENT_H

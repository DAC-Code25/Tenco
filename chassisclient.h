#ifndef CHASSISCLIENT_H
#define CHASSISCLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QElapsedTimer>
#include "trackingtypes.h"

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
    void setAuthorizationToken(const QString &token);
    QString authorizationToken() const { return m_authToken; }

    void setAutoReconnect(bool enabled);
    bool autoReconnect() const { return m_autoReconnect; }

    void setReconnectIntervalMs(int intervalMs);
    int reconnectIntervalMs() const { return m_reconnectIntervalMs; }
    void setReconnectMaxIntervalMs(int intervalMs);
    int reconnectMaxIntervalMs() const { return m_reconnectMaxIntervalMs; }

    void connectToHost();
    void disconnectFromHost();
    bool isConnected() const;
    bool controlFresh() const;
    bool manualPermission() const;
    ChassisControlState controlState() const;
    void requestManual();
    void releaseManual();
    void stopLatched();
    void resetStopLatch();

    void sendVelocityCommand(double xVel, double thetaVel);
    void sendRebootCommand();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &errorString);
    void controlStateChanged(const ChassisControlState& state);
    void manualPermissionChanged(bool granted);

private slots:
    void handleConnected();
    void handleDisconnected();
    void handleError(int error);
    void attemptReconnect();

private:
    void openIfPossible();
    void sendJson(const QJsonObject &packetObj, const QJsonObject &msgObj);
    void sendControlOperation(const QString& operation);
    void readControlState(const QString& message);
    void pollControlState();
    void revokeLocalPermission();
    int currentReconnectDelayMs() const;

    QUrl m_url;
    QWebSocket *m_socket = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    bool m_autoReconnect = true;
    int m_reconnectIntervalMs = 1000;
    int m_reconnectMaxIntervalMs = 15000;
    int m_reconnectAttempt = 0;
    bool m_manualDisconnect = false;
    QString m_authToken;
    QTimer* m_stateTimer = nullptr;
    ChassisControlState m_controlState;
    QElapsedTimer m_feedbackAge, m_handoffAge, m_stoppedAge;
    QString m_manualSession, m_requestedBoot;
    quint64 m_requestedEpoch = 0;
    bool m_manualRequested = false, m_manualGranted = false;
};

#endif // CHASSISCLIENT_H

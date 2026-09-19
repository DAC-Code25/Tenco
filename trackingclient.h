#pragma once
#include "jsonhttpclient.h"
#include "trackingtypes.h"
#include <QElapsedTimer>
#include <QTimer>

class TrackingClient : public QObject {
    Q_OBJECT
  public:
    explicit TrackingClient(QObject *parent = nullptr);
    void configure(const QUrl &baseUrl, const QString &token, int timeoutMs = 3000);
    void start();
    void stop();
    void stopRenewingSession() { m_renewSession = false; }
    bool isConfigured() const { return m_http.configured(); }
    bool fresh() const;
    bool hasSession() const;
    bool motionReady() const;
    bool busy() const { return !m_pending.isEmpty(); }
    TrackingSnapshot snapshot() const { return m_status; }
    QJsonObject configuration() const { return m_configuration; }
    QJsonObject health() const { return m_health; }
    QJsonObject preparedPlan() const { return m_preparedPlan; }
    void invalidatePreparedPlan() { m_preparedPlan = {}; m_readTask = {}; }
    void acquireSession();
    bool upload(const QJsonObject &plan);
    bool control(const QString &operation);
    bool acknowledge(const QJsonObject &event, bool success);
    bool updateOrigin(const QJsonObject &origin, const QString &expectedRevision);
    void readTask(const QString &taskId, int revision);
    bool confirmReadTask(const QJsonObject &record, const TrackingContext &context);
  signals:
    void statusChanged(const TrackingSnapshot &status);
    void availabilityChanged(bool available, const QString &reason);
    void sessionChanged(bool available);
    void configurationChanged(const QJsonObject &configuration);
    void commandFinished(const QString &operation, const QJsonObject &result);
    void commandUncertain(const QString &operation, const QString &requestId);
    void taskReceived(const QJsonObject &task);
    void executionReceived(const QJsonObject &execution);
    void eventReceived(const QJsonObject &event);
    void eventsResynchronized();
    void originChanged(const QJsonObject &origin);
    void errorOccurred(const QString &reason);
    void serverRestarted();

  private:
    struct Pending {
        QString requestId, commandId, operation, sessionId;
        qint64 createdAt = 0, queriedAt = 0;
    };
    void tick();
    void pollStatus();
    void pollMetadata();
    void heartbeat();
    void pollCommands();
    void pollEvents();
    void loseSession();
    bool sendCommand(const QString &path, QJsonObject body, const QString &operation, bool safety = false);
    JsonHttpClient m_http;
    QTimer m_timer;
    QElapsedTimer m_clock, m_statusAge;
    QHash<QString, Pending> m_pending;
    TrackingSnapshot m_status;
    QJsonObject m_configuration, m_health, m_preparedPlan, m_readTask;
    QString m_readTaskBoot, m_readTaskExecution;
    QString m_clientId, m_sessionId, m_sessionToken, m_sessionBoot, m_permit;
    quint64 m_permitVersion = 0, m_eventCursor = 0;
    qint64 m_sessionDeadline = 0, m_permitDeadline = 0;
    qint64 m_nextHeartbeat = 0, m_nextMetadata = 0, m_nextStatus = 0, m_nextEvents = 0;
    int m_failures = 0;
    qint64 m_statusRoundTripMs = 0;
    bool m_running = false, m_online = false, m_renewSession = true;
};

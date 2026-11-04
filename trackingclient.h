#pragma once
#include "jsonhttpclient.h"
#include "trackingtypes.h"
#include <QElapsedTimer>
#include <QTimer>
#include <QSet>
#include <optional>

class TrackingClient : public QObject {
    Q_OBJECT
  public:
    explicit TrackingClient(QObject *parent = nullptr);
    void configure(const QUrl &baseUrl, const QString &token, int timeoutMs = 5000);
    void start();
    void stop();
    void stopRenewingSession() { m_renewSession = false; }
    bool releaseSessionForMaintenance(QString *message = nullptr);
    bool isConfigured() const { return m_http.configured(); }
    bool matchesConfiguration(const QUrl &url, const QString &token, int timeoutMs) const {
        return m_http.matchesConfiguration(url, token, timeoutMs);
    }
    bool fresh() const;
    bool connected() const;
    bool commandFresh() const;
    bool hasSession() const;
    bool motionReady() const;
    bool configurationFresh() const;
    bool healthFresh() const;
    qint64 healthAgeMs() const;
    void refreshConfiguration();
    bool canResetFault() const;
    bool busy() const { return !m_pending.isEmpty(); }
    TrackingSnapshot snapshot() const { return m_status; }
    QJsonObject configuration() const { return m_configuration; }
    QJsonObject health() const { return m_health; }
    QJsonObject preparedPlan() const { return m_preparedPlan; }
    void invalidatePreparedPlan() { m_preparedPlan = {}; m_readTask = {}; }
    bool acquireSession();
    bool upload(const QJsonObject &plan, QString *requestId = nullptr);
    bool control(const QString &operation, QString *requestId = nullptr);
    bool acknowledge(const QJsonObject &event, bool success);
    bool updateOrigin(const QJsonObject &origin, const QString &expectedRevision);
    bool readTask(const QString &taskId, int revision);
    bool confirmReadTask(const QJsonObject &record, const TrackingContext &context);
  signals:
    void statusChanged(const TrackingSnapshot &status);
    void availabilityChanged(bool available, const QString &reason);
    void sessionChanged(bool available);
    void pendingChanged(bool pending);
    void configurationChanged(const QJsonObject &configuration);
    void commandAccepted(const QString &operation, const QString &requestId);
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
        qint64 createdAt = 0, queriedAt = 0, nextQueryAt = 0;
        int queryCount = 0;
        bool safety = false, accepted = false, postPending = true;
    };
    struct HistoryRead {
        QString boot, execution, cursor, task;
        int taskRevision = 0, pages = 0;
        QSet<QByteArray> seenCursors;
        quint64 generation = 0, resumeCursor = 0, lastSeq = 0, highWater = 0;
        bool highWaterKnown = false;
    };
    void tick();
    void pollStatus();
    void pollMetadata();
    void heartbeat();
    void pollCommands();
    void pollCommandLane(bool safety);
    void noteAccepted(const QString &requestId, const QString &commandId);
    void pollEvents();
    void pollExecutionHistory();
    void loseSession();
    void revokeMotionPermit();
    bool sendCommand(const QString &path, QJsonObject body, const QString &operation, bool safety = false,
                     QString *requestId = nullptr);
    JsonHttpClient m_http;
    // Separate connection pool: slow monitoring and ordinary writes cannot queue stops.
    JsonHttpClient m_safetyHttp;
    QTimer m_timer;
    QElapsedTimer m_clock, m_statusAge, m_configurationAge, m_healthAge;
    QHash<QString, Pending> m_pending;
    std::optional<HistoryRead> m_historyRead;
    quint64 m_historyGeneration = 0;
    QString m_historyContextBoot, m_historyContextTask, m_historyContextExecution;
    int m_historyContextRevision = 0;
    int m_historyAttempts = 0;
    TrackingSnapshot m_status;
    QJsonObject m_configuration, m_health, m_preparedPlan, m_readTask;
    QString m_readTaskBoot, m_readTaskExecution;
    QString m_clientId, m_sessionId, m_sessionToken, m_sessionBoot, m_permit;
    quint64 m_permitVersion = 0, m_eventCursor = 0;
    quint64 m_permitGeneration = 0, m_metadataGeneration = 0, m_configurationGeneration = 0;
    qint64 m_sessionDeadline = 0, m_permitDeadline = 0;
    int m_sessionLeaseMs = 1000;
    qint64 m_nextHeartbeat = 0, m_nextHealth = 0, m_nextConfiguration = 0, m_nextStatus = 0, m_nextEvents = 0;
    qint64 m_nextCommandResult = 0, m_nextSafetyResult = 0;
    int m_failures = 0;
    qint64 m_statusRoundTripMs = 0, m_configurationRoundTripMs = 0, m_healthRoundTripMs = 0;
    bool m_statusUsable = false;
    bool m_statusTransportHealthy = false;
    bool m_running = false, m_online = false, m_connected = false, m_commandFresh = false, m_renewSession = true;
};

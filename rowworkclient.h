#ifndef ROWWORKCLIENT_H
#define ROWWORKCLIENT_H

#include "rowworktypes.h"

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class RowWorkClient : public QObject
{
    Q_OBJECT

public:
    explicit RowWorkClient(QObject *parent = nullptr);
    ~RowWorkClient() override;

    void setBaseUrl(const QUrl &url);
    QUrl baseUrl() const { return m_baseUrl; }

    void setAuthorizationToken(const QString &token);
    void setStatusPollIntervalMs(int intervalMs);
    int statusPollIntervalMs() const { return m_statusPollIntervalMs; }
    void setCommandTimeoutMs(int timeoutMs);
    int commandTimeoutMs() const { return m_commandTimeoutMs; }

    bool isConfigured() const;
    bool isBusy() const;
    bool isPolling() const;

    void startStatusPolling();
    void stopStatusPolling();

    void requestStatus();
    void requestPlan();
    void uploadPlan(const RowWorkPlan &plan);
    void startRowWork();
    void pauseRowWork();
    void resumeRowWork();
    void stopRowWork();
    void capturePose();

signals:
    void busyChanged(bool busy);
    void statusReceived(const RowWorkStatus &status);
    void planReceived(const RowWorkPlan &plan);
    void planUploaded(const QString &message);
    void actionSucceeded(const QString &action, const QString &message);
    void poseCaptured(const RowWorkPose &pose, int sampleDurationMs, int sampleCount, const QString &message);
    void requestFailed(const QString &operation, const QString &message);

private slots:
    void handleReplyFinished();
    void handleRequestTimeout();
    void handleStatusPollTimeout();

private:
    enum class Operation {
        None,
        QueryStatus,
        QueryPlan,
        UploadPlan,
        Start,
        Pause,
        Resume,
        Stop,
        CapturePose
    };

    void sendJsonRequest(Operation operation, const QString &endpointPath, const QByteArray &body = QByteArrayLiteral("{}"), bool useGet = false);
    void cleanupReply();
    QUrl buildEndpointUrl(const QString &endpointPath) const;
    static QString operationName(Operation operation);

    QNetworkAccessManager *m_manager = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QTimer *m_statusPollTimer = nullptr;
    QUrl m_baseUrl;
    QString m_authToken;
    bool m_requestTimedOut = false;
    Operation m_pendingOperation = Operation::None;
    int m_statusPollIntervalMs = 300;
    int m_commandTimeoutMs = 3000;
};

#endif // ROWWORKCLIENT_H

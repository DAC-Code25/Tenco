#ifndef CAMERACONTROLCLIENT_H
#define CAMERACONTROLCLIENT_H

#include <QObject>

#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class CameraControlClient : public QObject
{
    Q_OBJECT

public:
    explicit CameraControlClient(QObject *parent = nullptr);
    ~CameraControlClient() override;

    void setBaseUrl(const QUrl &url);
    QUrl baseUrl() const { return m_baseUrl; }

    void setAuthorizationToken(const QString &token);
    void setRequestTimeoutMs(int timeoutMs);

    bool isConfigured() const;
    bool isBusy() const;
    bool isRecording() const { return m_isRecording; }

    void capturePhoto();
    void startRecording();
    void stopRecording();
    void requestStatus();

signals:
    void busyChanged(bool busy);
    void recordingStateChanged(bool recording);
    void photoSaved(const QString &path, const QString &message);
    void recordingStarted(const QString &path, const QString &message);
    void recordingStopped(const QString &path, const QString &message);
    void statusReceived(bool cameraConnected, bool recording, const QString &message);
    void requestFailed(const QString &operation, const QString &message);

private slots:
    void handleReplyFinished();
    void handleRequestTimeout();

private:
    enum class Operation {
        None,
        CapturePhoto,
        StartRecording,
        StopRecording,
        QueryStatus
    };

    void sendRequest(Operation operation, const QString &endpointPath, bool useGet = false);
    void cleanupReply();
    QUrl buildEndpointUrl(const QString &endpointPath) const;
    static QString operationName(Operation operation);

    QNetworkAccessManager *m_manager = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QUrl m_baseUrl;
    QString m_authToken;
    int m_requestTimeoutMs = 5000;
    bool m_isRecording = false;
    bool m_requestTimedOut = false;
    Operation m_pendingOperation = Operation::None;
};

#endif // CAMERACONTROLCLIENT_H

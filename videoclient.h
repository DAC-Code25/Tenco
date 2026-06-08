#ifndef VIDEOCLIENT_H
#define VIDEOCLIENT_H

#include <QObject>

#include <QByteArray>
#include <QImage>
#include <QNetworkReply>
#include <QString>
#include <QUrl>

#include "videoframeworker.h"

class QNetworkAccessManager;
class QTimer;
class QThread;

// Handles MJPEG streaming over HTTP and exposes decoded frames via signals.
class VideoClient : public QObject
{
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Connecting,
        Streaming,
        Error
    };
    Q_ENUM(State)

    explicit VideoClient(QObject *parent = nullptr);
    ~VideoClient() override;

    void setStreamUrlTemplate(const QString &templateUrl);
    QString streamUrlTemplate() const { return m_streamTemplate; }

    QString buildUrlForTopic(const QString &topic) const;

    void setStreamUrl(const QUrl &url);
    QUrl streamUrl() const { return m_url; }

    void setAutoReconnect(bool enabled);
    bool autoReconnect() const { return m_autoReconnect; }

    void setReconnectIntervalMs(int intervalMs);
    int reconnectIntervalMs() const { return m_reconnectIntervalMs; }
    void setMaxDisplayFps(int fps);
    int maxDisplayFps() const { return m_workerSettings.maxDisplayFps; }

    void start();
    void stop();
    bool isActive() const;

    bool startRecording(const QString &directory, QString *outPath = nullptr);
    QString stopRecording();
    bool isRecording() const { return m_isRecording; }
    QString recordingFilePath() const { return m_recordFilePath; }

    bool saveSnapshot(const QString &directory, QString *outPath = nullptr) const;
    QImage lastFrame() const;
    VideoFrameWorker::Metrics metrics() const;

signals:
    void frameReceived(const QImage &frame);
    void stateChanged(VideoClient::State state, const QString &message);

private slots:
    void handleReadyRead();
    void handleStreamFinished();
    void handleStreamError(QNetworkReply::NetworkError error);
    void restartStream();

private:
    void setState(State state, const QString &message = QString());
    void scheduleReconnect();
    void cleanupReply();
    int currentReconnectDelayMs() const;
    void initializeFrameWorker();
    void resetFrameWorkerStream();

    QNetworkAccessManager *m_manager = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QThread *m_frameThread = nullptr;
    VideoFrameWorker *m_frameWorker = nullptr;
    VideoFrameWorker::Settings m_workerSettings;

    QString m_streamTemplate;
    QUrl m_url;

    int m_reconnectIntervalMs = 2000;
    int m_reconnectMaxIntervalMs = 30000;
    int m_reconnectAttempt = 0;
    bool m_autoReconnect = true;
    State m_state = State::Stopped;
    bool m_seenFirstFrame = false;

    QString m_recordFilePath;
    bool m_isRecording = false;

    QImage m_lastFrame;
};

#endif // VIDEOCLIENT_H

#pragma once
#include "jsonhttpclient.h"
#include "trackingtypes.h"
#include <QElapsedTimer>
#include <QTimer>

class PoseClient : public QObject {
    Q_OBJECT
  public:
    explicit PoseClient(QObject *parent = nullptr);
    ~PoseClient() override;
    void configure(const QUrl &baseUrl, const QString &token, int timeoutMs = 3000);
    void start();
    void stop();
    bool isConfigured() const { return m_http.configured(); }
    bool fresh() const;
    void setStateTimeoutMs(int timeoutMs);
    ControlPoseSnapshot snapshot() const;
    bool capture(const MapFrameBinding &binding, int durationMs = 1000);
  signals:
    void poseChanged(const ControlPoseSnapshot &pose);
    void availabilityChanged(bool fresh, const QString &reason);
    void frameChanged();
    void captureFinished(const QJsonObject &capture);
    void errorOccurred(const QString &reason);

  private:
    void openStream();
    void readStream();
    void failStream(const QString &reason);
    bool acceptPose(const QJsonObject &object, qint64 lineAgeMs);
    void pollCapture();
    void acceptCapture(const JsonHttpResult &result);
    JsonHttpClient m_http;
    QNetworkAccessManager m_streamManager;
    QPointer<QNetworkReply> m_streamReply;
    QUrl m_streamBaseUrl;
    QString m_streamToken;
    QByteArray m_streamBuffer;
    QTimer m_timer, m_streamDeadline;
    QElapsedTimer m_lineAge;
    QElapsedTimer m_age, m_captureAge;
    ControlPoseSnapshot m_pose;
    QString m_captureId, m_captureBoot;
    bool m_running = false, m_online = false;
    int m_failures = 0;
    int m_stateTimeoutMs = 350;
    int m_streamTimeoutMs = 3000;
    double m_firstStreamSendTime = 0, m_lastStreamSendTime = 0;
    qint64 m_firstStreamReceiveMs = 0;
    qint64 m_nextPoll = 0;
    QElapsedTimer m_clock;
};

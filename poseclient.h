#pragma once
#include "jsonhttpclient.h"
#include "trackingtypes.h"
#include <QElapsedTimer>
#include <QTimer>

class PoseClient : public QObject {
    Q_OBJECT
  public:
    explicit PoseClient(QObject *parent = nullptr);
    void configure(const QUrl &baseUrl, const QString &token, int timeoutMs = 3000);
    void start();
    void stop();
    bool isConfigured() const { return m_http.configured(); }
    bool fresh() const;
    ControlPoseSnapshot snapshot() const;
    bool capture(const MapFrameBinding &binding, int durationMs = 1000);
  signals:
    void poseChanged(const ControlPoseSnapshot &pose);
    void availabilityChanged(bool fresh, const QString &reason);
    void frameChanged();
    void captureFinished(const QJsonObject &capture);
    void errorOccurred(const QString &reason);

  private:
    void poll();
    void pollCapture();
    void acceptCapture(const JsonHttpResult &result);
    JsonHttpClient m_http;
    QTimer m_timer;
    QElapsedTimer m_age, m_captureAge;
    ControlPoseSnapshot m_pose;
    QString m_captureId, m_captureBoot;
    bool m_running = false, m_online = false;
    int m_failures = 0;
    qint64 m_nextPoll = 0;
    QElapsedTimer m_clock;
};

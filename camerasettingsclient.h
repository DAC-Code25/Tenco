#pragma once
#include "jsonhttpclient.h"
#include <QElapsedTimer>
#include <QTimer>

// The IPC owns these settings. Local configuration save never writes them.
class CameraSettingsClient : public QObject {
    Q_OBJECT
public:
    explicit CameraSettingsClient(QObject *parent = nullptr);
    void configure(const QUrl &url, const QString &token, int timeoutMs = 5000);
    void read();
    bool apply(const QJsonObject &settings);
    void refreshStatus();
    bool busy() const { return m_busy; }
    const QJsonObject &configuration() const { return m_configuration; }
    const QJsonObject &capabilities() const { return m_capabilities; }
signals:
    void loaded(const QJsonObject &configuration, const QJsonObject &capabilities);
    void statusReceived(const QJsonObject &status);
    void busyChanged(bool busy);
    void finished(bool success, const QString &message);
private:
    void setBusy(bool busy);
    void poll();
    void observe(const QJsonObject &configuration);
    void finish(bool ok, const QString &message);
    static bool validConfiguration(const QJsonObject &value);
    JsonHttpClient m_http;
    QTimer m_poll;
    QElapsedTimer m_elapsed;
    QJsonObject m_configuration, m_capabilities, m_target;
    QString m_expectedRevision, m_targetRevision;
    bool m_busy = false, m_applying = false;
};

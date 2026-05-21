#ifndef STATUSCLIENT_H
#define STATUSCLIENT_H

#include <QObject>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

class QThread;
class HomeNetworkWorker;

// Owns a HomeNetworkWorker + QThread and exposes status polling via signals.
class StatusClient : public QObject
{
    Q_OBJECT

public:
    explicit StatusClient(QObject *parent = nullptr);
    ~StatusClient() override;

    void configure(const QUrl &url,
                   const QJsonArray &requests,
                   int intervalMs,
                   const QString &authToken = QString(),
                   int requestTimeoutMs = 3000,
                   int maxBackoffMs = 5000);
    void start();
    void stop();

    bool isRunning() const { return m_running; }

signals:
    void statusReceived(const QJsonObject &packet);
    void requestFailed(int httpStatus, const QString &errorString, const QByteArray &responseBody);

private:
    void rebuildWorker(bool restartAfterwards);
    void teardownWorker();

    QUrl m_url;
    QJsonArray m_requests;
    int m_intervalMs = 100;
    int m_requestTimeoutMs = 3000;
    int m_maxBackoffMs = 5000;
    QString m_authToken;

    HomeNetworkWorker *m_worker = nullptr;
    QThread *m_thread = nullptr;
    bool m_running = false;
};

#endif // STATUSCLIENT_H

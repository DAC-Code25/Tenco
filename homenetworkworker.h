#ifndef HOMENETWORKWORKER_H
#define HOMENETWORKWORKER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>
#include <QByteArray>

class QTimer;
class QNetworkReply;
class QNetworkAccessManager;

class HomeNetworkWorker : public QObject
{
    Q_OBJECT
public:
    explicit HomeNetworkWorker(QObject *parent = nullptr);

    void configure(const QUrl &url, const QJsonArray &requests, int intervalMs);

public slots:
    void start();
    void stop();
    void triggerFetch();

signals:
    void statusReceived(const QJsonObject &packet);
    void requestFailed(int httpStatus, const QString &errorString, const QByteArray &responseBody);

private:
    void handleReply(QNetworkReply *reply);
    void onReplyFinished();

    QNetworkAccessManager *m_manager;
    QTimer *m_timer;
    QUrl m_url;
    QJsonArray m_requests;
    QNetworkReply *m_currentReply = nullptr;
    bool m_fetchPending = false;
    bool m_running = false;
    int m_intervalMs = 100;
};

#endif // HOMENETWORKWORKER_H

#include "homenetworkworker.h"

#include "networkpolicy.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QLoggingCategory>
#include <QPointer>
#include <QtGlobal>

Q_LOGGING_CATEGORY(lcHomeNetworkWorker, "tenco.net.status")

HomeNetworkWorker::HomeNetworkWorker(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
    , m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, &HomeNetworkWorker::triggerFetch);
}

void HomeNetworkWorker::configure(const QUrl &url,
                                  const QJsonArray &requests,
                                  int intervalMs,
                                  const QString &authToken,
                                  int requestTimeoutMs,
                                  int maxBackoffMs)
{
    const bool restart = m_running;
    if (restart) stop();
    m_url = url;
    m_requests = requests;
    m_intervalMs = qBound(50, intervalMs, 5000);
    m_authToken = authToken.trimmed();
    m_requestTimeoutMs = qBound(500, requestTimeoutMs, 30000);
    m_maxBackoffMs = qBound(m_intervalMs, maxBackoffMs, 60000);
    m_timer->setInterval(m_intervalMs);
    if (restart) start();
}

void HomeNetworkWorker::start()
{
    if (m_running) {
        return;
    }

    m_running = true;
    ++m_generation;
    m_failureCount = 0;
    m_timer->setInterval(m_intervalMs);
    triggerFetch();
}

void HomeNetworkWorker::stop()
{
    m_running = false;
    ++m_generation;
    m_timer->stop();
    m_failureCount = 0;
    m_timer->setInterval(m_intervalMs);

    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        if (m_currentReply->isRunning()) {
            m_currentReply->abort();
        }
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
}

void HomeNetworkWorker::triggerFetch()
{
    if (!m_running) {
        return;
    }

    if (!m_url.isValid() || m_requests.isEmpty()) {
        return;
    }

    if (m_currentReply) {
        return;
    }
    m_timer->stop();

    QNetworkRequest request = NetworkPolicy::makeJsonRequest(m_url,
                                                             m_authToken,
                                                             QByteArrayLiteral("TencoStatusClient/1.0"),
                                                             m_requestTimeoutMs);

    const QByteArray payload = QJsonDocument(m_requests).toJson(QJsonDocument::Compact);
    m_currentReply = m_manager->post(request, payload);
    connect(m_currentReply, &QNetworkReply::finished, this, &HomeNetworkWorker::onReplyFinished);
}
void HomeNetworkWorker::onReplyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }

    if (reply != m_currentReply) return;
    m_currentReply = nullptr;
    const auto generation = m_generation;
    const QPointer<HomeNetworkWorker> guard(this);
    reply->deleteLater();
    handleReply(reply);

    // A slow response never creates polling debt or bypasses error backoff.
    if (guard && m_generation == generation && m_running && !m_currentReply) m_timer->start();
}

void HomeNetworkWorker::handleReply(QNetworkReply *reply)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool canReadBody = reply->isOpen();
    const QByteArray payload = canReadBody ? reply->readAll() : QByteArray();

    if (reply->error() != QNetworkReply::NoError || httpStatus != 200) {
        applyPollingIntervalByHealth(false);
        qCWarning(lcHomeNetworkWorker) << "Status poll failed, http=" << httpStatus << ", error=" << reply->errorString();
        emit requestFailed(httpStatus, reply->error() == QNetworkReply::NoError ? QString() : reply->errorString(), payload);
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        applyPollingIntervalByHealth(false);
        qCWarning(lcHomeNetworkWorker) << "Status payload parse failed:" << parseError.errorString();
        emit requestFailed(httpStatus, parseError.errorString(), payload);
        return;
    }

    applyPollingIntervalByHealth(true);
    emit statusReceived(doc.object());
}

void HomeNetworkWorker::applyPollingIntervalByHealth(bool success)
{
    if (success) {
        m_failureCount = 0;
        if (m_timer->interval() != m_intervalMs) {
            m_timer->setInterval(m_intervalMs);
        }
        return;
    }

    ++m_failureCount;
    const int backoffInterval = NetworkPolicy::exponentialBackoffDelayMs(m_intervalMs,
                                                                         m_maxBackoffMs,
                                                                         qMax(1, m_failureCount));

    if (m_timer->interval() != backoffInterval) {
        m_timer->setInterval(backoffInterval);
    }
}

#include "homenetworkworker.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTimer>
#include <QMetaObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcHomeNetworkWorker, "tenco.net.status")

HomeNetworkWorker::HomeNetworkWorker(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
    , m_timer(new QTimer(this))
{
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &HomeNetworkWorker::triggerFetch);
}

void HomeNetworkWorker::configure(const QUrl &url, const QJsonArray &requests, int intervalMs, const QString &authToken)
{
    m_url = url;
    m_requests = requests;
    m_intervalMs = qBound(50, intervalMs, 5000);
    m_authToken = authToken.trimmed();
    m_timer->setInterval(m_intervalMs);
}

void HomeNetworkWorker::start()
{
    if (m_running) {
        return;
    }

    m_running = true;
    m_fetchPending = false;
    m_failureCount = 0;
    m_timer->setInterval(m_intervalMs);

    if (!m_timer->isActive()) {
        m_timer->start(m_intervalMs);
    }

    triggerFetch();
}

void HomeNetworkWorker::stop()
{
    m_running = false;
    m_timer->stop();
    m_fetchPending = false;
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

    if (m_currentReply && m_currentReply->isRunning()) {
        m_fetchPending = true;
        return;
    }

    m_fetchPending = false;

    QNetworkRequest request(m_url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Connection", "keep-alive");
    request.setRawHeader("User-Agent", "TencoClient/1.0");
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(m_requestTimeoutMs);
#endif

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

    if (reply == m_currentReply) {
        m_currentReply = nullptr;
    }

    handleReply(reply);

    reply->deleteLater();

    if (m_running && m_fetchPending) {
        m_fetchPending = false;
        QMetaObject::invokeMethod(this, &HomeNetworkWorker::triggerFetch, Qt::QueuedConnection);
    }
}

void HomeNetworkWorker::handleReply(QNetworkReply *reply)
{
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray payload = reply->readAll();

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
    const int boundedFailures = qBound(1, m_failureCount, 6);
    const int factor = 1 << boundedFailures;
    const qint64 candidate = static_cast<qint64>(m_intervalMs) * factor;
    const int backoffInterval = static_cast<int>(qMin(candidate, static_cast<qint64>(m_maxBackoffMs)));

    if (m_timer->interval() != backoffInterval) {
        m_timer->setInterval(backoffInterval);
    }
}

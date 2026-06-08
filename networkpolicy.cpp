#include "networkpolicy.h"

#include <QNetworkRequest>
#include <QUrl>
#include <QtGlobal>

namespace NetworkPolicy {

int exponentialBackoffDelayMs(int baseIntervalMs, int maxIntervalMs, int attempt, int maxExponent)
{
    const int base = qMax(1, baseIntervalMs);
    const int maximum = qMax(base, maxIntervalMs);
    const int boundedAttempt = qBound(0, attempt, qMax(0, maxExponent));
    const qint64 factor = qint64(1) << boundedAttempt;
    const qint64 candidate = qint64(base) * factor;
    return static_cast<int>(qMin(candidate, qint64(maximum)));
}

QByteArray bearerAuthorizationHeader(const QString &token)
{
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return QByteArray();
    }
    return QByteArrayLiteral("Bearer ") + trimmed.toUtf8();
}

void applyBearerAuthorization(QNetworkRequest *request, const QString &token)
{
    if (!request) {
        return;
    }
    const QByteArray header = bearerAuthorizationHeader(token);
    if (!header.isEmpty()) {
        request->setRawHeader("Authorization", header);
    }
}

QNetworkRequest makeJsonRequest(const QUrl &url,
                                const QString &authToken,
                                const QByteArray &userAgent,
                                int transferTimeoutMs)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Connection", "keep-alive");
    if (!userAgent.isEmpty()) {
        request.setRawHeader("User-Agent", userAgent);
    }
    applyBearerAuthorization(&request, authToken);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (transferTimeoutMs > 0) {
        request.setTransferTimeout(transferTimeoutMs);
    }
#else
    Q_UNUSED(transferTimeoutMs);
#endif
    return request;
}

} // namespace NetworkPolicy

#ifndef NETWORKPOLICY_H
#define NETWORKPOLICY_H

#include <QByteArray>
#include <QString>

class QNetworkRequest;
class QUrl;

namespace NetworkPolicy {

int exponentialBackoffDelayMs(int baseIntervalMs, int maxIntervalMs, int attempt, int maxExponent = 6);
QByteArray bearerAuthorizationHeader(const QString &token);
void applyBearerAuthorization(QNetworkRequest *request, const QString &token);
QNetworkRequest makeJsonRequest(const QUrl &url,
                                const QString &authToken = QString(),
                                const QByteArray &userAgent = QByteArrayLiteral("TencoClient/1.0"),
                                int transferTimeoutMs = 0);

} // namespace NetworkPolicy

#endif // NETWORKPOLICY_H

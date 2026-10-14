#include "jsonhttpclient.h"
#include "networkpolicy.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTimer>
#include <cstring>

namespace {
// A consumed sequential body cannot be replayed by Qt after a connection closes.
// Resolve an ambiguous write using its requestId instead.
class OneShotBody final : public QIODevice {
  public:
    explicit OneShotBody(QByteArray body) : m_body(std::move(body)) { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    bool reset() override { return false; }
    qint64 bytesAvailable() const override { return m_body.size() - m_offset + QIODevice::bytesAvailable(); }
    bool atEnd() const override { return m_offset == m_body.size(); }

  protected:
    qint64 readData(char *target, qint64 length) override {
        const auto size = qMin(length, qint64(m_body.size() - m_offset));
        if (size <= 0)
            return -1;
        std::memcpy(target, m_body.constData() + m_offset, size);
        m_offset += size;
        return size;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

  private:
    QByteArray m_body;
    qint64 m_offset = 0;
};
} // namespace

JsonHttpClient::JsonHttpClient(QObject *parent) : QObject(parent) {
    m_manager.setProxy(QNetworkProxy::NoProxy);
}
JsonHttpClient::~JsonHttpClient() { cancelAll(); }
void JsonHttpClient::configure(const QUrl &url, const QString &bearer, int timeout) {
    cancelAll();
    m_base = url;
    m_bearer = bearer.trimmed();
    m_timeoutMs = qBound(200, timeout, 10000);
}
bool JsonHttpClient::configured() const {
    return m_base.isValid() && !m_base.host().isEmpty() && m_base.userInfo().isEmpty() &&
           (m_base.scheme() == "http" || m_base.scheme() == "https");
}
void JsonHttpClient::cancelAll() {
    ++m_generation;
    const auto replies = m_pending;
    m_pending.clear();
    for (const auto &reply : replies)
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
}
bool JsonHttpClient::request(const QString &lane, const QString &method, const QString &path,
                             const QJsonObject &body, const QUrlQuery &query, const QString &operatorToken,
                             Callback callback, qint64 maxResponseBytes) {
    if (!configured() || busy(lane) || m_pending.size() >= 12)
        return false;
    const qint64 responseLimit = qBound(qint64(1), maxResponseBytes, qint64(16 * 1024 * 1024));
    QUrl url = m_base;
    QString prefix = url.path();
    while (prefix.endsWith('/'))
        prefix.chop(1);
    url.setPath(prefix + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    NetworkPolicy::applyBearerAuthorization(&request, m_bearer);
    if (!operatorToken.isEmpty())
        request.setRawHeader("X-Operator-Token", operatorToken.toUtf8());
    QNetworkReply *reply = nullptr;
    if (method == "GET")
        reply = m_manager.get(request);
    else {
        auto *upload = new OneShotBody(QJsonDocument(body).toJson(QJsonDocument::Compact));
        request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        request.setHeader(QNetworkRequest::ContentLengthHeader, upload->bytesAvailable());
        reply = m_manager.post(request, upload);
        upload->setParent(reply);
    }
    reply->setReadBufferSize(responseLimit + 1);
    m_pending.insert(lane, reply);
    const auto generation = m_generation;
    auto bytes = std::make_shared<QByteArray>();
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply] {
        reply->setProperty("deadline", true);
        reply->abort();
    });
    deadline->start(m_timeoutMs);
    connect(reply, &QIODevice::readyRead, reply, [reply, bytes, responseLimit] {
        bytes->append(reply->readAll());
        if (bytes->size() > responseLimit) {
            reply->setProperty("oversize", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, lane, generation, callback = std::move(callback), bytes, elapsed, deadline, responseLimit] {
                deadline->stop();
                reply->deleteLater();
                if (generation != m_generation)
                    return;
                m_pending.remove(lane);
                if (reply->isOpen())
                    bytes->append(reply->readAll());
                JsonHttpResult result;
                result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                result.roundTripMs = elapsed->elapsed();
                if (reply->property("oversize").toBool() || bytes->size() > responseLimit)
                    result.error = "response_too_large";
                else if (reply->property("deadline").toBool())
                    result.error = "request_timeout";
                else if (result.status == 0)
                    result.error = "connection_failed";
                else if (result.status != 204) {
                    QJsonParseError error;
                    const auto doc = QJsonDocument::fromJson(*bytes, &error);
                    if (error.error != QJsonParseError::NoError || !doc.isObject())
                        result.error = "invalid_json_response";
                    else
                        result.object = doc.object();
                }
                if (result.error.isEmpty() && (result.status < 200 || result.status >= 300))
                    result.error =
                        result.object.value("code").toString(QStringLiteral("http_%1").arg(result.status));
                callback(result);
            });
    return true;
}

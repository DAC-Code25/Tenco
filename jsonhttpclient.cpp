#include "jsonhttpclient.h"
#include "networkpolicy.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTimer>
#include <QUuid>
#include <QRegularExpression>
#include <cmath>
#include <cstring>

namespace {
Q_LOGGING_CATEGORY(httpTimingLog, "tenco.http.timing")
QJsonObject serverTiming(const QByteArray &header) {
    QJsonObject result;
    if (header.size() > 2048) return result;
    const auto metrics = header.split(',');
    if (metrics.size() > 16) return result;
    static const QRegularExpression duration(QStringLiteral("^[0-9]{1,9}(?:\\.[0-9]{1,6})?$"));
    for (const auto &metric : metrics) {
        const auto fields = metric.split(';');
        const auto name = fields.first().trimmed();
        if (name != "body" && name != "app" && name != "queue" && name != "serialize" && name != "dispatch") continue;
        if (result.contains(QString::fromLatin1(name))) continue;
        QByteArray milliseconds;
        bool seen = false, duplicate = false;
        for (int index = 1; index < fields.size(); ++index) {
            const auto field = fields.at(index).trimmed();
            if (!field.startsWith("dur=")) continue;
            if (seen) duplicate = true;
            seen = true;
            milliseconds = field.mid(4).trimmed();
        }
        if (duplicate || !duration.match(QString::fromLatin1(milliseconds)).hasMatch()) continue;
        bool valid = false;
        const auto value = milliseconds.toDouble(&valid);
        if (valid && std::isfinite(value) && value <= 3600000) result.insert(QString::fromLatin1(name), value);
    }
    return result;
}
struct RequestTiming {
    QElapsedTimer elapsed;
    qint64 connectionStartMs = -1, sentMs = -1, firstResponseMs = -1;
    int sendAttempts = 0;
    RequestTiming() { elapsed.start(); }
};
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
    m_operationManager.setProxy(QNetworkProxy::NoProxy);
}
JsonHttpClient::~JsonHttpClient() { cancelAll(); }
void JsonHttpClient::configure(const QUrl &url, const QString &bearer, int timeout) {
    cancelAll();
    m_base = url;
    m_bearer = bearer.trimmed();
    m_timeoutMs = qBound(200, timeout, 10000);
}
bool JsonHttpClient::matchesConfiguration(const QUrl &url, const QString &bearer, int timeout) const {
    return m_base == url && m_bearer == bearer.trimmed() && m_timeoutMs == qBound(200, timeout, 10000);
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
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.setRawHeader("X-Request-ID", requestId.toLatin1());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    // Monitoring can occupy Qt's per-host connections on a slow wireless link.
    // Session renewal, commands and their results use a separate bounded pool.
    const bool operation = method != "GET" || lane == "result";
    request.setPriority(operation ? QNetworkRequest::HighPriority :
                        lane == "status" ? QNetworkRequest::NormalPriority : QNetworkRequest::LowPriority);
    NetworkPolicy::applyBearerAuthorization(&request, m_bearer);
    if (!operatorToken.isEmpty())
        request.setRawHeader("X-Operator-Token", operatorToken.toUtf8());
    auto timing = std::make_shared<RequestTiming>();
    auto &manager = operation ? m_operationManager : m_manager;
    QNetworkReply *reply = nullptr;
    if (method == "GET")
        reply = manager.get(request);
    else {
        auto *upload = new OneShotBody(QJsonDocument(body).toJson(QJsonDocument::Compact));
        request.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        request.setHeader(QNetworkRequest::ContentLengthHeader, upload->bytesAvailable());
        reply = method == "POST" ? manager.post(request, upload)
                                 : manager.sendCustomRequest(request, method.toUtf8(), upload);
        upload->setParent(reply);
    }
    reply->setReadBufferSize(responseLimit + 1);
    m_pending.insert(lane, reply);
    const auto generation = m_generation;
    auto bytes = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::socketStartedConnecting, reply, [timing] {
        if (timing->connectionStartMs < 0) timing->connectionStartMs = timing->elapsed.elapsed();
    });
    connect(reply, &QNetworkReply::requestSent, reply, [timing] {
        if (timing->sentMs < 0) timing->sentMs = timing->elapsed.elapsed();
        ++timing->sendAttempts;
    });
    connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, timing] {
        if (timing->firstResponseMs < 0 &&
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() > 0)
            timing->firstResponseMs = timing->elapsed.elapsed();
    });
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
            [this, reply, lane, method, generation, requestId, callback = std::move(callback), bytes, timing, deadline, responseLimit] {
                deadline->stop();
                reply->deleteLater();
                if (generation != m_generation)
                    return;
                m_pending.remove(lane);
                if (reply->isOpen())
                    bytes->append(reply->readAll());
                JsonHttpResult result;
                result.requestId = requestId;
                result.serverRequestIdMatched = reply->rawHeader("X-Request-ID") == requestId.toLatin1();
                if (result.serverRequestIdMatched) result.serverTimingMs = serverTiming(reply->rawHeader("Server-Timing"));
                result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                result.roundTripMs = timing->elapsed.elapsed();
                result.connectionStartMs = timing->connectionStartMs;
                result.requestSentMs = timing->sentMs;
                result.firstResponseMs = timing->firstResponseMs;
                result.sendAttempts = timing->sendAttempts;
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
                const QJsonObject measurements{{"method", method}, {"status", result.status},
                    {"requestId", result.requestId}, {"serverRequestIdMatched", result.serverRequestIdMatched},
                    {"serverTimingMs", result.serverTimingMs},
                    {"ok", result.ok()}, {"totalMs", result.roundTripMs},
                    {"connectionStartMs", result.connectionStartMs}, {"requestSentMs", result.requestSentMs},
                    {"firstResponseMs", result.firstResponseMs}, {"sendAttempts", result.sendAttempts}};
                if (result.roundTripMs >= 1000 && (!m_slowLogAge.isValid() || m_slowLogAge.elapsed() >= 5000)) {
                    m_slowLogAge.start();
                    qCWarning(httpTimingLog).nospace() << "slow HTTP lane=" << lane << " method=" << method
                        << " status=" << result.status << " totalMs=" << result.roundTripMs
                        << " connectStartMs=" << result.connectionStartMs << " sentMs=" << result.requestSentMs
                        << " firstResponseMs=" << result.firstResponseMs << " sends=" << result.sendAttempts
                        << " requestId=" << result.requestId
                        << " serverRequestIdMatched=" << result.serverRequestIdMatched << " serverTimingMs="
                        << QJsonDocument(result.serverTimingMs).toJson(QJsonDocument::Compact);
                }
                // A measurement observer can cancel, reconfigure or destroy
                // this client. Keep the completion local across that signal.
                const auto completion = callback;
                const QPointer<JsonHttpClient> guard(this);
                emit requestMeasured(lane, measurements);
                if (guard && generation == guard->m_generation) completion(result);
            });
    return true;
}

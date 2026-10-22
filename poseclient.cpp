#include "poseclient.h"
#include "networkpolicy.h"
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <cmath>

namespace {
constexpr qint64 MaxPoseLineBytes = 64 * 1024;
}

PoseClient::PoseClient(QObject *parent) : QObject(parent), m_http(this), m_streamManager(this),
    m_timer(this), m_streamDeadline(this) {
    m_clock.start();
    m_streamManager.setProxy(QNetworkProxy::NoProxy);
    m_streamDeadline.setSingleShot(true);
    connect(&m_streamDeadline, &QTimer::timeout, this, [this] {
        failStream(QStringLiteral("pose_stream_timeout"));
    });
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const bool available = fresh();
        if (available != m_online) {
            m_online = available;
            emit availabilityChanged(available, available ? QString() : tr("位姿过期或不可用于控制"));
        }
        openStream();
        pollCapture();
    });
}
PoseClient::~PoseClient() {
    blockSignals(true);
    stop();
}
void PoseClient::configure(const QUrl &url, const QString &token, int timeout) {
    const bool restart = m_running;
    stop();
    m_http.configure(url, token, timeout);
    m_streamBaseUrl = url;
    m_streamToken = token.trimmed();
    m_streamTimeoutMs = qBound(200, timeout, 10000);
    if (restart)
        start();
}
void PoseClient::start() {
    if (m_running)
        return;
    m_running = true;
    m_timer.start();
    openStream();
}
void PoseClient::stop() {
    m_running = false;
    m_timer.stop();
    m_streamDeadline.stop();
    if (auto *reply = m_streamReply.data()) {
        m_streamReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_streamBuffer.clear();
    m_lineAge.invalidate();
    m_http.cancelAll();
    m_pose = {};
    m_age.invalidate();
    m_captureId.clear();
    m_captureAge.invalidate();
    m_nextPoll = 0;
    m_failures = 0;
    m_online = false;
    emit availabilityChanged(false, tr("位姿服务未连接"));
}
ControlPoseSnapshot PoseClient::snapshot() const {
    auto pose = m_pose;
    const double delay = m_age.isValid() ? m_age.elapsed() : 1e12;
    pose.stateAgeMs += delay;
    pose.positionAgeMs += delay;
    pose.headingAgeMs += delay;
    pose.validForControl = fresh();
    return pose;
}
bool PoseClient::fresh() const {
    return m_running && m_age.isValid() && m_pose.validForControl &&
           m_pose.stateAgeMs + m_age.elapsed() <= m_stateTimeoutMs;
}
void PoseClient::setStateTimeoutMs(int timeoutMs) {
    if (timeoutMs > 0 && timeoutMs <= 1000)
        m_stateTimeoutMs = timeoutMs;
}
void PoseClient::openStream() {
    if (!m_running || !isConfigured() || m_streamReply || m_clock.elapsed() < m_nextPoll)
        return;
    QUrl url = m_streamBaseUrl;
    QString prefix = url.path();
    while (prefix.endsWith('/'))
        prefix.chop(1);
    url.setPath(prefix + QStringLiteral("/pose/stream"));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/x-ndjson");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    NetworkPolicy::applyBearerAuthorization(&request, m_streamToken);
    m_streamBuffer.clear();
    m_lineAge.invalidate();
    m_firstStreamSendTime = m_lastStreamSendTime = 0;
    m_firstStreamReceiveMs = 0;
    auto *reply = m_streamManager.get(request);
    m_streamReply = reply;
    reply->setReadBufferSize(MaxPoseLineBytes + 1);
    m_streamDeadline.start(m_streamTimeoutMs);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (m_streamReply != reply)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            failStream(status == 404 ? tr("位姿服务不支持 /pose/stream 推送接口（HTTP 404）")
                                     : QStringLiteral("pose_stream_http_%1").arg(status));
            return;
        }
        const auto type = reply->header(QNetworkRequest::ContentTypeHeader).toString()
                              .section(';', 0, 0).trimmed().toLower();
        if (type != QStringLiteral("application/x-ndjson")) {
            failStream(QStringLiteral("pose_stream_content_type"));
            return;
        }
    });
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (m_streamReply == reply)
            readStream();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (m_streamReply != reply)
            return;
        if (reply->isOpen())
            readStream();
        if (m_streamReply == reply)
            failStream(QStringLiteral("pose_stream_disconnected"));
    });
}
void PoseClient::readStream() {
    auto *reply = m_streamReply.data();
    if (!reply || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return;
    while (m_streamReply == reply && reply->bytesAvailable() > 0) {
        if (m_streamBuffer.isEmpty())
            m_lineAge.restart();
        const auto bytes = reply->read(qMin(qint64(4096), MaxPoseLineBytes + 1 - m_streamBuffer.size()));
        if (bytes.isEmpty())
            break;
        m_streamBuffer.append(bytes);
        while (m_streamReply == reply) {
            const auto end = m_streamBuffer.indexOf('\n');
            if (end < 0)
                break;
            if (end > MaxPoseLineBytes) {
                failStream(QStringLiteral("pose_stream_line_too_large"));
                return;
            }
            auto line = m_streamBuffer.left(end);
            m_streamBuffer.remove(0, end + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            if (!line.isEmpty()) {
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(line, &error);
                if (error.error != QJsonParseError::NoError || !document.isObject()) {
                    failStream(QStringLiteral("pose_stream_invalid_json"));
                    return;
                }
                if (!acceptPose(document.object(), m_lineAge.elapsed()))
                    return;
                if (m_streamReply != reply)
                    return;
            }
            m_lineAge.restart();
        }
        if (m_streamBuffer.size() > MaxPoseLineBytes) {
            failStream(QStringLiteral("pose_stream_line_too_large"));
            return;
        }
    }
}
void PoseClient::failStream(const QString &reason) {
    if (!m_running || !m_streamReply)
        return;
    auto *reply = m_streamReply.data();
    m_streamReply.clear();
    m_streamDeadline.stop();
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    m_streamBuffer.clear();
    m_age.invalidate();
    m_online = false;
    m_nextPoll = m_clock.elapsed() + qMin(5000, 100 * (1 << qMin(++m_failures, 6)));
    emit availabilityChanged(false, reason);
    emit errorOccurred(reason);
}
bool PoseClient::acceptPose(const QJsonObject &object, qint64 lineAgeMs) {
    ControlPoseSnapshot pose;
    QString error;
    if (!TrackingJson::pose(object, &pose, &error)) {
        failStream(error);
        return false;
    }
    const auto send = object.value("sendTime");
    const double sendTime = send.toDouble();
    if (!send.isDouble() || !std::isfinite(sendTime) || sendTime <= 0) {
        failStream(QStringLiteral("pose_stream_send_time_invalid"));
        return false;
    }
    if (m_lastStreamSendTime > 0 && sendTime < m_lastStreamSendTime) {
        failStream(QStringLiteral("pose_stream_clock_rewound"));
        return false;
    }
    const qint64 localNow = m_clock.elapsed();
    if (m_firstStreamSendTime == 0) {
        m_firstStreamSendTime = sendTime;
        m_firstStreamReceiveMs = localNow - lineAgeMs;
    }
    m_lastStreamSendTime = sendTime;
    // Relative progress needs no agreement between the hosts' wall clocks.
    // A pause in delivery/Qt processing adds age to queued frames; current
    // frames recover as their send times catch up. Initial one-way delay is
    // unknown, and waiting for HTTP headers is not the age of later samples.
    const double sourceProgressMs = (sendTime - m_firstStreamSendTime) * 1000;
    if (!std::isfinite(sourceProgressMs)) {
        failStream(QStringLiteral("pose_stream_send_time_invalid"));
        return false;
    }
    const double transportAgeMs = qMax(double(lineAgeMs),
        qMax(0.0, double(localNow - m_firstStreamReceiveMs) - sourceProgressMs));
    // Replayed sequence numbers never refresh state age, including after a
    // transport reconnect. Boot changes remain visible to coordinate consumers.
    if (pose.bootId == m_pose.bootId && pose.seq <= m_pose.seq)
        return true;
    m_failures = 0;
    m_streamDeadline.start(m_streamTimeoutMs);
    const bool changed = !m_pose.bootId.isEmpty() &&
                         (pose.bootId != m_pose.bootId || pose.originRevision != m_pose.originRevision ||
                          pose.calibrationId != m_pose.calibrationId);
    pose.stateAgeMs += transportAgeMs;
    pose.positionAgeMs += transportAgeMs;
    pose.headingAgeMs += transportAgeMs;
    m_pose = pose;
    m_age.restart();
    if (changed)
        emit frameChanged();
    if (m_running)
        emit poseChanged(snapshot());
    return true;
}
bool PoseClient::capture(const MapFrameBinding &binding, int duration) {
    if (!fresh() || !binding.isUsable() || !m_captureId.isEmpty() || m_http.busy("capture") ||
        binding.context.originRevision != m_pose.originRevision ||
        binding.context.calibrationId != m_pose.calibrationId)
        return false;
    QJsonObject body{{"requestId", TrackingJson::newId()},
                     {"originRevision", m_pose.originRevision},
                     {"calibrationId", m_pose.calibrationId},
                     {"durationMs", qBound(300, duration, 5000)}};
    m_captureAge.restart();
    m_captureBoot = m_pose.bootId;
    return m_http.request("capture", "POST", "/captures", body, {}, {},
                          [this](const JsonHttpResult &r) { acceptCapture(r); });
}
void PoseClient::acceptCapture(const JsonHttpResult &r) {
    if (!r.ok() || r.object["apiVersion"].toInt() != 1 ||
        r.object["fusionBootId"].toString() != m_captureBoot || m_pose.bootId != m_captureBoot) {
        m_captureId.clear();
        m_captureAge.invalidate();
        emit errorOccurred(r.error.isEmpty() ? "capture_version_or_boot_mismatch" : r.error);
        return;
    }
    if (r.object["state"].toString() == "running") {
        m_captureId = r.object["captureId"].toString();
        return;
    }
    m_captureId.clear();
    m_captureAge.invalidate();
    emit captureFinished(r.object);
}
void PoseClient::pollCapture() {
    if (m_captureId.isEmpty())
        return;
    if (m_captureAge.elapsed() > 10000) {
        m_captureId.clear();
        emit errorOccurred("capture_timeout");
        return;
    }
    m_http.request("capture", "GET", "/captures/" + m_captureId, {}, {}, {},
                   [this](const JsonHttpResult &r) { acceptCapture(r); });
}

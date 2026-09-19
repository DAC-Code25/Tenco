#include "poseclient.h"
#include <cmath>

PoseClient::PoseClient(QObject *parent) : QObject(parent) {
    m_clock.start();
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const bool available = fresh();
        if (available != m_online) {
            m_online = available;
            emit availabilityChanged(available, available ? QString() : tr("位姿过期或不可用于控制"));
        }
        poll();
        pollCapture();
    });
}
void PoseClient::configure(const QUrl &url, const QString &token, int timeout) {
    const bool restart = m_running;
    stop();
    m_http.configure(url, token, timeout);
    if (restart)
        start();
}
void PoseClient::start() {
    m_running = true;
    m_timer.start();
    poll();
}
void PoseClient::stop() {
    m_running = false;
    m_timer.stop();
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
void PoseClient::poll() {
    if (!m_running || m_clock.elapsed() < m_nextPoll)
        return;
    QUrlQuery query;
    query.addQueryItem("bootId", m_pose.bootId);
    query.addQueryItem("since", QString::number(m_pose.seq));
    query.addQueryItem("waitMs", "100");
    m_http.request("pose", "GET", "/pose", {}, query, {}, [this](const JsonHttpResult &result) {
        if (!m_running)
            return;
        if (!result.ok()) {
            m_nextPoll = m_clock.elapsed() + qMin(5000, 100 * (1 << qMin(++m_failures, 5)));
            if (m_online) {
                m_online = false;
                emit availabilityChanged(false, result.error);
            }
            m_age.invalidate();
            return;
        }
        m_failures = 0;
        if (result.status == 204)
            return;
        ControlPoseSnapshot pose;
        QString error;
        if (!TrackingJson::pose(result.object, &pose, &error)) {
            m_age.invalidate();
            emit errorOccurred(error);
            return;
        }
        if (pose.bootId == m_pose.bootId && pose.seq <= m_pose.seq)
            return;
        const bool changed = !m_pose.bootId.isEmpty() &&
                             (pose.bootId != m_pose.bootId || pose.originRevision != m_pose.originRevision ||
                              pose.calibrationId != m_pose.calibrationId);
        // RTT is a conservative transport-age allowance; it cannot make old observations fresh.
        pose.stateAgeMs += result.roundTripMs;
        pose.positionAgeMs += result.roundTripMs;
        pose.headingAgeMs += result.roundTripMs;
        m_pose = pose;
        m_age.restart();
        if (changed)
            emit frameChanged();
        emit poseChanged(snapshot());
    });
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

#include "camerasettingsclient.h"

namespace {
QString errorText(const JsonHttpResult &r) {
    if (r.status == 404) return QObject::tr("相机服务尚不支持媒体参数接口，请更新服务。");
    const auto message = r.object.value("message").toString();
    return message.isEmpty() ? r.error : message;
}
}
CameraSettingsClient::CameraSettingsClient(QObject *parent) : QObject(parent) {
    m_poll.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, &CameraSettingsClient::poll);
}
void CameraSettingsClient::configure(const QUrl &url, const QString &token, int timeoutMs) {
    m_poll.stop(); m_http.configure(url, token, timeoutMs);
    m_configuration = {}; m_capabilities = {}; m_applying = false; setBusy(false);
}
void CameraSettingsClient::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy; emit busyChanged(busy);
}
bool CameraSettingsClient::validConfiguration(const QJsonObject &v) {
    const auto state = v.value("applyState").toString();
    return v.value("schemaVersion").toInt() == 1 && !v.value("revision").toString().isEmpty() &&
           v.value("settings").isObject() && (state == "applied" || state == "pending" || state == "rejected");
}
void CameraSettingsClient::finish(bool ok, const QString &message) {
    m_poll.stop(); m_applying = false; setBusy(false); emit finished(ok, message);
}
void CameraSettingsClient::read() {
    if (m_busy) return;
    setBusy(true);
    if (!m_http.request("settings", "GET", "/camera/capabilities", {}, {}, {}, [this](const auto &r) {
        if (!r.ok() || r.object.value("schemaVersion").toInt() != 1) {
            m_configuration = {}; finish(false, r.ok() ? tr("相机能力响应格式无效") : errorText(r)); return;
        }
        m_capabilities = r.object;
        m_http.request("settings", "GET", "/camera/settings", {}, {}, {}, [this](const auto &r) {
            if (!r.ok() || !validConfiguration(r.object)) {
                m_configuration = {}; finish(false, r.ok() ? tr("相机参数响应格式无效") : errorText(r)); return;
            }
            m_configuration = r.object;
            emit loaded(m_configuration, m_capabilities);
            if (m_configuration.value("applyState") == "pending") {
                m_elapsed.start(); m_poll.start(300);
            } else {
                finish(true, tr("已读取工控机当前参数")); refreshStatus();
            }
        }, 65536);
    }, 65536)) finish(false, tr("相机地址无效或请求尚未结束"));
}
bool CameraSettingsClient::apply(const QJsonObject &settings) {
    if (m_busy || !validConfiguration(m_configuration) ||
        !m_configuration.value("cameraConnected").toBool() || m_configuration.value("recording").toBool()) return false;
    m_target = settings; m_expectedRevision = m_configuration.value("revision").toString(); m_targetRevision.clear();
    m_applying = true; setBusy(true); m_elapsed.start();
    const QJsonObject body{{"schemaVersion", 1}, {"expectedRevision", m_expectedRevision}, {"settings", settings}};
    if (!m_http.request("settings", "PUT", "/camera/settings", body, {}, {}, [this](const auto &r) {
        if (!r.ok()) {
            if (r.status == 0 || r.error == "request_timeout" || r.error == "connection_failed") {
                // The write may already be persisted. Resolve by GET; never replay the PUT.
                m_poll.start(200); return;
            }
            m_configuration = {}; finish(false, errorText(r) + tr("；请重新读取工控机参数。")); return;
        }
        if (!validConfiguration(r.object)) { m_poll.start(200); return; }
        m_targetRevision = r.object.value("revision").toString();
        observe(r.object);
    }, 65536)) { finish(false, tr("相机地址无效或请求尚未结束")); return false; }
    return true;
}
void CameraSettingsClient::observe(const QJsonObject &v) {
    m_configuration = v;
    const auto state = v.value("applyState").toString();
    if (m_applying) {
        const bool revisionMatches = m_targetRevision.isEmpty()
            ? v.value("revision").toString() != m_expectedRevision || v.value("settings").toObject() == m_target
            : v.value("revision").toString() == m_targetRevision;
        if (state == "rejected") {
            emit loaded(v, m_capabilities); finish(false, tr("参数应用失败，已恢复原参数：%1").arg(v.value("lastError").toString())); return;
        }
        if (state == "applied" && revisionMatches) {
            emit loaded(v, m_capabilities);
            const bool matches = v.value("activeSettings").toObject() == m_target;
            finish(matches, matches ? tr("参数已在工控机生效并保存") : tr("参数已被其他程序修改，请重新读取。")); return;
        }
    } else if (state != "pending") {
        emit loaded(v, m_capabilities); finish(state == "applied", state == "applied" ? tr("参数已生效") : v.value("lastError").toString()); return;
    }
    if (m_elapsed.elapsed() >= 30000) {
        m_configuration = {}; finish(false, tr("生效结果尚未确认，请稍后重新读取；不会自动重发修改请求。")); return;
    }
    m_poll.start(300);
}
void CameraSettingsClient::poll() {
    if (!m_http.request("settings", "GET", "/camera/settings", {}, {}, {}, [this](const auto &r) {
        if (r.ok() && validConfiguration(r.object)) observe(r.object);
        else if (m_elapsed.elapsed() >= 30000) {
            m_configuration = {}; finish(false, tr("暂时无法确认结果，请重新读取工控机参数。"));
        } else m_poll.start(500);
    }, 65536)) {
        m_configuration = {}; finish(false, tr("无法读取生效状态，请重新读取。"));
    }
}
void CameraSettingsClient::refreshStatus() {
    if (m_busy || m_http.busy("status")) return;
    m_http.request("status", "GET", "/camera/status", {}, {}, {}, [this](const auto &r) {
        if (r.ok()) {
            const auto config = r.object.value("mediaConfiguration").toObject();
            // Refresh readiness/recording without silently replacing the edit revision.
            if (config.value("revision") == m_configuration.value("revision")) {
                m_configuration["cameraConnected"] = config.value("cameraConnected");
                m_configuration["recording"] = config.value("recording");
            }
            emit statusReceived(r.object);
        } else emit statusReceived({});
    }, 65536);
}

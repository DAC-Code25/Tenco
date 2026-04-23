#include "cameracontrolclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

Q_LOGGING_CATEGORY(lcCameraControlClient, "tenco.camera.control")

namespace {
constexpr int kRequestTimeoutMs = 5000;
constexpr auto kPhotoEndpoint = "/camera/photo";
constexpr auto kRecordStartEndpoint = "/camera/record/start";
constexpr auto kRecordStopEndpoint = "/camera/record/stop";
constexpr auto kStatusEndpoint = "/camera/status";
}

CameraControlClient::CameraControlClient(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
    , m_timeoutTimer(new QTimer(this))
{
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &CameraControlClient::handleRequestTimeout);
}

CameraControlClient::~CameraControlClient()
{
    cleanupReply();
}

void CameraControlClient::setBaseUrl(const QUrl &url)
{
    const QString trimmed = url.toString().trimmed();
    m_baseUrl = trimmed.isEmpty() ? QUrl() : QUrl(trimmed);
}

void CameraControlClient::setAuthorizationToken(const QString &token)
{
    m_authToken = token.trimmed();
}

bool CameraControlClient::isConfigured() const
{
    return !m_baseUrl.isEmpty() && m_baseUrl.isValid();
}

bool CameraControlClient::isBusy() const
{
    return m_reply != nullptr;
}

void CameraControlClient::capturePhoto()
{
    sendRequest(Operation::CapturePhoto, QString::fromUtf8(kPhotoEndpoint));
}

void CameraControlClient::startRecording()
{
    sendRequest(Operation::StartRecording, QString::fromUtf8(kRecordStartEndpoint));
}

void CameraControlClient::stopRecording()
{
    sendRequest(Operation::StopRecording, QString::fromUtf8(kRecordStopEndpoint));
}

void CameraControlClient::requestStatus()
{
    sendRequest(Operation::QueryStatus, QString::fromUtf8(kStatusEndpoint), true);
}

void CameraControlClient::handleReplyFinished()
{
    if (!m_reply) {
        return;
    }

    QNetworkReply *reply = m_reply;
    const Operation operation = m_pendingOperation;
    m_timeoutTimer->stop();

    const QByteArray body = reply->readAll();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString message;
    QString path;
    bool successFlag = true;
    bool hasStatusRecording = false;
    bool statusRecording = m_isRecording;
    bool cameraConnected = false;

    const QByteArray trimmedBody = body.trimmed();
    if (!trimmedBody.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(trimmedBody);
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            if (obj.contains(QStringLiteral("success"))) {
                successFlag = obj.value(QStringLiteral("success")).toBool();
            }
            message = obj.value(QStringLiteral("message")).toString().trimmed();
            path = obj.value(QStringLiteral("path")).toString().trimmed();
            if (obj.contains(QStringLiteral("recording"))) {
                hasStatusRecording = true;
                statusRecording = obj.value(QStringLiteral("recording")).toBool();
            }
            if (obj.contains(QStringLiteral("cameraConnected"))) {
                cameraConnected = obj.value(QStringLiteral("cameraConnected")).toBool();
            } else if (obj.contains(QStringLiteral("connected"))) {
                cameraConnected = obj.value(QStringLiteral("connected")).toBool();
            }
        }
    }

    const bool httpSucceeded = httpStatus >= 200 && httpStatus < 300;
    bool requestSucceeded = httpSucceeded && !m_requestTimedOut && reply->error() == QNetworkReply::NoError && successFlag;

    if (!requestSucceeded) {
        if (message.isEmpty()) {
            if (m_requestTimedOut) {
                message = tr("请求超时");
            } else if (reply->error() != QNetworkReply::NoError) {
                message = reply->errorString();
            } else if (httpStatus > 0) {
                message = tr("HTTP %1").arg(httpStatus);
            } else {
                message = tr("远程相机服务无响应");
            }
        }
        qCWarning(lcCameraControlClient) << "Remote camera request failed:" << operationName(operation) << message;
        cleanupReply();
        emit requestFailed(operationName(operation), message);
        return;
    }

    switch (operation) {
    case Operation::CapturePhoto:
        emit photoSaved(path, message);
        break;
    case Operation::StartRecording:
        if (!m_isRecording) {
            m_isRecording = true;
            emit recordingStateChanged(true);
        }
        emit recordingStarted(path, message);
        break;
    case Operation::StopRecording:
        if (m_isRecording) {
            m_isRecording = false;
            emit recordingStateChanged(false);
        }
        emit recordingStopped(path, message);
        break;
    case Operation::QueryStatus:
        if (hasStatusRecording && m_isRecording != statusRecording) {
            m_isRecording = statusRecording;
            emit recordingStateChanged(m_isRecording);
        }
        emit statusReceived(cameraConnected, m_isRecording, message);
        break;
    case Operation::None:
        break;
    }

    qCInfo(lcCameraControlClient) << "Remote camera request succeeded:" << operationName(operation) << message << path;
    cleanupReply();
}

void CameraControlClient::handleRequestTimeout()
{
    if (!m_reply) {
        return;
    }
    m_requestTimedOut = true;
    m_reply->abort();
}

void CameraControlClient::sendRequest(Operation operation, const QString &endpointPath, bool useGet)
{
    const QString opName = operationName(operation);
    if (m_reply) {
        emit requestFailed(opName, tr("上一条相机命令尚未完成"));
        return;
    }
    if (!isConfigured()) {
        emit requestFailed(opName, tr("未配置远程相机控制地址"));
        return;
    }

    const QUrl url = buildEndpointUrl(endpointPath);
    if (!url.isValid()) {
        emit requestFailed(opName, tr("远程相机控制地址无效"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TencoCameraControlClient/1.0"));
    request.setRawHeader("Accept", "application/json");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
    if (!m_authToken.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + m_authToken.toUtf8());
    }

    m_requestTimedOut = false;
    m_pendingOperation = operation;

    if (useGet) {
        m_reply = m_manager->get(request);
    } else {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        m_reply = m_manager->post(request, QByteArrayLiteral("{}"));
    }

    connect(m_reply, &QNetworkReply::finished, this, &CameraControlClient::handleReplyFinished);
    m_timeoutTimer->start(kRequestTimeoutMs);
    emit busyChanged(true);
    qCInfo(lcCameraControlClient) << "Sending remote camera request:" << opName << url;
}

void CameraControlClient::cleanupReply()
{
    if (m_timeoutTimer->isActive()) {
        m_timeoutTimer->stop();
    }

    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        if (m_reply->isRunning()) {
            m_reply->abort();
        }
        m_reply->deleteLater();
        m_reply = nullptr;
    }

    m_requestTimedOut = false;
    m_pendingOperation = Operation::None;
    emit busyChanged(false);
}

QUrl CameraControlClient::buildEndpointUrl(const QString &endpointPath) const
{
    QString base = m_baseUrl.toString().trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + endpointPath);
}

QString CameraControlClient::operationName(Operation operation)
{
    switch (operation) {
    case Operation::CapturePhoto:
        return QStringLiteral("photo");
    case Operation::StartRecording:
        return QStringLiteral("record_start");
    case Operation::StopRecording:
        return QStringLiteral("record_stop");
    case Operation::QueryStatus:
        return QStringLiteral("status");
    case Operation::None:
        break;
    }
    return QStringLiteral("unknown");
}

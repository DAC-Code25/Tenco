#include "rowworkclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

Q_LOGGING_CATEGORY(lcRowWorkClient, "tenco.rowwork.client")

namespace
{
constexpr auto kStatusEndpoint = "/row-work/status";
constexpr auto kPlanEndpoint = "/row-work/plan";
constexpr auto kStartEndpoint = "/row-work/start";
constexpr auto kPauseEndpoint = "/row-work/pause";
constexpr auto kResumeEndpoint = "/row-work/resume";
constexpr auto kStopEndpoint = "/row-work/stop";
constexpr auto kCapturePoseEndpoint = "/row-work/capture-pose";
}

RowWorkClient::RowWorkClient(QObject *parent)
    : QObject(parent)
    , m_manager(new QNetworkAccessManager(this))
    , m_timeoutTimer(new QTimer(this))
    , m_statusPollTimer(new QTimer(this))
{
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, &RowWorkClient::handleRequestTimeout);

    m_statusPollTimer->setSingleShot(false);
    m_statusPollTimer->setInterval(m_statusPollIntervalMs);
    connect(m_statusPollTimer, &QTimer::timeout, this, &RowWorkClient::handleStatusPollTimeout);
}

RowWorkClient::~RowWorkClient()
{
    cleanupReply();
}

void RowWorkClient::setBaseUrl(const QUrl &url)
{
    const QString trimmed = url.toString().trimmed();
    m_baseUrl = trimmed.isEmpty() ? QUrl() : QUrl(trimmed);
}

void RowWorkClient::setAuthorizationToken(const QString &token)
{
    m_authToken = token.trimmed();
}

void RowWorkClient::setStatusPollIntervalMs(int intervalMs)
{
    m_statusPollIntervalMs = qMax(100, intervalMs);
    if (m_statusPollTimer) {
        m_statusPollTimer->setInterval(m_statusPollIntervalMs);
    }
}

void RowWorkClient::setCommandTimeoutMs(int timeoutMs)
{
    m_commandTimeoutMs = qMax(1000, timeoutMs);
}

bool RowWorkClient::isConfigured() const
{
    return !m_baseUrl.isEmpty() && m_baseUrl.isValid();
}

bool RowWorkClient::isBusy() const
{
    return m_reply != nullptr;
}

bool RowWorkClient::isPolling() const
{
    return m_statusPollTimer && m_statusPollTimer->isActive();
}

void RowWorkClient::startStatusPolling()
{
    if (!m_statusPollTimer || !isConfigured()) {
        return;
    }
    if (!m_statusPollTimer->isActive()) {
        m_statusPollTimer->start();
    }
    requestStatus();
}

void RowWorkClient::stopStatusPolling()
{
    if (m_statusPollTimer) {
        m_statusPollTimer->stop();
    }
}

void RowWorkClient::requestStatus()
{
    sendJsonRequest(Operation::QueryStatus, QString::fromUtf8(kStatusEndpoint), QByteArray(), true);
}

void RowWorkClient::requestPlan()
{
    sendJsonRequest(Operation::QueryPlan, QString::fromUtf8(kPlanEndpoint), QByteArray(), true);
}

void RowWorkClient::uploadPlan(const RowWorkPlan &plan)
{
    sendJsonRequest(Operation::UploadPlan,
                    QString::fromUtf8(kPlanEndpoint),
                    QJsonDocument(RowWorkJson::planToJson(plan)).toJson(QJsonDocument::Compact));
}

void RowWorkClient::startRowWork()
{
    sendJsonRequest(Operation::Start, QString::fromUtf8(kStartEndpoint));
}

void RowWorkClient::pauseRowWork()
{
    sendJsonRequest(Operation::Pause, QString::fromUtf8(kPauseEndpoint));
}

void RowWorkClient::resumeRowWork()
{
    sendJsonRequest(Operation::Resume, QString::fromUtf8(kResumeEndpoint));
}

void RowWorkClient::stopRowWork()
{
    sendJsonRequest(Operation::Stop, QString::fromUtf8(kStopEndpoint));
}

void RowWorkClient::capturePose()
{
    sendJsonRequest(Operation::CapturePose, QString::fromUtf8(kCapturePoseEndpoint));
}

void RowWorkClient::handleReplyFinished()
{
    if (!m_reply) {
        return;
    }

    QNetworkReply *reply = m_reply;
    const Operation operation = m_pendingOperation;
    m_timeoutTimer->stop();

    const QByteArray body = reply->readAll();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool httpSucceeded = httpStatus >= 200 && httpStatus < 300;

    QJsonObject obj;
    QString message;
    if (!body.trimmed().isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            obj = doc.object();
            message = obj.value(QStringLiteral("message")).toString().trimmed();
        }
    }

    const bool successFlag = obj.contains(QStringLiteral("success"))
                                 ? obj.value(QStringLiteral("success")).toBool()
                                 : httpSucceeded;
    const bool requestSucceeded =
        httpSucceeded && !m_requestTimedOut && reply->error() == QNetworkReply::NoError && successFlag;

    if (!requestSucceeded) {
        if (message.isEmpty()) {
            if (m_requestTimedOut) {
                message = tr("请求超时");
            } else if (reply->error() != QNetworkReply::NoError) {
                message = reply->errorString();
            } else if (httpStatus > 0) {
                message = tr("HTTP %1").arg(httpStatus);
            } else {
                message = tr("直线作业服务无响应");
            }
        }
        if (operation != Operation::QueryStatus) {
            qCWarning(lcRowWorkClient) << "Row work request failed:" << operationName(operation) << message;
        }
        cleanupReply();
        emit requestFailed(operationName(operation), message);
        return;
    }

    switch (operation) {
    case Operation::QueryStatus: {
        RowWorkStatus status;
        RowWorkJson::statusFromJson(obj, &status);
        if (status.message.isEmpty()) {
            status.message = message;
        }
        emit statusReceived(status);
        break;
    }
    case Operation::QueryPlan: {
        RowWorkPlan plan;
        bool parsed = false;
        if (RowWorkJson::planFromJson(obj, &plan)) {
            parsed = true;
            emit planReceived(plan);
        } else if (obj.contains(QStringLiteral("plan")) && obj.value(QStringLiteral("plan")).isObject()) {
            plan = RowWorkPlan{};
            if (RowWorkJson::planFromJson(obj.value(QStringLiteral("plan")).toObject(), &plan)) {
                parsed = true;
                emit planReceived(plan);
            }
        }
        if (!parsed) {
            emit requestFailed(operationName(operation), tr("作业计划响应格式无效"));
        }
        break;
    }
    case Operation::UploadPlan:
        emit planUploaded(message.isEmpty() ? tr("作业计划已下发") : message);
        emit actionSucceeded(operationName(operation), message.isEmpty() ? tr("作业计划已下发") : message);
        break;
    case Operation::CapturePose: {
        RowWorkPose pose;
        int sampleDurationMs = 0;
        int sampleCount = 0;
        QString responseMessage;
        if (RowWorkJson::capturePoseResponseFromJson(obj, &pose, &sampleDurationMs, &sampleCount, &responseMessage)) {
            emit poseCaptured(pose, sampleDurationMs, sampleCount, responseMessage.isEmpty() ? message : responseMessage);
        } else {
            emit requestFailed(operationName(operation), tr("位姿采样响应格式无效"));
        }
        break;
    }
    case Operation::Start:
    case Operation::Pause:
    case Operation::Resume:
    case Operation::Stop:
        emit actionSucceeded(operationName(operation), message);
        break;
    case Operation::None:
        break;
    }

    if (operation != Operation::QueryStatus) {
        qCInfo(lcRowWorkClient) << "Row work request succeeded:" << operationName(operation) << message;
    }
    cleanupReply();
}

void RowWorkClient::handleRequestTimeout()
{
    if (!m_reply) {
        return;
    }
    m_requestTimedOut = true;
    m_reply->abort();
}

void RowWorkClient::handleStatusPollTimeout()
{
    if (!isBusy()) {
        requestStatus();
    }
}

void RowWorkClient::sendJsonRequest(Operation operation, const QString &endpointPath, const QByteArray &body, bool useGet)
{
    if (m_reply) {
        emit requestFailed(operationName(operation), tr("上一条直线作业请求尚未完成"));
        return;
    }
    if (!isConfigured()) {
        emit requestFailed(operationName(operation), tr("未配置直线作业服务地址"));
        return;
    }

    const QUrl url = buildEndpointUrl(endpointPath);
    if (!url.isValid()) {
        emit requestFailed(operationName(operation), tr("直线作业服务地址无效"));
        return;
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TencoRowWorkClient/1.0"));
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
        m_reply = m_manager->post(request, body.isEmpty() ? QByteArrayLiteral("{}") : body);
    }

    connect(m_reply, &QNetworkReply::finished, this, &RowWorkClient::handleReplyFinished);
    m_timeoutTimer->start(m_commandTimeoutMs);
    emit busyChanged(true);
    if (operation != Operation::QueryStatus) {
        qCInfo(lcRowWorkClient) << "Sending row work request:" << operationName(operation) << url;
    }
}

void RowWorkClient::cleanupReply()
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

QUrl RowWorkClient::buildEndpointUrl(const QString &endpointPath) const
{
    QString base = m_baseUrl.toString().trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + endpointPath);
}

QString RowWorkClient::operationName(Operation operation)
{
    switch (operation) {
    case Operation::QueryStatus:
        return QStringLiteral("status");
    case Operation::QueryPlan:
        return QStringLiteral("plan");
    case Operation::UploadPlan:
        return QStringLiteral("upload_plan");
    case Operation::Start:
        return QStringLiteral("start");
    case Operation::Pause:
        return QStringLiteral("pause");
    case Operation::Resume:
        return QStringLiteral("resume");
    case Operation::Stop:
        return QStringLiteral("stop");
    case Operation::CapturePose:
        return QStringLiteral("capture_pose");
    case Operation::None:
        break;
    }
    return QStringLiteral("unknown");
}

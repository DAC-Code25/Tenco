#include "videoclient.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QLoggingCategory>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcVideoClient, "tenco.net.video")

VideoClient::VideoClient(QObject *parent)
    : QObject(parent)
{
}

VideoClient::~VideoClient()
{
    stop();
    stopRecording();
}

void VideoClient::setStreamUrlTemplate(const QString &templateUrl)
{
    m_streamTemplate = templateUrl.trimmed();
}

QString VideoClient::buildUrlForTopic(const QString &topic) const
{
    if (m_streamTemplate.isEmpty()) {
        return QString();
    }

    QUrl url(m_streamTemplate);
    if (!url.isValid()) {
        return QString();
    }

    QUrlQuery query(url);
    if (query.hasQueryItem(QStringLiteral("topic"))) {
        query.removeAllQueryItems(QStringLiteral("topic"));
    }
    if (!topic.isEmpty()) {
        query.addQueryItem(QStringLiteral("topic"), topic);
    }
    url.setQuery(query);

    return url.toString(QUrl::FullyEncoded);
}

void VideoClient::setStreamUrl(const QUrl &url)
{
    m_url = url;
    m_reconnectAttempt = 0;
}

void VideoClient::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
    if (!m_autoReconnect && m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
}

void VideoClient::setReconnectIntervalMs(int intervalMs)
{
    m_reconnectIntervalMs = std::max(200, intervalMs);
    m_reconnectAttempt = 0;
}

bool VideoClient::isActive() const
{
    return m_reply && m_reply->isRunning();
}

void VideoClient::start()
{
    if (m_url.isEmpty() || !m_url.isValid()) {
        setState(State::Error, tr("未配置/无效的视频流 URL"));
        return;
    }
    if (m_reply) {
        return;
    }

    if (!m_manager) {
        m_manager = new QNetworkAccessManager(this);
    }

    m_buffer.clear();
    m_seenFirstFrame = false;
    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }

    QNetworkRequest req{m_url};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TencoVideoClient/1.0"));
    req.setRawHeader("Accept", "multipart/x-mixed-replace");
    req.setRawHeader("Connection", "keep-alive");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif

    m_reply = m_manager->get(req);
    qCInfo(lcVideoClient) << "Opening stream" << m_url << ", reconnect attempt" << m_reconnectAttempt;
    connect(m_reply, &QNetworkReply::readyRead, this, &VideoClient::handleReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &VideoClient::handleStreamFinished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_reply, &QNetworkReply::errorOccurred, this, &VideoClient::handleStreamError);
#else
    connect(m_reply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error), this, &VideoClient::handleStreamError);
#endif

    setState(State::Connecting, tr("正在连接视频流..."));
}

void VideoClient::stop()
{
    if (m_reconnectTimer && m_reconnectTimer->isActive()) {
        m_reconnectTimer->stop();
    }

    cleanupReply();
    m_buffer.clear();
    setState(State::Stopped, tr("视频流已停止"));
}

void VideoClient::cleanupReply()
{
    if (!m_reply) {
        return;
    }
    disconnect(m_reply, nullptr, this, nullptr);
    if (m_reply->isRunning()) {
        m_reply->abort();
    }
    m_reply->deleteLater();
    m_reply = nullptr;
}

void VideoClient::handleReadyRead()
{
    if (!m_reply || !m_reply->isOpen()) {
        return;
    }

    const QByteArray chunk = m_reply->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    m_buffer.append(chunk);
    if (m_buffer.size() > kMaxBufferSize) {
        m_buffer = m_buffer.right(kMaxBufferSize / 2);
    }

    static const QByteArray kJpegStart("\xFF\xD8", 2);
    static const QByteArray kJpegEnd("\xFF\xD9", 2);

    while (true) {
        int startIndex = m_buffer.indexOf(kJpegStart);
        if (startIndex < 0) {
            m_buffer = m_buffer.right(kMaxBufferSize / 2);
            return;
        }
        if (startIndex > 0) {
            m_buffer.remove(0, startIndex);
            startIndex = 0;
        }

        const int endIndex = m_buffer.indexOf(kJpegEnd, startIndex + kJpegStart.size());
        if (endIndex < 0) {
            return;
        }

        const int frameSize = endIndex - startIndex + kJpegEnd.size();
        const QByteArray frameData = m_buffer.mid(startIndex, frameSize);
        m_buffer.remove(0, startIndex + frameSize);

        QImage image;
        if (!image.loadFromData(frameData, "JPG")) {
            continue;
        }

        if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32) {
            image = image.convertToFormat(QImage::Format_RGB32);
        }

        m_lastFrame = image;
        emit frameReceived(m_lastFrame);

        if (!m_seenFirstFrame) {
            m_seenFirstFrame = true;
            m_reconnectAttempt = 0;
            setState(State::Streaming, tr("视频流已连接"));
        }

        if (m_isRecording) {
            if (m_recordFile && m_recordFile->isOpen()) {
                const qint64 written = m_recordFile->write(frameData);
                if (written != frameData.size()) {
                    setState(State::Error, tr("录像写入失败，已停止录像"));
                    stopRecording();
                }
            } else {
                setState(State::Error, tr("录像文件未就绪，已停止录像"));
                stopRecording();
            }
        }
    }
}

void VideoClient::handleStreamFinished()
{
    qCWarning(lcVideoClient) << "Stream finished";
    cleanupReply();
    setState(State::Stopped, tr("视频流已结束，等待重连..."));
    scheduleReconnect();
}

void VideoClient::handleStreamError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error);
    const QString errStr = m_reply ? m_reply->errorString() : QStringLiteral("unknown");
    qCWarning(lcVideoClient) << "Stream error:" << errStr;
    cleanupReply();
    setState(State::Error, tr("视频流异常：%1").arg(errStr));
    scheduleReconnect();
}

void VideoClient::scheduleReconnect()
{
    if (!m_autoReconnect || m_url.isEmpty() || !m_url.isValid()) {
        return;
    }

    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, &VideoClient::restartStream);
    }

    if (!m_reconnectTimer->isActive()) {
        const int delayMs = currentReconnectDelayMs();
        qCInfo(lcVideoClient) << "Scheduling reconnect in" << delayMs << "ms";
        m_reconnectTimer->start(delayMs);
        ++m_reconnectAttempt;
    }
}

void VideoClient::restartStream()
{
    if (m_reply) {
        return;
    }
    start();
}

int VideoClient::currentReconnectDelayMs() const
{
    const int boundedAttempt = std::clamp(m_reconnectAttempt, 0, 6);
    const int factor = 1 << boundedAttempt;
    const qint64 candidate = static_cast<qint64>(m_reconnectIntervalMs) * factor;
    return static_cast<int>(std::min(candidate, static_cast<qint64>(m_reconnectMaxIntervalMs)));
}

void VideoClient::setState(State state, const QString &message)
{
    if (m_state == state && message.isEmpty()) {
        return;
    }
    m_state = state;
    emit stateChanged(m_state, message);
}

bool VideoClient::startRecording(const QString &directory, QString *outPath)
{
    if (m_isRecording) {
        if (outPath) {
            *outPath = m_recordFilePath;
        }
        return true;
    }

    if (directory.trimmed().isEmpty()) {
        return false;
    }

    QDir dir(directory);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    const QString fileName = QStringLiteral("video_%1.mjpeg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString fullPath = dir.filePath(fileName);

    auto *file = new QFile(fullPath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        file->deleteLater();
        return false;
    }

    m_recordFile = file;
    m_recordFilePath = fullPath;
    m_isRecording = true;

    if (outPath) {
        *outPath = m_recordFilePath;
    }
    return true;
}

QString VideoClient::stopRecording()
{
    const QString savedPath = m_recordFilePath;

    m_isRecording = false;
    m_recordFilePath.clear();

    if (m_recordFile) {
        if (m_recordFile->isOpen()) {
            m_recordFile->flush();
            m_recordFile->close();
        }
        m_recordFile->deleteLater();
        m_recordFile = nullptr;
    }

    return savedPath;
}

bool VideoClient::saveSnapshot(const QString &directory, QString *outPath) const
{
    if (m_lastFrame.isNull()) {
        return false;
    }
    if (directory.trimmed().isEmpty()) {
        return false;
    }

    QDir dir(directory);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    const QString fileName = QStringLiteral("photo_%1.jpg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString fullPath = dir.filePath(fileName);
    if (!m_lastFrame.save(fullPath, "JPG", 90)) {
        return false;
    }

    if (outPath) {
        *outPath = fullPath;
    }
    return true;
}

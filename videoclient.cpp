#include "videoclient.h"

#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QLoggingCategory>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcVideoClient, "tenco.net.video")

VideoClient::VideoClient(QObject *parent)
    : QObject(parent)
{
    initializeFrameWorker();
}

VideoClient::~VideoClient()
{
    stop();
    stopRecording();
    if (m_frameWorker) {
        QMetaObject::invokeMethod(m_frameWorker, "reset", Qt::BlockingQueuedConnection);
    }
    if (m_frameThread) {
        m_frameThread->quit();
        m_frameThread->wait();
    }
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

void VideoClient::setMaxDisplayFps(int fps)
{
    m_workerSettings.maxDisplayFps = qBound(1, fps, 120);
    if (m_frameWorker) {
        const VideoFrameWorker::Settings settings = m_workerSettings;
        QMetaObject::invokeMethod(m_frameWorker, [worker = m_frameWorker, settings]() {
            worker->configure(settings);
        }, Qt::QueuedConnection);
    }
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

    resetFrameWorkerStream();
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
    if (m_isRecording) {
        stopRecording();
    }
    resetFrameWorkerStream();
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
    if (!m_frameWorker) {
        return;
    }
    QMetaObject::invokeMethod(m_frameWorker, [worker = m_frameWorker, chunk]() {
        worker->enqueueBytes(chunk);
    }, Qt::QueuedConnection);
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

void VideoClient::initializeFrameWorker()
{
    if (m_frameWorker) {
        return;
    }

    qRegisterMetaType<VideoFrameWorker::Metrics>("VideoFrameWorker::Metrics");
    m_frameThread = new QThread(this);
    m_frameThread->setObjectName(QStringLiteral("VideoFrameWorkerThread"));
    m_frameWorker = new VideoFrameWorker;
    m_frameWorker->configure(m_workerSettings);
    m_frameWorker->moveToThread(m_frameThread);

    connect(m_frameThread, &QThread::finished, m_frameWorker, &QObject::deleteLater);
    connect(m_frameWorker, &VideoFrameWorker::frameReady, this, [this](const QImage &frame) {
        m_lastFrame = frame;
        if (!m_seenFirstFrame) {
            m_seenFirstFrame = true;
            m_reconnectAttempt = 0;
            setState(State::Streaming, tr("视频流已连接"));
        }
        emit frameReceived(m_lastFrame);
    });
    connect(m_frameWorker, &VideoFrameWorker::workerError, this, [this](const QString &message) {
        m_isRecording = false;
        m_recordFilePath.clear();
        setState(State::Error, message);
    });
    m_frameThread->start();
}

void VideoClient::resetFrameWorkerStream()
{
    m_lastFrame = QImage();
    if (!m_frameWorker) {
        return;
    }
    if (QThread::currentThread() == m_frameWorker->thread()) {
        m_frameWorker->resetStream();
        return;
    }
    QMetaObject::invokeMethod(m_frameWorker, "resetStream", Qt::BlockingQueuedConnection);
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
    if (!m_frameWorker) {
        return false;
    }

    QString path;
    bool ok = false;
    auto startWorkerRecording = [&]() {
        ok = m_frameWorker->startRecording(directory, &path);
    };
    if (QThread::currentThread() == m_frameWorker->thread()) {
        startWorkerRecording();
    } else {
        QMetaObject::invokeMethod(m_frameWorker, startWorkerRecording, Qt::BlockingQueuedConnection);
    }
    if (!ok) {
        return false;
    }

    m_recordFilePath = path;
    m_isRecording = true;
    if (outPath) {
        *outPath = m_recordFilePath;
    }
    return true;
}

QString VideoClient::stopRecording()
{
    QString savedPath;
    if (m_frameWorker) {
        auto stopWorkerRecording = [&]() {
            savedPath = m_frameWorker->stopRecording();
        };
        if (QThread::currentThread() == m_frameWorker->thread()) {
            stopWorkerRecording();
        } else {
            QMetaObject::invokeMethod(m_frameWorker, stopWorkerRecording, Qt::BlockingQueuedConnection);
        }
    } else {
        savedPath = m_recordFilePath;
    }

    m_isRecording = false;
    m_recordFilePath.clear();

    return savedPath;
}

bool VideoClient::saveSnapshot(const QString &directory, QString *outPath) const
{
    QImage frame = m_lastFrame;
    if (m_frameWorker) {
        auto readLastFrame = [&]() {
            frame = m_frameWorker->lastFrame();
        };
        if (QThread::currentThread() == m_frameWorker->thread()) {
            readLastFrame();
        } else {
            QMetaObject::invokeMethod(m_frameWorker, readLastFrame, Qt::BlockingQueuedConnection);
        }
    }
    if (frame.isNull()) {
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
    if (!frame.save(fullPath, "JPG", 90)) {
        return false;
    }

    if (outPath) {
        *outPath = fullPath;
    }
    return true;
}

QImage VideoClient::lastFrame() const
{
    if (!m_frameWorker) {
        return m_lastFrame;
    }
    QImage frame;
    auto readLastFrame = [&]() {
        frame = m_frameWorker->lastFrame();
    };
    if (QThread::currentThread() == m_frameWorker->thread()) {
        readLastFrame();
    } else {
        QMetaObject::invokeMethod(m_frameWorker, readLastFrame, Qt::BlockingQueuedConnection);
    }
    return frame.isNull() ? m_lastFrame : frame;
}

VideoFrameWorker::Metrics VideoClient::metrics() const
{
    if (!m_frameWorker) {
        return VideoFrameWorker::Metrics{};
    }
    VideoFrameWorker::Metrics snapshot;
    auto readMetrics = [&]() {
        snapshot = m_frameWorker->metrics();
    };
    if (QThread::currentThread() == m_frameWorker->thread()) {
        readMetrics();
    } else {
        QMetaObject::invokeMethod(m_frameWorker, readMetrics, Qt::BlockingQueuedConnection);
    }
    return snapshot;
}

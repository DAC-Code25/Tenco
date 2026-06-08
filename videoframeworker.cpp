#include "videoframeworker.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>

#include <QtGlobal>

namespace {
const QByteArray kJpegStart("\xFF\xD8", 2);
const QByteArray kJpegEnd("\xFF\xD9", 2);

QString timestampForFileName()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"));
}
} // namespace

VideoFrameWorker::VideoFrameWorker(QObject *parent)
    : QObject(parent)
{
}

VideoFrameWorker::~VideoFrameWorker()
{
    stopRecording();
}

void VideoFrameWorker::configure(const Settings &settings)
{
    QMutexLocker locker(&m_mutex);
    m_settings.maxDisplayFps = qBound(kMinDisplayFps, settings.maxDisplayFps, kMaxDisplayFps);
    m_settings.maxBufferBytes = qBound(kMinBufferBytes, settings.maxBufferBytes, kMaxBufferBytesLimit);
    if (m_buffer.size() > m_settings.maxBufferBytes) {
        m_buffer = m_buffer.right(m_settings.maxBufferBytes / 2);
    }
}

VideoFrameWorker::Settings VideoFrameWorker::settings() const
{
    QMutexLocker locker(&m_mutex);
    return m_settings;
}

bool VideoFrameWorker::startRecording(const QString &directory, QString *outPath)
{
    if (outPath) {
        outPath->clear();
    }

    QMutexLocker locker(&m_mutex);
    if (m_recording) {
        if (outPath) {
            *outPath = m_recordFilePath;
        }
        return true;
    }

    if (directory.trimmed().isEmpty()) {
        return false;
    }

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return false;
    }

    const QString fileName = QStringLiteral("video_%1.mjpeg").arg(timestampForFileName());
    const QString fullPath = dir.filePath(fileName);
    auto *file = new QFile(fullPath);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        return false;
    }

    m_recordFile = file;
    m_recordFilePath = fullPath;
    m_recording = true;
    if (outPath) {
        *outPath = m_recordFilePath;
    }
    return true;
}

QString VideoFrameWorker::stopRecording()
{
    QFile *file = nullptr;
    QString savedPath;
    {
        QMutexLocker locker(&m_mutex);
        savedPath = m_recordFilePath;
        file = m_recordFile;
        m_recordFile = nullptr;
        m_recordFilePath.clear();
        m_recording = false;
    }

    if (file) {
        if (file->isOpen()) {
            file->flush();
            file->close();
        }
        delete file;
    }
    return savedPath;
}

bool VideoFrameWorker::isRecording() const
{
    QMutexLocker locker(&m_mutex);
    return m_recording;
}

QImage VideoFrameWorker::lastFrame() const
{
    QMutexLocker locker(&m_mutex);
    return m_lastFrame;
}

VideoFrameWorker::Metrics VideoFrameWorker::metrics() const
{
    QMutexLocker locker(&m_mutex);
    return m_metrics;
}

void VideoFrameWorker::enqueueBytes(const QByteArray &chunk)
{
    if (chunk.isEmpty()) {
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_buffer.append(chunk);
        m_metrics.bytesReceived += static_cast<quint64>(chunk.size());
        if (m_buffer.size() > m_settings.maxBufferBytes) {
            m_buffer = m_buffer.right(m_settings.maxBufferBytes / 2);
        }
    }

    processBuffer();
}

void VideoFrameWorker::reset()
{
    stopRecording();
    resetStream();
}

void VideoFrameWorker::resetStream()
{
    QMutexLocker locker(&m_mutex);
    m_buffer.clear();
    m_lastFrame = QImage();
    m_metrics = Metrics{};
    m_lastDisplayMs = 0;
}

void VideoFrameWorker::processBuffer()
{
    while (true) {
        QByteArray frameData;
        {
            QMutexLocker locker(&m_mutex);
            int startIndex = m_buffer.indexOf(kJpegStart);
            if (startIndex < 0) {
                if (m_buffer.size() > m_settings.maxBufferBytes / 2) {
                    m_buffer = m_buffer.right(m_settings.maxBufferBytes / 2);
                }
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
            frameData = m_buffer.mid(startIndex, frameSize);
            m_buffer.remove(0, startIndex + frameSize);
            ++m_metrics.framesReceived;
        }

        handleJpegFrame(frameData);
    }
}

void VideoFrameWorker::handleJpegFrame(const QByteArray &frameData)
{
    QElapsedTimer decodeTimer;
    decodeTimer.start();

    QImage image;
    if (!image.loadFromData(frameData, "JPG")) {
        {
            QMutexLocker locker(&m_mutex);
            ++m_metrics.decodeFailures;
        }
        emitMetrics();
        return;
    }

    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32) {
        image = image.convertToFormat(QImage::Format_RGB32);
    }

    const double decodeMs = static_cast<double>(decodeTimer.nsecsElapsed()) / 1000000.0;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool displayFrame = false;
    {
        QMutexLocker locker(&m_mutex);
        ++m_metrics.framesDecoded;
        m_metrics.averageDecodeMs +=
            (decodeMs - m_metrics.averageDecodeMs) / static_cast<double>(m_metrics.framesDecoded);
        m_metrics.lastFrameEpochMs = nowMs;
        m_lastFrame = image;
        displayFrame = shouldDisplayFrame(nowMs);
        if (displayFrame) {
            ++m_metrics.framesDisplayed;
            m_lastDisplayMs = nowMs;
        } else {
            ++m_metrics.framesDropped;
        }
    }

    writeRecordingFrame(frameData);
    emitMetrics();

    if (displayFrame) {
        emit frameReady(image);
    }
}

bool VideoFrameWorker::shouldDisplayFrame(qint64 nowMs) const
{
    if (m_lastDisplayMs <= 0) {
        return true;
    }
    const int intervalMs = qMax(1, 1000 / qBound(kMinDisplayFps, m_settings.maxDisplayFps, kMaxDisplayFps));
    return nowMs - m_lastDisplayMs >= intervalMs;
}

void VideoFrameWorker::writeRecordingFrame(const QByteArray &frameData)
{
    QFile *file = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_recording || !m_recordFile) {
            return;
        }
        file = m_recordFile;
    }

    if (!file->isOpen() || file->write(frameData) != frameData.size()) {
        const QString message = tr("录像写入失败，已停止录像");
        stopRecording();
        emit workerError(message);
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        ++m_metrics.recordingFrames;
    }
}

void VideoFrameWorker::emitMetrics()
{
    emit metricsUpdated(metrics());
}

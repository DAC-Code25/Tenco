#include "mjpegvideosource.h"

#include <QUrlQuery>

MjpegVideoSource::MjpegVideoSource(QObject *parent)
    : AbstractVideoSource(parent)
{
    connect(&m_client, &VideoClient::frameReceived, this, [this](const QImage &frame) {
        emit frameReceived(frame);
    });
    connect(&m_client, &VideoClient::stateChanged, this, [this](VideoClient::State state, const QString &message) {
        emit stateChanged(mapState(state), message);
    });
}

void MjpegVideoSource::start()
{
    m_client.start();
}

void MjpegVideoSource::stop()
{
    m_client.stop();
}

bool MjpegVideoSource::isActive() const
{
    return m_client.isActive();
}

bool MjpegVideoSource::isConfigured() const
{
    return !m_client.streamUrl().isEmpty() && m_client.streamUrl().isValid();
}

bool MjpegVideoSource::startRecording(const QString &directory, QString *outPath)
{
    return m_client.startRecording(directory, outPath);
}

QString MjpegVideoSource::stopRecording()
{
    return m_client.stopRecording();
}

bool MjpegVideoSource::isRecording() const
{
    return m_client.isRecording();
}

bool MjpegVideoSource::saveSnapshot(const QString &directory, QString *outPath) const
{
    return m_client.saveSnapshot(directory, outPath);
}

QImage MjpegVideoSource::lastFrame() const
{
    return m_client.lastFrame();
}

void MjpegVideoSource::setStreamUrlTemplate(const QString &templateUrl)
{
    m_client.setStreamUrlTemplate(templateUrl);
    const QUrl url(templateUrl.trimmed());
    m_supportsTopics = url.isValid() && QUrlQuery(url).hasQueryItem(QStringLiteral("topic"));
}

QString MjpegVideoSource::buildUrlForTopic(const QString &topic) const
{
    return m_client.buildUrlForTopic(topic);
}

void MjpegVideoSource::setStreamUrl(const QUrl &url)
{
    m_client.setStreamUrl(url);
}

QUrl MjpegVideoSource::streamUrl() const
{
    return m_client.streamUrl();
}

void MjpegVideoSource::setReconnectIntervalMs(int intervalMs)
{
    m_client.setReconnectIntervalMs(intervalMs);
}

void MjpegVideoSource::setAutoReconnect(bool enabled)
{
    m_client.setAutoReconnect(enabled);
}

void MjpegVideoSource::setMaxDisplayFps(int fps)
{
    m_client.setMaxDisplayFps(fps);
}

bool MjpegVideoSource::supportsTopics() const
{
    return m_supportsTopics;
}

AbstractVideoSource::State MjpegVideoSource::mapState(VideoClient::State state)
{
    switch (state) {
    case VideoClient::State::Stopped:
        return AbstractVideoSource::State::Stopped;
    case VideoClient::State::Connecting:
        return AbstractVideoSource::State::Connecting;
    case VideoClient::State::Streaming:
        return AbstractVideoSource::State::Streaming;
    case VideoClient::State::Error:
        return AbstractVideoSource::State::Error;
    }
    return AbstractVideoSource::State::Error;
}

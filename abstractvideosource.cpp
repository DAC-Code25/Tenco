#include "abstractvideosource.h"

AbstractVideoSource::AbstractVideoSource(QObject *parent)
    : QObject(parent)
{
}

AbstractVideoSource::~AbstractVideoSource() = default;

void AbstractVideoSource::setStreamUrlTemplate(const QString &templateUrl)
{
    Q_UNUSED(templateUrl);
}

QString AbstractVideoSource::buildUrlForTopic(const QString &topic) const
{
    Q_UNUSED(topic);
    return QString();
}

void AbstractVideoSource::setStreamUrl(const QUrl &url)
{
    Q_UNUSED(url);
}

QUrl AbstractVideoSource::streamUrl() const
{
    return QUrl();
}

void AbstractVideoSource::setReconnectIntervalMs(int intervalMs)
{
    Q_UNUSED(intervalMs);
}

void AbstractVideoSource::setAutoReconnect(bool enabled)
{
    Q_UNUSED(enabled);
}

bool AbstractVideoSource::supportsTopics() const
{
    return false;
}

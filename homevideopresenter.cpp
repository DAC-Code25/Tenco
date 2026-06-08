#include "homevideopresenter.h"

#include <QLabel>
#include <QPixmap>
#include <Qt>

HomeVideoPresenter::HomeVideoPresenter(QLabel *label)
{
    setLabel(label);
}

void HomeVideoPresenter::setLabel(QLabel *label)
{
    m_label = label;
    if (!m_label) {
        return;
    }
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setScaledContents(m_scaleContents);
}

bool HomeVideoPresenter::isReady() const
{
    return m_label != nullptr;
}

void HomeVideoPresenter::setScaleContents(bool enabled)
{
    m_scaleContents = enabled;
    if (m_label) {
        m_label->setScaledContents(m_scaleContents);
    }
}

void HomeVideoPresenter::showPlaceholder(const QString &message)
{
    if (!m_label) {
        return;
    }
    m_label->setScaledContents(m_scaleContents);
    m_label->setPixmap(QPixmap());
    m_label->setText(message);
}

void HomeVideoPresenter::showFrame(const QImage &image)
{
    if (!m_label || image.isNull()) {
        return;
    }

    QImage frame = image;
    if (!m_scaleContents && !m_label->size().isEmpty()) {
        frame = image.scaled(m_label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    m_label->setText(QString());
    m_label->setPixmap(QPixmap::fromImage(frame));
    m_lastFrame = image;
}

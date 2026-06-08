#ifndef HOMEVIDEOPRESENTER_H
#define HOMEVIDEOPRESENTER_H

#include <QImage>
#include <QString>

class QLabel;

class HomeVideoPresenter
{
public:
    explicit HomeVideoPresenter(QLabel *label = nullptr);

    void setLabel(QLabel *label);
    bool isReady() const;

    void setScaleContents(bool enabled);
    bool scaleContents() const { return m_scaleContents; }

    void showPlaceholder(const QString &message);
    void showFrame(const QImage &image);
    QImage lastFrame() const { return m_lastFrame; }

private:
    QLabel *m_label = nullptr;
    bool m_scaleContents = true;
    QImage m_lastFrame;
};

#endif // HOMEVIDEOPRESENTER_H

#include "homevideopresenter.h"

#include <QLabel>
#include <QPixmap>
#include <QtTest>

class HomeVideoPresenterTest : public QObject
{
    Q_OBJECT

private slots:
    void placeholderInvalidatesThePreviousFrame()
    {
        QLabel label;
        HomeVideoPresenter presenter(&label);
        QImage frame(4, 3, QImage::Format_RGB32);
        frame.fill(Qt::green);
        presenter.showFrame(frame);
        QCOMPARE(presenter.lastFrame(), frame);
        QVERIFY(!label.pixmap().isNull());

        presenter.showPlaceholder(QStringLiteral("视频已断开"));
        QVERIFY(presenter.lastFrame().isNull());
        QVERIFY(label.pixmap().isNull());
        QCOMPARE(label.text(), QStringLiteral("视频已断开"));

        frame.fill(Qt::blue);
        presenter.showFrame(frame);
        QCOMPARE(presenter.lastFrame(), frame);
        QVERIFY(!label.pixmap().isNull());
        QVERIFY(label.text().isEmpty());
    }

    void placeholderInvalidatesCacheWithoutAnAttachedLabel()
    {
        QLabel label;
        HomeVideoPresenter presenter(&label);
        QImage frame(4, 3, QImage::Format_RGB32);
        frame.fill(Qt::green);
        presenter.showFrame(frame);
        presenter.setLabel(nullptr);
        presenter.showPlaceholder(QStringLiteral("视频已停止"));
        QVERIFY(presenter.lastFrame().isNull());
    }
};

QTEST_MAIN(HomeVideoPresenterTest)
#include "test_homevideopresenter.moc"

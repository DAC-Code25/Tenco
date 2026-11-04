#include "buttonfocusstyle.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QStyleFactory>
#include <QTest>
#include <QVBoxLayout>

namespace {
class FocusMarkerStyle final : public QProxyStyle {
public:
    FocusMarkerStyle() : QProxyStyle(QStyleFactory::create("Windows")) {}
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override {
        if (element == PE_FrameFocusRect) {
            painter->fillRect(option->rect, QColor(255, 0, 255));
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};
bool containsMarker(const QImage &image) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y) == QColor(255, 0, 255)) return true;
    return false;
}
QImage focusFrame(QWidget &widget) {
    QImage image(80, 30, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    QStyleOptionFocusRect option;
    option.initFrom(&widget);
    option.rect = image.rect();
    QApplication::style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, &widget);
    return image;
}
}

class ButtonFocusStyleTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QApplication::setStyle(new ButtonFocusStyle(new FocusMarkerStyle)); }

    void mouseClickKeepsFocusWithoutInnerFrameAndTabRemainsVisible() {
        QWidget page;
        auto *layout = new QVBoxLayout(&page);
        auto *first = new QPushButton("拍照", &page);
        auto *second = new QPushButton("录像", &page);
        first->setFocusPolicy(Qt::StrongFocus);
        second->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(first); layout->addWidget(second);
        page.show(); page.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&page));
        QTest::mouseClick(first, Qt::LeftButton);
        QVERIFY(first->hasFocus());
        QVERIFY(!containsMarker(focusFrame(*first)));
        QVERIFY(!containsMarker(first->grab().toImage()));
        QTest::keyClick(first, Qt::Key_Tab);
        QVERIFY(second->hasFocus());
        QVERIFY(containsMarker(focusFrame(*second)));
        QVERIFY(containsMarker(second->grab().toImage()));
        QTest::keyClick(second, Qt::Key_Backtab);
        QVERIFY(first->hasFocus());
        QVERIFY(containsMarker(focusFrame(*first)));
        QTest::mouseClick(second, Qt::LeftButton);
        QVERIFY(second->hasFocus());
        QVERIFY(!containsMarker(focusFrame(*second)));
        QVERIFY(!containsMarker(second->grab().toImage()));
    }

    void nonButtonFocusFramesArePreserved() {
        QWidget editor;
        QVERIFY(containsMarker(focusFrame(editor)));
    }

    void styledButtonRetainsKeyboardBorderAndActivation() {
        QWidget page;
        auto *layout = new QVBoxLayout(&page);
        auto *first = new QPushButton("前一项", &page);
        auto *capture = new QPushButton("拍照", &page);
        capture->setStyleSheet("QPushButton {background:white;border:2px solid transparent;}"
                              "QPushButton:focus {border:2px solid #24775c;}");
        layout->addWidget(first); layout->addWidget(capture);
        page.show(); page.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&page));
        QTest::mouseClick(first, Qt::LeftButton);
        QTest::keyClick(first, Qt::Key_Tab);
        QVERIFY(capture->hasFocus());
        const auto image = capture->grab().toImage();
        QCOMPARE(image.pixelColor(image.width() / 2, 0), QColor("#24775c"));
        QSignalSpy activated(capture, &QPushButton::clicked);
        QTest::keyClick(capture, Qt::Key_Space);
        QCOMPARE(activated.count(), 1);
        QTest::mouseClick(capture, Qt::LeftButton);
        QVERIFY(capture->hasFocus());
        QVERIFY(!containsMarker(capture->grab().toImage()));
    }
};

QTEST_MAIN(ButtonFocusStyleTest)
#include "test_buttonfocusstyle.moc"

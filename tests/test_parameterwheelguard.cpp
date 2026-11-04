#include "parameterwheelguard.h"

#include <QAbstractItemView>
#include <QDoubleSpinBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTest>
#include <QVBoxLayout>

namespace {
void wheelOn(QWidget *target, const QPoint &angle = QPoint(0, -120), const QPoint &pixels = {}, bool inverted = false) {
    const QPoint local = target->rect().center();
    QWheelEvent wheel(local, target->mapToGlobal(local), pixels, angle,
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, inverted);
    QApplication::sendEvent(target, &wheel);
}
struct ParameterPage {
    QWidget window;
    QScrollArea *scroll = nullptr;
    QWidget *content = nullptr;
    QVBoxLayout *form = nullptr;
    ParameterPage() {
        window.resize(600, 320);
        auto *layout = new QVBoxLayout(&window);
        scroll = new QScrollArea(&window);
        scroll->setWidgetResizable(true);
        layout->addWidget(scroll);
        content = new QWidget;
        content->setMinimumHeight(1200);
        form = new QVBoxLayout(content);
        form->setAlignment(Qt::AlignTop);
        scroll->setWidget(content);
        ParameterWheelGuard::install(&window);
    }
    void show() {
        window.show();
        window.activateWindow();
        QTest::qWait(40);
    }
};
}

class ParameterWheelGuardTest : public QObject {
    Q_OBJECT
private slots:
    void focusedAndUnfocusedEditorsScrollWithoutChanging_data() {
        QTest::addColumn<bool>("focused");
        QTest::addColumn<bool>("lineEditor");
        QTest::addColumn<bool>("pixelScroll");
        QTest::newRow("unfocused-spin") << false << false << false;
        QTest::newRow("focused-spin") << true << false << false;
        QTest::newRow("focused-line-editor") << true << true << false;
        QTest::newRow("pixel-scroll") << true << true << true;
    }
    void focusedAndUnfocusedEditorsScrollWithoutChanging() {
        QFETCH(bool, focused); QFETCH(bool, lineEditor); QFETCH(bool, pixelScroll);
        ParameterPage page;
        auto *spin = new QDoubleSpinBox(page.content); // Created after the guard.
        spin->setRange(0, 100); spin->setValue(42.5);
        page.form->addWidget(spin);
        page.show();
        if (focused) spin->setFocus(Qt::MouseFocusReason);
        else page.scroll->setFocus();
        QSignalSpy changed(spin, &QDoubleSpinBox::valueChanged);
        auto *target = lineEditor ? static_cast<QWidget *>(spin->findChild<QLineEdit *>()) : spin;
        QVERIFY(target);
        wheelOn(target, pixelScroll ? QPoint() : QPoint(0, -120), pixelScroll ? QPoint(0, -55) : QPoint());
        QCOMPARE(spin->value(), 42.5);
        QCOMPARE(changed.count(), 0);
        QVERIFY(page.scroll->verticalScrollBar()->value() > 0);
        if (focused) QVERIFY(spin->hasFocus());
    }
    void closedComboScrollsPageButExpandedListStillScrolls() {
        ParameterPage page;
        auto *combo = new QComboBox(page.content);
        combo->setMaxVisibleItems(8);
        for (int i = 0; i < 100; ++i) combo->addItem(QString("Option %1").arg(i));
        combo->setCurrentIndex(0);
        page.form->addWidget(combo);
        page.show();
        combo->setFocus(Qt::MouseFocusReason);
        QSignalSpy changed(combo, &QComboBox::currentIndexChanged);
        wheelOn(combo);
        QCOMPARE(combo->currentIndex(), 0);
        QCOMPARE(changed.count(), 0);
        QVERIFY(page.scroll->verticalScrollBar()->value() > 0);
        page.scroll->verticalScrollBar()->setValue(0);
        combo->showPopup();
        QTRY_VERIFY(combo->view()->isVisible());
        auto *list = combo->view();
        QTRY_VERIFY(list->verticalScrollBar()->maximum() > 0);
        const int pagePosition = page.scroll->verticalScrollBar()->value();
        wheelOn(list->viewport());
        QTRY_VERIFY(list->verticalScrollBar()->value() > 0);
        QCOMPARE(page.scroll->verticalScrollBar()->value(), pagePosition);
        QTest::keyClick(list, Qt::Key_End);
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(combo->currentIndex(), 99);
    }
    void pixelScrollUsesPlatformDirection_data() {
        QTest::addColumn<bool>("inverted");
        QTest::addColumn<int>("delta");
        QTest::newRow("normal-down") << false << -45;
        QTest::newRow("normal-up") << false << 45;
        QTest::newRow("natural-down") << true << -45;
        QTest::newRow("natural-up") << true << 45;
    }
    void pixelScrollUsesPlatformDirection() {
        QFETCH(bool, inverted); QFETCH(int, delta);
        ParameterPage page;
        auto *spin = new QSpinBox(page.content);
        spin->setValue(10); page.form->addWidget(spin);
        page.show();
        page.scroll->verticalScrollBar()->setValue(100);
        wheelOn(spin, {}, QPoint(0, delta), inverted);
        QCOMPARE(spin->value(), 10);
        QCOMPARE(page.scroll->verticalScrollBar()->value(), 100 - delta);
    }
    void nestedScrollBoundaryPassesRemainingDistanceOutward_data() {
        QTest::addColumn<bool>("pixels");
        QTest::newRow("touchpad") << true;
        QTest::newRow("mouse-wheel") << false;
    }
    void nestedScrollBoundaryPassesRemainingDistanceOutward() {
        QFETCH(bool, pixels);
        ParameterPage page;
        auto *inner = new QScrollArea(page.content);
        inner->setWidgetResizable(true); inner->setFixedHeight(180);
        auto *innerContent = new QWidget;
        innerContent->setMinimumHeight(500);
        auto *innerLayout = new QVBoxLayout(innerContent);
        innerLayout->addStretch();
        auto *spin = new QSpinBox(innerContent);
        spin->setValue(10); innerLayout->addWidget(spin);
        inner->setWidget(innerContent); page.form->addWidget(inner);
        page.show();
        QVERIFY(inner->verticalScrollBar()->maximum() > 0);
        const int maximum=inner->verticalScrollBar()->maximum();
        inner->verticalScrollBar()->setValue(pixels ? maximum-10 : maximum);
        wheelOn(spin, pixels ? QPoint() : QPoint(0,-120), pixels ? QPoint(0,-45) : QPoint());
        QCOMPARE(spin->value(), 10);
        QCOMPARE(inner->verticalScrollBar()->value(), maximum);
        QVERIFY(page.scroll->verticalScrollBar()->value() > 0);
        if (pixels) QCOMPARE(page.scroll->verticalScrollBar()->value(), 35);
    }
    void keyboardEditingAndOtherWindowsAreUnaffected() {
        ParameterPage page;
        auto *spin = new QSpinBox(page.content);
        spin->setRange(0, 100); spin->setValue(50);
        page.form->addWidget(spin);
        page.show();
        spin->setFocus(Qt::TabFocusReason);
        QTest::keyClick(spin, Qt::Key_Up);
        QCOMPARE(spin->value(), 51);
        QSpinBox unrelated;
        unrelated.setRange(0, 100); unrelated.setValue(50);
        wheelOn(&unrelated);
        QCOMPARE(unrelated.value(), 49);
        // No scroll area is also protected, as in the origin dialog.
        QWidget dialog;
        auto *dialogLayout = new QVBoxLayout(&dialog);
        auto *origin = new QDoubleSpinBox(&dialog);
        origin->setRange(-90, 90); origin->setValue(30.2);
        dialogLayout->addWidget(origin);
        ParameterWheelGuard::install(&dialog);
        wheelOn(origin);
        QCOMPARE(origin->value(), 30.2);
    }
};

QTEST_MAIN(ParameterWheelGuardTest)
#include "test_parameterwheelguard.moc"

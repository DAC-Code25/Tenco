#pragma once

#include <QAbstractButton>
#include <QCoreApplication>
#include <QFocusEvent>
#include <QProxyStyle>
#include <QStyleOption>
#include <QVariant>

// Pointer focus already has the button's normal hover/focus border. Keep the
// native inner focus frame for keyboard navigation, where it is useful.
class ButtonFocusStyle final : public QProxyStyle {
public:
    explicit ButtonFocusStyle(QStyle *base = nullptr) : QProxyStyle(base) {
        if (auto *app = QCoreApplication::instance()) app->installEventFilter(this);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override {
        if (element == PE_FrameFocusRect && option && qobject_cast<const QAbstractButton *>(widget)) {
            const auto keyboardFocus = widget->property("tencoKeyboardButtonFocus");
            if (!(keyboardFocus.isValid() ? keyboardFocus.toBool() :
                  option->state.testFlag(State_KeyboardFocusChange))) return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (auto *button = qobject_cast<QAbstractButton *>(object)) {
            if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin) {
                // Qt may retain State_KeyboardFocusChange after a later click,
                // including a click on the button that already owns focus.
                button->setProperty("tencoKeyboardButtonFocus", false);
                button->update();
            } else if (event->type() == QEvent::FocusIn) {
                const auto reason = static_cast<QFocusEvent *>(event)->reason();
                if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason)
                    button->setProperty("tencoKeyboardButtonFocus", true);
                else if (reason == Qt::MouseFocusReason || reason == Qt::OtherFocusReason)
                    button->setProperty("tencoKeyboardButtonFocus", false);
            }
        }
        return QProxyStyle::eventFilter(object, event);
    }
};

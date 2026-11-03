#pragma once

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QObject>
#include <QPointer>
#include <QScrollBar>
#include <QWheelEvent>

// A maintenance parameter is changed by typing, keys or an explicit selection,
// never by browsing the page with the mouse wheel. The application filter also
// covers editors created later from a remote parameter whitelist.
class ParameterWheelGuard final : public QObject {
public:
    static void install(QWidget *root) {
        if (!root || root->property("parameterWheelGuardInstalled").toBool()) return;
        root->setProperty("parameterWheelGuardInstalled", true);
        auto *guard = new ParameterWheelGuard(root);
        qApp->installEventFilter(guard);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Wheel || !m_root) return false;
        auto *target = qobject_cast<QWidget *>(watched);
        QWidget *editor = nullptr;
        for (auto *current = target; current; current = current->parentWidget()) {
            if (qobject_cast<QAbstractSpinBox *>(current) || qobject_cast<QComboBox *>(current)) {
                editor = current;
                break;
            }
            if (!editor && qobject_cast<QLineEdit *>(current)) editor = current;
            if (current == m_root) break;
        }
        if (!editor || !(editor == m_root || m_root->isAncestorOf(editor))) return false;
        if (auto *combo = qobject_cast<QComboBox *>(editor)) {
            auto *view = combo->view();
            if (view->isVisible() && (target == view || view->isAncestorOf(target))) return false;
        }

        auto *wheel = static_cast<QWheelEvent *>(event);
        QPoint remainingPixels = wheel->pixelDelta();
        for (auto *parent = editor->parentWidget(); parent; parent = parent->parentWidget()) {
            auto *scroll = qobject_cast<QAbstractScrollArea *>(parent);
            if (!scroll) continue;
            if (!wheel->pixelDelta().isNull()) {
                // QScrollBar on Windows ignores pixel-only touchpad events.
                // Pixels already include the platform's natural-scroll sign;
                // applying inverted() again would reverse the user's gesture.
                const auto consume = [](QScrollBar *bar, int &remaining) {
                    const int before = bar->value();
                    bar->setValue(before - remaining);
                    remaining += bar->value() - before;
                };
                consume(scroll->horizontalScrollBar(), remainingPixels.rx());
                consume(scroll->verticalScrollBar(), remainingPixels.ry());
                if (remainingPixels.isNull()) break;
                continue; // Pass only the unconsumed distance to an outer page.
            }
            const auto canScroll = [](QScrollBar *bar, int delta) {
                return delta < 0 ? bar->value() < bar->maximum() :
                    delta > 0 && bar->value() > bar->minimum();
            };
            if (!canScroll(scroll->horizontalScrollBar(), wheel->angleDelta().x()) &&
                !canScroll(scroll->verticalScrollBar(), wheel->angleDelta().y())) continue;
            QWheelEvent forwarded(scroll->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),
                wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(),
                wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source(), wheel->pointingDevice());
            QCoreApplication::sendEvent(scroll->viewport(), &forwarded);
            if (forwarded.isAccepted()) break;
        }
        wheel->accept();
        return true;
    }

private:
    explicit ParameterWheelGuard(QWidget *root) : QObject(root), m_root(root) {}
    QPointer<QWidget> m_root;
};

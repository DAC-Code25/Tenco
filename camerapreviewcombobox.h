#pragma once

#include <QComboBox>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyleOptionComboBox>
#include <QStylePainter>

class CameraPreviewItemDelegate final : public QStyledItemDelegate {
public:
    explicit CameraPreviewItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QSize size = QStyledItemDelegate::sizeHint(option, index);
        return {qMax(size.width(), 96), qMax(size.height(), 48)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const bool enabled = item.state.testFlag(QStyle::State_Enabled);
        const bool selected = enabled && item.state.testFlag(QStyle::State_Selected);
        const bool hovered = enabled && item.state.testFlag(QStyle::State_MouseOver);
        const QRect row = item.rect.adjusted(0, 2, 0, -2);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(selected ? QColor("#d5d0f3") : QColor(Qt::transparent));
        painter->setBrush(selected ? QColor("#eceafd") : hovered ? QColor("#f6f5ff") : QColor(Qt::transparent));
        painter->drawRoundedRect(row.adjusted(1, 1, -1, -1), 7, 7);
        painter->setFont(item.font);
        painter->setPen(!enabled ? QColor("#a7a3b8") : selected ? QColor("#514bb2") : QColor("#45416d"));
        const QRect textArea = row.adjusted(12, 0, -12, 0);
        painter->drawText(textArea, Qt::AlignCenter,
            item.fontMetrics.elidedText(item.text, Qt::ElideRight, textArea.width()));
        painter->restore();
    }
};

// Keep native combo-box interaction and popup behavior while centering the
// preview label inside the area reserved separately from the arrow.
class CameraPreviewComboBox final : public QComboBox {
public:
    explicit CameraPreviewComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}

protected:
    void paintEvent(QPaintEvent *) override {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QString label = option.currentText;
        option.currentText.clear();
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
        QRect textArea = style()->subControlRect(QStyle::CC_ComboBox, &option,
            QStyle::SC_ComboBoxEditField, this);
        if (!option.currentIcon.isNull()) textArea.adjust(option.iconSize.width() + 4, 0, 0, 0);
        painter.setPen(option.palette.color(QPalette::Text));
        painter.drawText(textArea, Qt::AlignCenter,
            fontMetrics().elidedText(label, Qt::ElideRight, textArea.width()));
    }
};

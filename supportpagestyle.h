#pragma once

#include <QString>
#include <QWidget>

// Shared presentation for maintenance, help and about; scoped to each page root.
namespace SupportPageStyle {
inline QString base() {
    return QStringLiteral(R"(
QWidget { font-family: "Microsoft YaHei"; font-size:13px; color:#30493d; }
QWidget[supportPage="true"] { background:#f4f6f5; }
QLabel { background:transparent; }
QLabel[supportSectionTitle="true"] { color:#183a2c; font-size:16px; font-weight:600; }
QLabel[supportCaption="true"] { color:#718278; font-size:12px; }
QFrame[supportCard="true"] { background:white; border:1px solid #dfe7e3; border-radius:12px; }
QTabWidget::pane { border:none; background:transparent; }
QTabBar::tab { background:transparent; color:#718278; padding:11px 14px;
    border-bottom:2px solid transparent; margin-right:4px; }
QTabBar::tab:hover { color:#24775c; background:#edf5f0; }
QTabBar::tab:selected { color:#18563c; font-weight:600; border-bottom-color:#24775c; }
QScrollArea { background:transparent; border:none; }
QScrollArea > QWidget > QWidget { background:transparent; }
QScrollBar:vertical { background:transparent; width:8px; margin:2px; }
QScrollBar::handle:vertical { background:#c5d2cb; min-height:36px; border-radius:3px; }
QScrollBar::handle:vertical:hover { background:#9eafa5; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:none; }
QPushButton { background:white; color:#385346; border:1px solid #d3ded7;
    border-radius:8px; padding:8px 14px; min-height:20px; font-weight:500; }
QPushButton:hover { background:#edf5f0; border-color:#a4bfaf; }
QPushButton:pressed { background:#e1eee6; }
QPushButton:focus { border-color:#24775c; }
QPushButton:disabled { background:#f0f3f1; color:#93a198; border-color:#e1e7e3; }
QPushButton[supportPrimary="true"] { background:#24775c; color:white; border-color:#24775c; }
QPushButton[supportPrimary="true"]:hover { background:#1e664e; }
QPushButton[supportPrimary="true"]:disabled { background:#dce9e1; color:#8da798; border-color:#dce9e1; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background:white; border:1px solid #d3ded7; border-radius:7px;
    padding:7px 10px; min-height:20px; selection-background-color:#24775c; selection-color:white; }
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus { border-color:#24775c; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width:16px; border:none; }
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled { background:#f4f6f5; color:#8c9b92; }
QComboBox::drop-down { width:26px; border:none; border-left:1px solid #e4eae6; }
QComboBox::down-arrow { image:url(:/image/support-chevron-down.svg); width:16px; height:16px; }
QComboBox QAbstractItemView { background:white; selection-background-color:#e4f0e9; selection-color:#184c37; }
QCheckBox { spacing:8px; color:#385346; min-height:24px; }
QListWidget[supportNavigation="true"] { background:transparent; border:none; outline:none; font-size:14px; }
QListWidget[supportNavigation="true"]::item { padding:10px 12px; margin:2px 0; border-radius:8px; }
QListWidget[supportNavigation="true"]::item:hover { background:#f0f5f2; }
QListWidget[supportNavigation="true"]::item:selected { background:#e3f0e8; color:#18563c; font-weight:600; }
QToolTip { color:#30493d; background:#ffffff; border:1px solid #d3ded7; padding:6px; }
)");
}
inline void apply(QWidget *root, const QString &pageStyle = {}) {
    root->setProperty("supportPage", true);
    root->setStyleSheet(base() + pageStyle);
}
} // namespace SupportPageStyle

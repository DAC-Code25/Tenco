#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QPushButton>
#include <QStringList>
#include <QGlobalStatic>
#include <QString>
#include <QLineEdit>
#include <QComboBox>
#include <QListView>
#include <QGridLayout>
#include <QLabel>

namespace {
QString cameraActionStyle(const QString &background, const QString &hover, const QString &pressed)
{
    return QStringLiteral(
        "QPushButton { background:%1; color:white; border:2px solid transparent; border-radius:8px;"
        " font-family:'Microsoft YaHei'; font-size:17px; font-weight:600; }"
        "QPushButton:hover { background:%2; }"
        "QPushButton:pressed, QPushButton:checked { background:%3; }"
        "QPushButton:disabled { background:#e5ebe8; color:#88978f; border-color:#dfe6e1; }")
        .arg(background, hover, pressed);
}
}

// 集中式样式管理
//运动方向控制
static QString BUTTON_STYLE3() {
    return    QStringLiteral(
        "QPushButton {"
        " min-width: 80px;"
        " min-height: 80px;"
        " background-color:#f4f8f6;"
        " border:1px solid #dbe4de;"
        " border-radius:12px;"
        "}"
        "QPushButton:hover {"
        "    background-color:#e6f0e9;"

        "}"
        "QPushButton:pressed {"
        "   background-color:#d6e7dc;"
        "}"
        );
}
//急停按钮
static QString BUTTON_STYLE4() {
    return    QStringLiteral(
        "QPushButton {"
        " background-color: #e74c3c;"
        " min-width: 80px;"
        " min-height: 80px;"
        " border: 2px solid white;"
        " border-radius: 42px;"
        " font-size: 24px;"
        " font-weight: bold;"
        "font-family: Times New Roman; "
        "}"
        "QPushButton:hover {"
        "  background-color: #c0392b;"
        " border: 2px solid  white;"
        "}"
        "QPushButton:pressed {"
        " background-color: #a93226;"
        " border: 2px solid #e67e22;"
        "}"
        );
}
//云台控制按钮
static QString BUTTON_STYLE5() {
    return    QStringLiteral(
        "QPushButton {"
        "background-color: #b6b6b6;"
        " border-radius: 40px;"
        " font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "  background-color: #a8a8a8;"
        "  border: 2px solid #ffffff;"
        "}"
        "QPushButton:pressed {"
        " background-color: #7d7d7d;"
        "border: 3px solid #ffffff;"
        "}"
        );
}
//视频区域
static QString VIDEO_STYLE() {
    return    QStringLiteral(
        "QLabel {"
        "  background-color: black;"
        "color: #ecf0f1;"
        " border-radius: 8px;"
        " border: 2px solid #7f8c8d;"
        "  font-size: 16px;"
        "font-weight: bold;"
        "}"
        );
}
//首页信息框
static QString LINEEDIT_STYLE1() {
    return    QStringLiteral(
        "QLineEdit {"
        "border: 2px ridge #c4c4c4;"
        "border-radius: 5px;"
        "font-size: 16px;"
        "font-weight: bold;"
        "font-family: Times New Roman;"
        "}"
        );
}


//确定按钮
static QString BUTTON_STYLE6(){
    return    QStringLiteral(
        "QPushButton {"
        "   border-radius: 5px;"
        "   border: none;"
        "   color: white;"
        "   background-color: #e08af9;"
        "   font: 700 10pt Segoe Script;"
        "}"
        "QPushButton:hover {"
        "   background-color: #d358f7;"
        "}"
        "QPushButton:pressed {"
        "   background-color: #c519ff;"
        "}"
        );
}
//重启按钮
static QString BUTTON_STYLE8(){
    return    QStringLiteral(
        "QPushButton {"
        "background-color: #8ae0f9;"
        "color: #550000;"
        "border-radius: 10px;"
        "border: 1px solid white;"
        "font-size: 18px;"
        "min-width: 80px;"
        "min-height: 50px;"
        "font-family: 微软雅黑;"
        "}"
        "QPushButton:hover {"
        "  background-color: #69d7f8;"
        "  border: 2px solid #ffffff;"
        "}"
        "QPushButton:pressed {"
        " background-color: #00bfff;"
        "border: 3px solid #ffffff;"
        "}"
        );
}
//导航按钮
static QString BUTTON_STYLE9(){
    return    QStringLiteral(
        "QPushButton {"
        "border: none;"
        "color: #ecf0f1;"
        "font-size: 16px;"
        "font-weight: bold;"
        "padding: 15px 25px;"
        "border-radius: 5px;"
        "margin: 5px;"
        "font-family: 微软雅黑;"
        "}"
        "QPushButton:hover {"
        " background-color: #34495e;"
        "}"
        "QPushButton:pressed {"
        " background-color: #1abc9c;"
        "}"
        " QPushButton:checked {"
        "background-color: #1abc9c;"
        "color: white;"
        "}"
        );
}
// //窗口控制按钮
// static QString BUTTON_STYLE10(){
//     return    QStringLiteral(
//         "QPushButton {"
//         " background-color: transparent;"
//         "margin: 5px;"
//         " border: none;"
//         "}"
//         "QPushButton:hover {"
//          // "background-color: #effbef;"
//         "}"
//         "QPushButton:pressed {"
//         " background-color: #848484;"
//         "}"
//         );
// }
//导航栏
static QString QWidget_STYLE(){
    return    QStringLiteral(
        "QWidget {"
        "background-color: #2c3e50;"
        "}"
        );
}


void MainWindow::initUIComponents()
{
    // 1. 集中设置样式
    //按钮
    ui->recordButton->setStyleSheet(cameraActionStyle("#a85c18", "#914c10", "#753c0b"));
    ui->captureButton->setStyleSheet(cameraActionStyle("#24775c", "#1e664e", "#18543f"));
    ui->videoPreviewButton->setStyleSheet(cameraActionStyle("#246f95", "#1b5c7e", "#174c68"));
    // The four camera actions share one compact button row. Override the
    // legacy button padding/minimum so the row fits the 1200 px main window.
    const QString cameraActionMetrics = QStringLiteral(
        "QPushButton { min-width:0; min-height:0; padding:0 8px; border:2px solid transparent; }"
        "QPushButton:hover, QPushButton:focus { border:2px solid rgba(255,255,255,190); }"
        "QPushButton:pressed { border:2px solid white; }");
    for (auto *button : {ui->recordButton, ui->captureButton, ui->videoPreviewButton}) {
        button->setStyleSheet(button->styleSheet() + cameraActionMetrics);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumWidth(96);
        button->setFixedHeight(64);
    }
    ui->cameraPreviewKind->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    ui->cameraPreviewKind->setMinimumWidth(96);
    ui->cameraPreviewKind->setFixedHeight(64);
    ui->cameraPreviewKind->setCursor(Qt::PointingHandCursor);
    auto *previewMenu = new QListView(ui->cameraPreviewKind);
    previewMenu->setSpacing(2);
    previewMenu->setMouseTracking(true);
    previewMenu->viewport()->setAttribute(Qt::WA_Hover);
    previewMenu->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->cameraPreviewKind->setView(previewMenu);
    previewMenu->setItemDelegate(new CameraPreviewItemDelegate(previewMenu));
    ui->cameraPreviewKind->setStyleSheet(QStringLiteral(
        "QComboBox { background:#7b78d6; color:white; border:2px solid transparent;"
        " border-radius:8px; padding:0 9px; min-height:0; min-width:0;"
        " font-size:18px; font-weight:600; }"
        "QComboBox:hover, QComboBox:focus { background:#6c68c9; border:2px solid rgba(255,255,255,190); }"
        "QComboBox:on { background:#5e59b7; border:2px solid white; }"
        "QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right;"
        " border:none; width:26px; }"
        "QComboBox::down-arrow { image:url(:/image/camera-chevron-down.svg); width:14px; height:14px; }"
        "QComboBox QAbstractItemView { background:white; color:#45416d; border:1px solid #dedcf1;"
        " border-radius:10px; padding:7px; font-size:16px; font-weight:600; outline:none;"
        " selection-background-color:#eceafd; selection-color:#514bb2; }"));
    ui->forwardButton->setStyleSheet(BUTTON_STYLE3());
    ui->leftButton->setStyleSheet(BUTTON_STYLE3());
    ui->rightButton->setStyleSheet(BUTTON_STYLE3());
    ui->backwardButton->setStyleSheet(BUTTON_STYLE3());
    ui->stopButton->setStyleSheet(BUTTON_STYLE4());
    ui->pushButton_3->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_4->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_5->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_6->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_7->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_8->setStyleSheet(BUTTON_STYLE5());
    ui->pushButton_9->setStyleSheet(BUTTON_STYLE6()); //确定按钮
    ui->pushButton_restart->setStyleSheet(BUTTON_STYLE8());
    ui->homeButton->setStyleSheet(BUTTON_STYLE9());//导航栏按钮
    ui->mapButton->setStyleSheet(BUTTON_STYLE9());
    ui->maintenanceButton->setStyleSheet(BUTTON_STYLE9());
    ui->helpButton->setStyleSheet(BUTTON_STYLE9());
    ui->aboutButton->setStyleSheet(BUTTON_STYLE9());
    // ui->zhiding->setStyleSheet(BUTTON_STYLE10());
    // ui->suoxiao->setStyleSheet(BUTTON_STYLE10());
    // ui->fangda->setStyleSheet(BUTTON_STYLE10());
    // ui->guanbi->setStyleSheet(BUTTON_STYLE10());


    //视频
    ui->videoDisplay->setStyleSheet(VIDEO_STYLE());

    //信息框

    ui->lineEdit_BattryState->setStyleSheet(LINEEDIT_STYLE1());
    ui->lineEdit_BattryTemp->setStyleSheet(LINEEDIT_STYLE1());
    ui->lineEdit_BattryVol->setStyleSheet(LINEEDIT_STYLE1());
    ui->lineEdit_Position->setStyleSheet(LINEEDIT_STYLE1());
    ui->lineEdit_RunTime->setStyleSheet(LINEEDIT_STYLE1());

    //widget
    if (QLineEdit *lineEdit = ui->doubleSpinBox->findChild<QLineEdit *>()) {
        lineEdit->setReadOnly(true);
        lineEdit->setFocusPolicy(Qt::NoFocus);
    }
    ui->doubleSpinBox->setFocusPolicy(Qt::NoFocus);
    if (QLineEdit *lineEdit = ui->doubleSpinBox_2->findChild<QLineEdit *>()) {
        lineEdit->setReadOnly(true);
        lineEdit->setFocusPolicy(Qt::NoFocus);
    }
    ui->doubleSpinBox_2->setFocusPolicy(Qt::NoFocus);

    ui->navBar->setStyleSheet(QWidget_STYLE());




    //设置属性
    ui->lineEdit_BattryState->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_BattryState->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->lineEdit_RunTime->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_RunTime->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->lineEdit_BattryVol->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_BattryVol->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->lineEdit_BattryTemp->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_BattryTemp->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->lineEdit_Position->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_Position->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    for (auto *field : {ui->homeLocalizationState, ui->homeTaskState, ui->homeTaskProgress}) {
        field->setReadOnly(true);
        field->setFocusPolicy(Qt::NoFocus);
    }

    ui->plainTextEdit->setReadOnly(true);     // 禁止编辑
    ui->plainTextEdit->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->doubleSpinBox->setValue(0.20); //初始值
    ui->doubleSpinBox->setSingleStep(0.1);   // 每次增减0.1
    ui->doubleSpinBox->setSuffix("    直行速度");
    ui->doubleSpinBox->setStyleSheet(QString());
    ui->doubleSpinBox->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
    ui->doubleSpinBox_2->setValue(0.20);
    ui->doubleSpinBox_2->setSingleStep(0.1);
    ui->doubleSpinBox_2->setSuffix("    转向速度");
    ui->doubleSpinBox_2->setStyleSheet(QString());
    ui->doubleSpinBox_2->setButtonSymbols(QAbstractSpinBox::UpDownArrows);

    // Scope the homepage presentation to its cards; borders must not cascade
    // onto every child widget (the original unqualified styles did so).
    ui->homePage->setStyleSheet(QStringLiteral(
        "QWidget#homePage { background:#f4f6f5; }"
        "QWidget#homePage QLabel { color:#385346; font-family:'Microsoft YaHei'; font-size:13px; }"));
    for (auto *card : {ui->widget_top, ui->widget_info, ui->widget_2, ui->widget_3, ui->controlwidget}) {
        card->setStyleSheet(QStringLiteral(
            "QWidget#%1 { background:white; border:1px solid #dfe7e3; border-radius:9px; }")
            .arg(card->objectName()));
    }
    for (auto *child : {ui->widget_speed, ui->widget_4, ui->widget_switch, ui->widget_6})
        child->setStyleSheet(QStringLiteral("QWidget#%1 { background:transparent; border:none; }")
            .arg(child->objectName()));
    for (auto *label : {ui->label_10, ui->label_3, ui->label_4, ui->label_9,
                       ui->label_8, ui->label_11, ui->label, ui->label_2}) {
        label->setTextFormat(Qt::PlainText);
        label->setStyleSheet(QStringLiteral(
            "QLabel { background:transparent; border:none; color:#52665b;"
            " font-family:'Microsoft YaHei'; font-size:13px; }"));
    }
    ui->label_10->setText(tr("电池状态"));
    ui->label_3->setText(tr("运行时长"));
    ui->label->setText(tr("定位状态"));
    ui->lable_battray->setStyleSheet(QStringLiteral(
        "QLabel { background:transparent; border:none; color:black; font-family:'Times New Roman';"
        " font-size:14pt; font-weight:bold; }"));
    // Both coordinates share a column; the maintenance action must not shorten
    // only the longitude field while latitude uses an unrelated spacer width.
    auto *originGrid = new QGridLayout;
    originGrid->setContentsMargins(0, 0, 0, 0);
    originGrid->setHorizontalSpacing(4);
    originGrid->setVerticalSpacing(4);
    for (auto *row : {ui->horizontalLayout_12, ui->horizontalLayout_16}) {
        while (auto *item = row->takeAt(0)) delete item;
        ui->verticalLayout_7->removeItem(row);
        delete row;
    }
    originGrid->addWidget(ui->label_8, 0, 0);
    originGrid->addWidget(ui->lineEdit, 0, 1);
    originGrid->addWidget(ui->pushButton_9, 0, 2);
    originGrid->addWidget(ui->label_11, 1, 0);
    originGrid->addWidget(ui->lineEdit_8, 1, 1);
    originGrid->setColumnStretch(1, 1);
    ui->verticalLayout_7->addLayout(originGrid);
    const QString readoutStyle = QStringLiteral(
        "QLineEdit { background:#f5f8f6; color:#30493d; border:1px solid #dfe7e3;"
        " border-radius:5px; padding:4px 6px; font-family:'Microsoft YaHei'; font-size:13px; }");
    for (auto *field : {ui->lineEdit_BattryState, ui->lineEdit_BattryTemp, ui->lineEdit_BattryVol,
                       ui->lineEdit_Position, ui->lineEdit_RunTime, ui->homeLocalizationState,
                       ui->homeTaskState, ui->homeTaskProgress, ui->lineEdit, ui->lineEdit_8})
        field->setStyleSheet(readoutStyle);
    const QString secondaryAction = QStringLiteral(
        "QPushButton { background:#edf5f0; color:#24775c; border:1px solid #c8ddcf;"
        " border-radius:6px; padding:5px 7px; font-family:'Microsoft YaHei'; font-size:12px; }"
        "QPushButton:hover { background:#e0eee5; border-color:#90b59e; }"
        "QPushButton:pressed { background:#d5e7dc; }"
        "QPushButton:disabled { color:#93a198; background:#f0f3f1; border-color:#e1e7e3; }");
    ui->pushButton_9->setStyleSheet(secondaryAction);
    ui->pushButton_restart->setStyleSheet(secondaryAction);
    ui->pushButton_restart->setMinimumSize(88, 38);
    ui->label_12->setTextFormat(Qt::RichText);
    ui->label_12->setText(tr("<span style='font-size:24pt'>m/s </span>"
                           "<span style='font-size:14pt'>速度</span>"));
    ui->label_12->setAlignment(Qt::AlignBottom | Qt::AlignLeft);
    ui->label_12->setStyleSheet(QStringLiteral(
        "QLabel { border:none; background:transparent; color:black; }"));
    ui->lcdNumber->setStyleSheet(QStringLiteral("color:black; border:none; background:transparent;"));
    ui->doubleSpinBox->setPrefix(tr("直行 "));
    ui->doubleSpinBox->setSuffix(tr(" m/s"));
    ui->doubleSpinBox_2->setPrefix(tr("转向 "));
    ui->doubleSpinBox_2->setSuffix(tr(" rad/s"));
    const QString manualSpeedStyle = QStringLiteral(
        "QDoubleSpinBox { background:#f5f8f6; color:#30493d; border:1px solid #d3ded7;"
        " border-radius:5px; padding:5px 18px 5px 5px; font-family:'Microsoft YaHei'; font-size:12px; }"
        "QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { subcontrol-origin:border;"
        " width:18px; height:14px; border:none; border-left:1px solid #d3ded7; background:#edf3ef; }"
        "QDoubleSpinBox::up-button { subcontrol-position:top right; }"
        "QDoubleSpinBox::down-button { subcontrol-position:bottom right; }"
        "QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background:#dcebe2; }"
        "QDoubleSpinBox::up-arrow { image:url(:/image/support-chevron-up.svg); width:12px; height:12px; }"
        "QDoubleSpinBox::down-arrow { image:url(:/image/support-chevron-down.svg); width:12px; height:12px; }");
    ui->doubleSpinBox->setStyleSheet(manualSpeedStyle);
    ui->doubleSpinBox_2->setStyleSheet(manualSpeedStyle);
    ui->horizontalLayout_23->setSpacing(12);
    ui->horizontalLayout_19->setSpacing(12);
    ui->horizontalLayout_13->setSpacing(6);
    ui->plainTextEdit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#f6f8f7; color:#52665b; border:1px solid #e1e7e3;"
        " border-radius:6px; padding:5px; font-family:'Microsoft YaHei'; font-size:11px; }"));
    const QString gimbalActionStyle = secondaryAction + QStringLiteral(
        "QPushButton { padding:5px 0; font-size:12px; }"
        "QPushButton:disabled { color:#93a198; }");
    for (auto *button : {ui->pushButton_3, ui->pushButton_4, ui->pushButton_5,
                         ui->pushButton_6, ui->pushButton_7, ui->pushButton_8})
        button->setStyleSheet(gimbalActionStyle);
    ui->pushButton_3->setText(tr("升高"));
    ui->pushButton_4->setText(tr("降低"));
    ui->stopButton->setText(tr("手动\n停止"));
    ui->stopButton->setStyleSheet(BUTTON_STYLE4() + QStringLiteral(
        "QPushButton { color:white; font-family:'Microsoft YaHei'; font-size:18px; }"));
















    // // 1. 重置所有显示数据的控件为初始状态
    // ui->txtVoltage->setText("");          // 清空电压
    // ui->txtBattery->setText("");         // 清空电量
    // ui->txtMode->setText("");            // 清空模式
    // ui->txtCoordinate->setText("");      // 清空坐标
    // ui->txtMapName->setText("");         // 清空地图名称

    // //2. 集中设置文本框
    // ui->txtAgvIp->setText("192.168.31.7");
    // ui->txtAgvPort->setText("9999");
    // ui->txtRushTime->setText("100ms");

    // ui->txtVoltage->setReadOnly(true);      // 禁止编辑
    // ui->txtVoltage->setFocusPolicy(Qt::NoFocus); // 禁用焦点
    // ui->txtVoltage->setPlaceholderText("NULL");

    // ui->txtMode->setReadOnly(true);      // 禁止编辑
    // ui->txtMode->setFocusPolicy(Qt::NoFocus); // 禁用焦点
    // ui->txtMode->setPlaceholderText("NULL");

    // ui->txtCoordinate->setReadOnly(true);      // 禁止编辑
    // ui->txtCoordinate->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    // ui->txtMapName->setReadOnly(true);      // 禁止编辑
    // ui->txtMapName->setFocusPolicy(Qt::NoFocus); // 禁用焦点
    // ui->txtMapName->setPlaceholderText("NULL");

    // ui->txtAgvIp->setReadOnly(true);      // 禁止编辑
    // ui->txtAgvIp->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    // ui->txtAgvPort->setReadOnly(true);      // 禁止编辑
    // ui->txtAgvPort->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    // ui->txtRushTime->setReadOnly(true);      // 禁止编辑
    // ui->txtRushTime->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    // ui->txtBattery->setReadOnly(true);      // 禁止编辑
    // ui->txtBattery->setFocusPolicy(Qt::NoFocus); // 禁用焦点
    // ui->txtBattery->setPlaceholderText("NULL");

    // ui->txtBasePosition->setReadOnly(true);      // 禁止编辑
    // ui->txtBasePosition->setFocusPolicy(Qt::NoFocus); // 禁用焦点

}

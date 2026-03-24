#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QPushButton>
#include <QStringList>
#include <QGlobalStatic>
#include <QString>
#include <QLineEdit>

// 集中式样式管理
//录像按钮
static QString BUTTON_STYLE() {
    return    QStringLiteral(
        "QPushButton {"
        " background-color: #fea443;"
        " color: white;"
        " border: none;"
        " padding: 12px 24px;"
        " border-radius: 8px;"
        " font-size: 18px;"
        " font-weight: bold;"
        " min-width: 100px;"
        " min-height: 40px;"
        " qproperty-iconSize: 24px 24px;"
        "}"
        "QPushButton:hover {"
        " background-color: #ff8000;"
        " border: 2px solid #ffffff;"
        "}"
        "QPushButton:pressed {"
        " background-color: #b45f04;"
        " border: 3px solid #ffffff;"
        "}"
        );
}
//拍照按钮
static QString BUTTON_STYLE1() {
    return    QStringLiteral(
        "QPushButton {"
        " background-color: #b7e533;"
        " color: white;"
        " border: none;"
        " padding: 12px 24px;"
        " border-radius: 8px;"
        " font-size: 18px;"
        " font-weight: bold;"
        " min-width: 100px;"
        " min-height: 40px;"
        " qproperty-iconSize: 24px 24px;"
        "}"
        "QPushButton:hover {"
        " background-color: #92bc1d;"
        " border: 2px solid #ffffff;"
        "}"
        "QPushButton:pressed {"
        " background-color: #688a08;"
        " border: 3px solid #ffffff;"
        "}"
        );
}
//保存按钮
static QString BUTTON_STYLE2() {
    return    QStringLiteral(
        "QPushButton {"
        " background-color: #69d7f8;"
        " color: white;"
        " border: none;"
        " padding: 12px 24px;"
        " border-radius: 8px;"
        " font-size: 18px;"
        " font-weight: bold;"
        " min-width: 100px;"
        " min-height: 40px;"
        " qproperty-iconSize: 24px 24px;"
        "}"
        "QPushButton:hover {"
        " background-color: #00bfff;"
        " border: 2px solid #ffffff;"
        "}"
        "QPushButton:pressed {"
        " background-color: #0489b1;"
        " border: 3px solid #ffffff;"
        "}"
        );
}
//运动方向控制
static QString BUTTON_STYLE3() {
    return    QStringLiteral(
        "QPushButton {"
        " min-width: 80px;"
        " min-height: 80px;"
        "border: 2px solid #f9f5fd;"
        " border-radius: 30px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #dddddd;"

        "}"
        "QPushButton:pressed {"
        "   background-color: #cdcdcd;;"
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


//地图名称框
static QString LINEEDIT_STYLE2() {
    return    QStringLiteral(
        "QLineEdit {"
        "border-radius: 1px;"
        " border: 1px solid #9cd0ce;"
        "font-size: 12px;"
        "font-family: 微软雅黑;"
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
    ui->recordButton->setStyleSheet(BUTTON_STYLE());
    ui->captureButton->setStyleSheet(BUTTON_STYLE1());
    ui->savePathButton->setStyleSheet(BUTTON_STYLE2());
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
    ui->pushButton->setStyleSheet(BUTTON_STYLE6());
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
    ui->lineEdit_mapname->setStyleSheet(LINEEDIT_STYLE2());
    ui->lineEdit_mode->setStyleSheet(LINEEDIT_STYLE2());

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


    //设置QComboBox下拉选项的样式
    ui->comboBox_Mode->setStyleSheet(
        // 主控件基础样式
        "QComboBox {"
        "border: 2px ridge #c4c4c4;"
        "border-radius: 5px;"
        " padding: 5px;"                  // 输入区内边距
        " border-radius: 5px;"             // 圆角效果
        "font-family: 微软雅黑;"
        "}"

        // 下拉列表样式（核心部分）
        "QComboBox QAbstractItemView {"
        "   background: #F8F9F9;"           // 下拉背景浅灰色（匹配图片背景）
        "   border: 1px solid #D6DBDF;"     // 边框颜色
        "   selection-background-color: #AED6F1;"  // 选中项背景色
        "}"

        // 单个选项的样式
        "QComboBox QAbstractItemView::item {"
        "   height: 25px;"                  // 选项高度
        "   padding-left: 10px;"            // 文字左对齐
        "   color: #2C3E50;"                // 文字颜色（深蓝黑）
        "   font-family: '微软雅黑';"        // 字体设置
        "   font-size: 14px;"               // 字号
        "}"

        // 鼠标悬停效果
        "QComboBox QAbstractItemView::item:hover {"
        "   background: #EBF5FB;"           // 悬停背景色
        "   color: #21618C;"                // 悬停文字颜色
        "}"

        "QComboBox::drop-down {"
        " }"
        );




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

    ui->lineEdit_mapname->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_mapname->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->lineEdit_mode->setReadOnly(true);      // 禁止编辑
    ui->lineEdit_mode->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->plainTextEdit->setReadOnly(true);     // 禁止编辑
    ui->plainTextEdit->setFocusPolicy(Qt::NoFocus); // 禁用焦点

    ui->doubleSpinBox->setValue(0.20); //初始值
    ui->doubleSpinBox->setSingleStep(0.1);   // 每次增减0.1
    ui->doubleSpinBox->setSuffix("    直行速度");
    ui->doubleSpinBox_2->setValue(0.20);
    ui->doubleSpinBox_2->setSingleStep(0.1);
    ui->doubleSpinBox_2->setSuffix("    转向速度");
















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

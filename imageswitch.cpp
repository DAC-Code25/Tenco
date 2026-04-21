#ifdef Q_CC_MSVC
#pragma execution_character_set("utf-8")  // 仅对 MSVC 设置执行字符集，避免 GCC/MinGW 警告
#endif

#include "imageswitch.h"
#include "qpainter.h"
#include "qdebug.h"

ImageSwitch::ImageSwitch(QWidget *parent) : QWidget(parent)
{
    isChecked = false;  // 初始状态为未选中
    buttonStyle = ButtonStyle_2;  // 默认使用第二种样式

    // 设置默认的关闭和开启图片路径
    imgOffFile = ":/image/imageswitch/btncheckoff2.png";
    imgOnFile = ":/image/imageswitch/btncheckon2.png";
    imgFile = imgOffFile;  // 初始显示关闭状态的图片
}

void ImageSwitch::mousePressEvent(QMouseEvent *)
{
    // 鼠标点击事件，切换开关状态
    imgFile = isChecked ? imgOffFile : imgOnFile;  // 根据当前状态选择图片
    isChecked = !isChecked;  // 切换选中状态
    Q_EMIT checkedChanged(isChecked);  // 发射状态改变信号
    this->update();  // 更新控件显示
}

void ImageSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHints(QPainter::SmoothPixmapTransform);  // 设置平滑像素变换
    QImage img(imgFile);  // 加载当前图片
    img = img.scaled(this->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);  // 缩放图片到控件大小

    // 按照比例自动居中绘制
    int pixX = rect().center().x() - img.width() / 2;
    int pixY = rect().center().y() - img.height() / 2;
    QPoint point(pixX, pixY);
    painter.drawImage(point, img);  // 绘制图片
}

QSize ImageSwitch::sizeHint() const
{
    // 返回建议的控件大小
    return QSize(87, 28);
}

QSize ImageSwitch::minimumSizeHint() const
{
    // 返回控件的最小大小
    return QSize(87, 28);
}

bool ImageSwitch::getChecked() const
{
    // 获取当前选中状态
    return isChecked;
}

void ImageSwitch::setChecked(bool isChecked)
{
    // 设置选中状态
    if (this->isChecked != isChecked) {
        this->isChecked = isChecked;
        imgFile = isChecked ? imgOnFile : imgOffFile;  // 根据状态选择图片
        this->update();  // 更新显示
    }
}

ImageSwitch::ButtonStyle ImageSwitch::getButtonStyle() const
{
    // 获取当前按钮样式
    return this->buttonStyle;
}

void ImageSwitch::setButtonStyle(const ImageSwitch::ButtonStyle &buttonStyle)
{
    // 设置按钮样式
    if (this->buttonStyle != buttonStyle) {
        this->buttonStyle = buttonStyle;

        // 根据样式设置不同的图片路径和控件大小
        if (buttonStyle == ButtonStyle_1) {
            imgOffFile = ":/image/imageswitch/btncheckoff1.png";
            imgOnFile = ":/image/imageswitch/btncheckon1.png";
            this->resize(87, 28);
        } else if (buttonStyle == ButtonStyle_2) {
            imgOffFile = ":/image/imageswitch/btncheckoff2.png";
            imgOnFile = ":/image/imageswitch/btncheckon2.png";
            this->resize(87, 28);
        } else if (buttonStyle == ButtonStyle_3) {
            imgOffFile = ":/image/imageswitch/btncheckoff3.png";
            imgOnFile = ":/image/imageswitch/btncheckon3.png";
            this->resize(96, 38);
        }

        imgFile = isChecked ? imgOnFile : imgOffFile;  // 根据当前状态选择图片
        setChecked(isChecked);  // 更新选中状态
        this->update();  // 更新显示
        updateGeometry();  // 更新几何布局
    }
}

#include "battery.h"
#include <QPainter>
#include <QTimer>
#include <QDebug>

// 构造函数：初始化电池控件
Battery::Battery(QWidget *parent) : QWidget(parent)
{
    // 初始化默认值
    minValue = 0;       // 最小值默认为0
    maxValue = 100;     // 最大值默认为100
    value = 0;          // 当前值默认为0
    alarmValue = 30;    // 报警阈值默认为30%

    // 动画设置
    animation = true;       // 默认启用动画
    animationStep = 0.5;    // 动画步长(每次变化0.5%)

    // 形状设置
    borderWidth = 5;    // 边框宽度5像素
    borderRadius = 8;   // 边框圆角半径8像素
    bgRadius = 5;       // 背景圆角半径5像素
    headRadius = 3;     // 电池头部圆角半径3像素

    // 颜色设置
    borderColorStart = QColor(100, 100, 100);   // 边框渐变起始色(灰色)
    borderColorEnd = QColor(80, 80, 80);         // 边框渐变结束色(深灰)
    alarmColorStart = QColor(250, 118, 113);     // 报警状态起始色(浅红色)
    alarmColorEnd = QColor(204, 38, 38);          // 报警状态结束色(深红色)
    normalColorStart = QColor(50, 205, 51);      // 正常状态起始色(浅绿色)
    normalColorEnd = QColor(60, 179, 133);        // 正常状态结束色(深绿色)

    // 动画状态
    isForward = false;      // 初始动画方向(默认向后)
    currentValue = 0;       // 当前动画值初始为0

    // 创建并设置定时器
    timer = new QTimer(this);
    timer->setInterval(10); // 定时器间隔10ms(约100FPS)
    connect(timer, SIGNAL(timeout()), this, SLOT(updateValue()));
}

// 析构函数：停止定时器
Battery::~Battery()
{
    if (timer->isActive()) {
        timer->stop();
    }
}

// 绘制事件：绘制整个电池控件
void Battery::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    // 启用抗锯齿和文本抗锯齿
    painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);

    // 按顺序绘制电池各部分
    drawBorder(&painter);   // 1. 绘制边框
    drawBg(&painter);       // 2. 绘制电量填充
    drawHead(&painter);     // 3. 绘制电池头部
}

// 绘制电池边框
void Battery::drawBorder(QPainter *painter)
{
    painter->save();  // 保存当前绘图状态

    // 计算电池头部宽度(占整个宽度的1/15)
    double headWidth = width() / 15;
    // 电池主体宽度(总宽度减去头部宽度)
    double batteryWidth = width() - headWidth;

    // 定义电池主体矩形区域
    QPointF topLeft(borderWidth, borderWidth);  // 左上角坐标(考虑边框宽度)
    QPointF bottomRight(batteryWidth, height() - borderWidth); // 右下角坐标
    batteryRect = QRectF(topLeft, bottomRight);  // 电池主体矩形

    // 设置画笔(边框颜色和宽度)
    painter->setPen(QPen(borderColorStart, borderWidth));
    painter->setBrush(Qt::NoBrush);  // 无填充

    // 绘制圆角矩形边框
    painter->drawRoundedRect(batteryRect, borderRadius, borderRadius);

    painter->restore();  // 恢复绘图状态
}

// 绘制电池电量填充
void Battery::drawBg(QPainter *painter)
{
    // 如果值为最小值，不绘制填充
    if (value == minValue) {
        return;
    }

    painter->save();  // 保存当前绘图状态

    // 创建垂直线性渐变(从上到下)
    QLinearGradient batteryGradient(QPointF(0, 0), QPointF(0, height()));

    // 根据当前值是否低于报警阈值选择颜色
    if (currentValue <= alarmValue) {
        // 报警颜色(浅红到深红)
        batteryGradient.setColorAt(0.0, alarmColorStart);
        batteryGradient.setColorAt(1.0, alarmColorEnd);
    } else {
        // 正常颜色(浅绿到深绿)
        batteryGradient.setColorAt(0.0, normalColorStart);
        batteryGradient.setColorAt(1.0, normalColorEnd);
    }

    // 计算边距(取宽高中较小值的1/20)
    int margin = qMin(width(), height()) / 20;
    // 计算单位宽度(每1%值对应的像素宽度)
    double unit = (batteryRect.width() - (margin * 2)) / (maxValue - minValue);
    // 计算当前值对应的宽度
    double width = currentValue * unit;

    // 定义填充矩形区域(在边框内部)
    QPointF topLeft(batteryRect.topLeft().x() + margin, batteryRect.topLeft().y() + margin);
    QPointF bottomRight(width + margin + borderWidth, batteryRect.bottomRight().y() - margin);
    QRectF rect(topLeft, bottomRight);

    // 设置无边框，使用渐变填充
    painter->setPen(Qt::NoPen);
    painter->setBrush(batteryGradient);
    // 绘制圆角填充矩形
    painter->drawRoundedRect(rect, bgRadius, bgRadius);

    painter->restore();  // 恢复绘图状态
}

// 绘制电池头部(正极)
void Battery::drawHead(QPainter *painter)
{
    painter->save();  // 保存当前绘图状态

    // 计算头部矩形位置(在主体右侧)
    QPointF headRectTopLeft(batteryRect.topRight().x(), height() / 3);
    QPointF headRectBottomRight(width(), height() - height() / 3);
    QRectF headRect(headRectTopLeft, headRectBottomRight);

    // 创建头部线性渐变(从上到下)
    QLinearGradient headRectGradient(headRect.topLeft(), headRect.bottomLeft());
    headRectGradient.setColorAt(0.0, borderColorStart);  // 顶部颜色
    headRectGradient.setColorAt(1.0, borderColorEnd);    // 底部颜色

    // 设置无边框，使用渐变填充
    painter->setPen(Qt::NoPen);
    painter->setBrush(headRectGradient);
    // 绘制圆角头部
    painter->drawRoundedRect(headRect, headRadius, headRadius);

    painter->restore();  // 恢复绘图状态
}

// 定时器槽函数：更新当前值(实现动画效果)
void Battery::updateValue()
{
    if (isForward) {
        // 向后动画(值减小)
        currentValue -= animationStep;
        if (currentValue <= value) {
            currentValue = value;
            timer->stop();  // 达到目标值，停止定时器
        }
    } else {
        // 向前动画(值增大)
        currentValue += animationStep;
        if (currentValue >= value) {
            currentValue = value;
            timer->stop();  // 达到目标值，停止定时器
        }
    }

    this->update();  // 触发重绘
}

// 推荐尺寸
QSize Battery::sizeHint() const
{
    return QSize(150, 80);  // 默认建议尺寸150x80像素
}

// 最小尺寸
QSize Battery::minimumSizeHint() const
{
    return QSize(30, 10);  // 最小尺寸30x10像素
}

// 设置范围值
void Battery::setRange(double minValue, double maxValue)
{
    // 校验范围有效性
    if (minValue >= maxValue) {
        return;
    }

    // 更新范围
    this->minValue = minValue;
    this->maxValue = maxValue;

    // 确保当前值在新范围内
    if (value < minValue) {
        setValue(minValue);
    } else if (value > maxValue) {
        setValue(maxValue);
    }

    this->update();  // 更新显示
}

// 设置范围值(整型版本)
void Battery::setRange(int minValue, int maxValue)
{
    setRange((double)minValue, (double)maxValue);
}

// 获取最小值
double Battery::getMinValue() const
{
    return this->minValue;
}

// 设置最小值
void Battery::setMinValue(double minValue)
{
    setRange(minValue, maxValue);  // 通过设置范围更新
}

// 获取最大值
double Battery::getMaxValue() const
{
    return this->maxValue;
}

// 设置最大值
void Battery::setMaxValue(double maxValue)
{
    setRange(minValue, maxValue);  // 通过设置范围更新
}

// 获取当前值
double Battery::getValue() const
{
    return this->value;
}

// 设置目标值(核心方法)
void Battery::setValue(double value)
{
    // 值未变化时直接返回
    if (value == this->value) {
        return;
    }

    // 确保值在有效范围内
    if (value < minValue) {
        value = minValue;
    } else if (value > maxValue) {
        value = maxValue;
    }

    // 确定动画方向
    if (value > currentValue) {
        isForward = false;  // 增大方向
    } else if (value < currentValue) {
        isForward = true;   // 减小方向
    } else {
        // 值相同但需要更新显示的情况
        this->value = value;
        this->update();
        return;
    }

    // 更新目标值
    this->value = value;
    // 发射值变化信号
    Q_EMIT valueChanged(value);

    // 处理动画
    if (animation) {
        timer->stop();      // 先停止可能正在运行的动画
        timer->start();     // 启动动画
    } else {
        currentValue = value;  // 无动画时直接设置当前值
        this->update();     // 更新显示
    }
}

// 设置目标值(整型版本)
void Battery::setValue(int value)
{
    setValue((double)value);
}

// 获取报警阈值
double Battery::getAlarmValue() const
{
    return this->alarmValue;
}

// 设置报警阈值
void Battery::setAlarmValue(double alarmValue)
{
    if (this->alarmValue != alarmValue) {
        this->alarmValue = alarmValue;
        this->update();  // 更新显示
    }
}

// 设置报警阈值(整型版本)
void Battery::setAlarmValue(int alarmValue)
{
    setAlarmValue((double)alarmValue);
}

// 获取动画启用状态
bool Battery::getAnimation() const
{
    return this->animation;
}

// 设置动画启用状态
void Battery::setAnimation(bool animation)
{
    if (this->animation != animation) {
        this->animation = animation;
        this->update();  // 更新显示
    }
}

// 获取动画步长
double Battery::getAnimationStep() const
{
    return this->animationStep;
}

// 设置动画步长
void Battery::setAnimationStep(double animationStep)
{
    if (this->animationStep != animationStep) {
        this->animationStep = animationStep;
        this->update();  // 更新显示
    }
}

// 获取边框宽度
int Battery::getBorderWidth() const
{
    return this->borderWidth;
}

// 设置边框宽度
void Battery::setBorderWidth(int borderWidth)
{
    if (this->borderWidth != borderWidth) {
        this->borderWidth = borderWidth;
        this->update();  // 更新显示
    }
}

// 获取边框圆角半径
int Battery::getBorderRadius() const
{
    return this->borderRadius;
}

// 设置边框圆角半径
void Battery::setBorderRadius(int borderRadius)
{
    if (this->borderRadius != borderRadius) {
        this->borderRadius = borderRadius;
        this->update();  // 更新显示
    }
}

// 获取背景圆角半径
int Battery::getBgRadius() const
{
    return this->bgRadius;
}

// 设置背景圆角半径
void Battery::setBgRadius(int bgRadius)
{
    if (this->bgRadius != bgRadius) {
        this->bgRadius = bgRadius;
        this->update();  // 更新显示
    }
}

// 获取头部圆角半径
int Battery::getHeadRadius() const
{
    return this->headRadius;
}

// 设置头部圆角半径
void Battery::setHeadRadius(int headRadius)
{
    if (this->headRadius != headRadius) {
        this->headRadius = headRadius;
        this->update();  // 更新显示
    }
}

// 获取边框渐变起始色
QColor Battery::getBorderColorStart() const
{
    return this->borderColorStart;
}

// 设置边框渐变起始色
void Battery::setBorderColorStart(const QColor &borderColorStart)
{
    if (this->borderColorStart != borderColorStart) {
        this->borderColorStart = borderColorStart;
        this->update();  // 更新显示
    }
}

// 获取边框渐变结束色
QColor Battery::getBorderColorEnd() const
{
    return this->borderColorEnd;
}

// 设置边框渐变结束色
void Battery::setBorderColorEnd(const QColor &borderColorEnd)
{
    if (this->borderColorEnd != borderColorEnd) {
        this->borderColorEnd = borderColorEnd;
        this->update();  // 更新显示
    }
}

// 获取报警状态起始色
QColor Battery::getAlarmColorStart() const
{
    return this->alarmColorStart;
}

// 设置报警状态起始色
void Battery::setAlarmColorStart(const QColor &alarmColorStart)
{
    if (this->alarmColorStart != alarmColorStart) {
        this->alarmColorStart = alarmColorStart;
        this->update();  // 更新显示
    }
}

// 获取报警状态结束色
QColor Battery::getAlarmColorEnd() const
{
    return this->alarmColorEnd;
}

// 设置报警状态结束色
void Battery::setAlarmColorEnd(const QColor &alarmColorEnd)
{
    if (this->alarmColorEnd != alarmColorEnd) {
        this->alarmColorEnd = alarmColorEnd;
        this->update();  // 更新显示
    }
}

// 获取正常状态起始色
QColor Battery::getNormalColorStart() const
{
    return this->normalColorStart;
}

// 设置正常状态起始色
void Battery::setNormalColorStart(const QColor &normalColorStart)
{
    if (this->normalColorStart != normalColorStart) {
        this->normalColorStart = normalColorStart;
        this->update();  // 更新显示
    }
}

// 获取正常状态结束色
QColor Battery::getNormalColorEnd() const
{
    return this->normalColorEnd;
}

// 设置正常状态结束色
void Battery::setNormalColorEnd(const QColor &normalColorEnd)
{
    if (this->normalColorEnd != normalColorEnd) {
        this->normalColorEnd = normalColorEnd;
        this->update();  // 更新显示
    }
}

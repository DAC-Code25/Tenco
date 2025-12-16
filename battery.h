#ifndef BATTERY_H
#define BATTERY_H

#include <QWidget>

#ifdef quc
class Q_DECL_EXPORT Battery : public QWidget
#else
class Battery : public QWidget
#endif

{
    Q_OBJECT

    // 属性系统：支持在Qt Designer中设置这些属性
    Q_PROPERTY(double minValue READ getMinValue WRITE setMinValue)           // 最小值属性
    Q_PROPERTY(double maxValue READ getMaxValue WRITE setMaxValue)           // 最大值属性
    Q_PROPERTY(double value READ getValue WRITE setValue)                   // 当前值属性
    Q_PROPERTY(double alarmValue READ getAlarmValue WRITE setAlarmValue)     // 报警阈值属性

    Q_PROPERTY(bool animation READ getAnimation WRITE setAnimation)          // 是否启用动画
    Q_PROPERTY(double animationStep READ getAnimationStep WRITE setAnimationStep) // 动画步长

    Q_PROPERTY(int borderWidth READ getBorderWidth WRITE setBorderWidth)     // 边框宽度
    Q_PROPERTY(int borderRadius READ getBorderRadius WRITE setBorderRadius)  // 边框圆角半径
    Q_PROPERTY(int bgRadius READ getBgRadius WRITE setBgRadius)              // 背景圆角半径
    Q_PROPERTY(int headRadius READ getHeadRadius WRITE setHeadRadius)        // 电池头部圆角半径

    Q_PROPERTY(QColor borderColorStart READ getBorderColorStart WRITE setBorderColorStart)   // 边框渐变起始色
    Q_PROPERTY(QColor borderColorEnd READ getBorderColorEnd WRITE setBorderColorEnd)         // 边框渐变结束色

    Q_PROPERTY(QColor alarmColorStart READ getAlarmColorStart WRITE setAlarmColorStart)     // 报警状态渐变起始色
    Q_PROPERTY(QColor alarmColorEnd READ getAlarmColorEnd WRITE setAlarmColorEnd)           // 报警状态渐变结束色

    Q_PROPERTY(QColor normalColorStart READ getNormalColorStart WRITE setNormalColorStart)   // 正常状态渐变起始色
    Q_PROPERTY(QColor normalColorEnd READ getNormalColorEnd WRITE setNormalColorEnd)         // 正常状态渐变结束色

public:
    explicit Battery(QWidget *parent = 0);  // 构造函数
    ~Battery();                             // 析构函数

protected:
    // 重写QWidget的绘制事件
    void paintEvent(QPaintEvent *);

    // 绘制电池各部分的方法
    void drawBorder(QPainter *painter);     // 绘制电池边框
    void drawBg(QPainter *painter);         // 绘制电池背景(电量填充)
    void drawHead(QPainter *painter);       // 绘制电池头部

private slots:
    void updateValue();  // 更新当前值的动画效果槽函数

private:
    // 电池属性值
    double minValue;        // 最小值
    double maxValue;        // 最大值
    double value;           // 目标电量值
    double alarmValue;      // 电量报警阈值

    // 动画设置
    bool animation;         // 是否启用动画效果
    double animationStep;    // 动画步长(每次变化量)

    // 形状设置
    int borderWidth;        // 边框宽度(像素)
    int borderRadius;       // 边框圆角半径
    int bgRadius;           // 电量填充背景圆角半径
    int headRadius;         // 电池头部圆角半径

    // 颜色设置
    QColor borderColorStart; // 边框渐变起始颜色
    QColor borderColorEnd;   // 边框渐变结束颜色
    QColor alarmColorStart;  // 低电量报警状态填充起始色
    QColor alarmColorEnd;    // 低电量报警状态填充结束色
    QColor normalColorStart; // 正常电量状态填充起始色
    QColor normalColorEnd;   // 正常电量状态填充结束色

    // 内部状态
    bool isForward;         // 动画方向(前进/后退)
    double currentValue;    // 当前动画显示的电量值
    QRectF batteryRect;     // 电池主体矩形区域
    QTimer *timer;          // 动画定时器

public:
    // 默认尺寸建议
    QSize sizeHint() const;         // 推荐尺寸
    QSize minimumSizeHint() const;  // 最小尺寸

    // 范围设置方法
    void setRange(double minValue, double maxValue);  // 设置值范围(double)
    void setRange(int minValue, int maxValue);        // 设置值范围(int)

    // 属性访问器
    double getMinValue() const;     // 获取最小值
    void setMinValue(double minValue);  // 设置最小值

    double getMaxValue() const;     // 获取最大值
    void setMaxValue(double maxValue);  // 设置最大值

    double getValue() const;        // 获取目标电量值
    void setValue(double value);    // 设置目标电量值

    double getAlarmValue() const;   // 获取报警阈值
    void setAlarmValue(double alarmValue);  // 设置报警阈值

    bool getAnimation() const;      // 获取动画启用状态
    void setAnimation(bool animation);  // 设置动画启用状态

    double getAnimationStep() const;    // 获取动画步长
    void setAnimationStep(double animationStep);  // 设置动画步长

    int getBorderWidth() const;     // 获取边框宽度
    void setBorderWidth(int borderWidth);  // 设置边框宽度

    int getBorderRadius() const;    // 获取边框圆角半径
    void setBorderRadius(int borderRadius);  // 设置边框圆角半径

    int getBgRadius() const;        // 获取背景圆角半径
    void setBgRadius(int bgRadius); // 设置背景圆角半径

    int getHeadRadius() const;      // 获取头部圆角半径
    void setHeadRadius(int headRadius);  // 设置头部圆角半径

    QColor getBorderColorStart() const;     // 获取边框渐变起始色
    void setBorderColorStart(const QColor &borderColorStart);  // 设置边框渐变起始色

    QColor getBorderColorEnd() const;       // 获取边框渐变结束色
    void setBorderColorEnd(const QColor &borderColorEnd);      // 设置边框渐变结束色

    QColor getAlarmColorStart() const;      // 获取报警状态起始色
    void setAlarmColorStart(const QColor &alarmColorStart);    // 设置报警状态起始色

    QColor getAlarmColorEnd() const;        // 获取报警状态结束色
    void setAlarmColorEnd(const QColor &alarmColorEnd);        // 设置报警状态结束色

    QColor getNormalColorStart() const;     // 获取正常状态起始色
    void setNormalColorStart(const QColor &normalColorStart);  // 设置正常状态起始色

    QColor getNormalColorEnd() const;       // 获取正常状态结束色
    void setNormalColorEnd(const QColor &normalColorEnd);      // 设置正常状态结束色

public Q_SLOTS:
    // 整型值设置槽函数
    void setValue(int value);           // 设置目标电量值(int)
    void setAlarmValue(int alarmValue); // 设置报警阈值(int)

Q_SIGNALS:
    // 值变化信号
    void valueChanged(double value);    // 当电量值改变时发射
};

#endif // BATTERY_H

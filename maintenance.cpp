#include "maintenance.h"

#include "configmanager.h"
#include "loggingmanager.h"
#include "ui_mainwindow.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtGlobal>

#include <limits>

namespace {
constexpr int kFieldMinWidth = 360;
const char *kMaintenanceStyle = R"(
QWidget#maintenanceRoot {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #f5f7f2, stop:0.55 #eef3eb, stop:1 #e5ece3);
}
QFrame#maintenanceHero {
    background: rgba(255, 255, 255, 0.96);
    border: 1px solid rgba(125, 144, 128, 0.28);
    border-radius: 18px;
}
QFrame#maintenanceCard {
    background: rgba(255, 255, 255, 0.97);
    border: 1px solid rgba(124, 143, 126, 0.22);
    border-radius: 16px;
}
QLabel#maintenanceTitle {
    color: #132019;
    font-size: 28px;
    font-weight: 800;
}
QLabel#maintenanceSubtitle {
    color: #4d5d54;
    font-size: 14px;
    font-weight: 500;
}
QLabel#maintenanceHint {
    color: #62746a;
    font-size: 12px;
    font-weight: 500;
}
QLabel#maintenancePageTitle {
    color: #17211b;
    font-size: 22px;
    font-weight: 800;
}
QLabel#maintenancePageDesc {
    color: #526259;
    font-size: 13px;
    font-weight: 500;
}
QLabel#maintenanceFieldLabel {
    color: #24372d;
    font-size: 13px;
    font-weight: 700;
}
QLabel#maintenanceFieldHint {
    color: #738278;
    font-size: 12px;
}
QLabel#maintenanceReadonlyValue {
    color: #1f3128;
    font-size: 13px;
    font-weight: 600;
}
QLabel#maintenanceStatusLabel {
    color: #3c4d43;
    font-size: 12px;
    font-weight: 500;
}
QTabWidget::pane {
    border: 1px solid rgba(125, 144, 128, 0.28);
    border-radius: 14px;
    top: -1px;
    background: transparent;
}
QTabBar::tab {
    background: rgba(223, 232, 222, 0.92);
    color: #34493d;
    padding: 12px 20px;
    margin-right: 6px;
    border-top-left-radius: 12px;
    border-top-right-radius: 12px;
    min-width: 108px;
    font-size: 14px;
    font-weight: 800;
}
QTabBar::tab:selected {
    background: white;
    color: #123026;
}
QTabBar::tab:hover {
    background: rgba(248, 250, 252, 0.96);
}
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit {
    background: white;
    border: 1px solid #bdcbbb;
    border-radius: 10px;
    padding: 8px 12px;
    min-height: 34px;
    color: #17211b;
    font-size: 13px;
    selection-background-color: #2f7d64;
    selection-color: white;
}
QWidget#maintenanceUnitField {
    background: white;
    border: 1px solid #bdcbbb;
    border-radius: 10px;
    min-height: 34px;
}
QSpinBox#maintenanceUnitSpinBox, QDoubleSpinBox#maintenanceUnitSpinBox {
    background: transparent;
    border: none;
    border-radius: 0px;
    padding: 8px 8px 8px 12px;
    min-height: 34px;
}
QSpinBox#maintenanceUnitSpinBox:focus, QDoubleSpinBox#maintenanceUnitSpinBox:focus {
    border: none;
    background: transparent;
}
QLabel#maintenanceUnitLabel {
    color: #66786d;
    font-size: 12px;
    font-weight: 700;
    padding-right: 12px;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus, QPlainTextEdit:focus {
    border: 1px solid #2f7d64;
    background: #fbfffb;
}
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    width: 0px;
    border: none;
}
QSpinBox::up-arrow, QSpinBox::down-arrow, QDoubleSpinBox::up-arrow, QDoubleSpinBox::down-arrow {
    width: 0px;
    height: 0px;
}
QComboBox::drop-down {
    width: 34px;
    border: none;
    border-left: 1px solid #d7e0d4;
    border-top-right-radius: 10px;
    border-bottom-right-radius: 10px;
}
QComboBox::down-arrow {
    image: none;
    width: 0px;
    height: 0px;
}
QCheckBox {
    padding: 2px 0;
    color: #17211b;
    font-size: 13px;
    font-weight: 600;
}
QPushButton#maintenanceSecondaryButton {
    background: white;
    color: #3e5146;
    border: 1px solid #bdcbbb;
    border-radius: 11px;
    padding: 10px 18px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 700;
}
QPushButton#maintenanceSecondaryButton:hover {
    background: #f6faf4;
}
QPushButton#maintenanceSecondaryButton:disabled, QPushButton#maintenancePrimaryButton:disabled {
    background: #e5ebe3;
    color: #94a39a;
    border: 1px solid #d5dfd2;
}
QPushButton#maintenancePrimaryButton {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2f7d64, stop:1 #1f5f4b);
    color: white;
    border: none;
    border-radius: 11px;
    padding: 10px 22px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 800;
}
QPushButton#maintenancePrimaryButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b9274, stop:1 #287257);
}
QPushButton#maintenancePrimaryButton:pressed {
    background: #174c3b;
}
)";

template <typename Widget>
void applyFieldSize(Widget *widget)
{
    if (!widget) {
        return;
    }
    widget->setMinimumWidth(kFieldMinWidth);
    widget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

bool isEditableInput(QObject *object)
{
    return qobject_cast<QAbstractSpinBox *>(object) ||
           qobject_cast<QComboBox *>(object) ||
           qobject_cast<QLineEdit *>(object) ||
           qobject_cast<QPlainTextEdit *>(object);
}

void applyClickOnlyFocus(QWidget *widget, QObject *filter)
{
    if (!widget) {
        return;
    }

    widget->setFocusPolicy(Qt::ClickFocus);
    widget->installEventFilter(filter);

    if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
        if (auto *lineEdit = spin->findChild<QLineEdit *>()) {
            lineEdit->setFocusPolicy(Qt::ClickFocus);
            lineEdit->installEventFilter(filter);
        }
    }
}

QWidget *createUnitField(QAbstractSpinBox *spin, const QString &suffix)
{
    const QString unit = suffix.trimmed();
    if (!spin || unit.isEmpty()) {
        return spin;
    }

    spin->setObjectName(QStringLiteral("maintenanceUnitSpinBox"));
    spin->setFrame(false);
    spin->setMinimumWidth(0);
    spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *container = new QWidget();
    container->setObjectName(QStringLiteral("maintenanceUnitField"));
    container->setFocusProxy(spin);
    container->setMinimumWidth(kFieldMinWidth);
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(spin, 1);

    auto *unitLabel = new QLabel(unit, container);
    unitLabel->setObjectName(QStringLiteral("maintenanceUnitLabel"));
    unitLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    unitLabel->setMinimumWidth(58);
    layout->addWidget(unitLabel);

    return container;
}

void clearEditableFocusAround(QWidget *widget)
{
    for (QWidget *current = widget; current; current = current->parentWidget()) {
        if (qobject_cast<QScrollArea *>(current)) {
            break;
        }
        if (isEditableInput(current)) {
            current->clearFocus();
        }
        if (QWidget *proxy = current->focusProxy()) {
            proxy->clearFocus();
        }
    }
}

bool scrollParentArea(QWidget *widget, QWheelEvent *wheelEvent)
{
    if (!widget || !wheelEvent) {
        return false;
    }

    for (QWidget *parentWidget = widget->parentWidget(); parentWidget; parentWidget = parentWidget->parentWidget()) {
        auto *scrollArea = qobject_cast<QScrollArea *>(parentWidget);
        if (!scrollArea || !scrollArea->verticalScrollBar()) {
            continue;
        }

        QScrollBar *bar = scrollArea->verticalScrollBar();
        int delta = 0;
        if (!wheelEvent->pixelDelta().isNull()) {
            delta = -wheelEvent->pixelDelta().y();
        } else if (!wheelEvent->angleDelta().isNull()) {
            const int lines = qMax(1, QApplication::wheelScrollLines());
            delta = -wheelEvent->angleDelta().y() * lines * qMax(1, bar->singleStep()) / 120;
        }
        if (delta == 0 && !wheelEvent->angleDelta().isNull()) {
            delta = wheelEvent->angleDelta().y() > 0 ? -qMax(1, bar->singleStep()) : qMax(1, bar->singleStep());
        }
        bar->setValue(bar->value() + delta);
        return true;
    }

    return false;
}

QString parameterDescription(const QString &key)
{
    static const QHash<QString, QString> descriptions = {
        {QStringLiteral("network.websocketUrl"), QStringLiteral("底盘 WebSocket 控制地址，用于首页/地图向小车发送速度和控制指令。控制器 IP 或端口变化时修改。")},
        {QStringLiteral("network.statusReadUrl"), QStringLiteral("状态读取 HTTP 地址，用于获取电量、位姿、速度等运行状态。状态页无数据或换控制器时检查。")},
        {QStringLiteral("network.writeInsUrl"), QStringLiteral("写寄存器 HTTP 地址，用于向控制器写入部分控制/任务指令。接口地址变化时修改。")},
        {QStringLiteral("network.saveFileUrl"), QStringLiteral("地图保存 HTTP 地址，用于把地图或路径文件保存到控制器侧。地图保存失败时检查。")},
        {QStringLiteral("network.authToken"), QStringLiteral("接口认证 Token，需要与控制器服务保持一致。认证失败或服务更换密钥时修改。")},
        {QStringLiteral("network.statusPollIntervalMs"), QStringLiteral("状态轮询周期。数值越小状态越实时，但网络和控制器负载越高。")},
        {QStringLiteral("network.statusRequestTimeoutMs"), QStringLiteral("单次状态请求超时时间。太小容易误判离线，太大会让故障恢复变慢。")},
        {QStringLiteral("network.statusMaxBackoffMs"), QStringLiteral("状态请求连续失败后的最大退避间隔。网络不稳定时适当增大可减少日志和请求压力。")},
        {QStringLiteral("network.chassisAutoReconnect"), QStringLiteral("底盘 WebSocket 断开后是否自动重连。生产运行建议开启。")},
        {QStringLiteral("network.chassisReconnectIntervalMs"), QStringLiteral("底盘断线后的初始重连间隔。数值越小恢复更快，但断网时重连更频繁。")},
        {QStringLiteral("network.chassisReconnectMaxIntervalMs"), QStringLiteral("底盘连续重连失败后的最大重连间隔，不能小于基础重连间隔。")},

        {QStringLiteral("video.backend"), QStringLiteral("视频源类型。mjpeg_http 表示使用工控机 HTTP MJPEG 流，oak_depthai 表示本机直连 OAK 相机。")},
        {QStringLiteral("video.streamUrl"), QStringLiteral("首页默认预览流地址。启动预览或自动预览时优先打开该视频流。")},
        {QStringLiteral("video.controlBaseUrl"), QStringLiteral("工控机相机控制服务基地址，用于拍照、录像、状态查询等 HTTP 控制接口。")},
        {QStringLiteral("video.streamOptions"), QStringLiteral("可切换视频流列表，必须是 JSON 数组。每项包含 name 和 url，用于首页视频流下拉选择。")},
        {QStringLiteral("video.deviceId"), QStringLiteral("OAK 相机设备 ID。单台本机 OAK 可留空，多台相机时建议指定，避免打开错误设备。")},
        {QStringLiteral("video.previewWidth"), QStringLiteral("请求的视频预览宽度。分辨率越高画面越清晰，但 CPU、网络和解码压力越大。")},
        {QStringLiteral("video.previewHeight"), QStringLiteral("请求的视频预览高度。需与相机/服务端支持能力匹配。")},
        {QStringLiteral("video.previewFps"), QStringLiteral("请求的视频预览帧率。帧率越高越流畅，但负载越高。")},
        {QStringLiteral("video.recordMode"), QStringLiteral("录像模式。当前仅支持 host_opencv，表示由工控机/主机侧 OpenCV 处理录像。")},
        {QStringLiteral("video.recordCodec"), QStringLiteral("录像编码格式，例如 mp4v、MJPG 等。需与运行环境中的 OpenCV 编码器支持匹配。")},
        {QStringLiteral("video.reconnectIntervalMs"), QStringLiteral("视频流断开后的重连间隔。网络抖动频繁时可适当增大。")},
        {QStringLiteral("video.cameraRequestTimeoutMs"), QStringLiteral("拍照、录像开始/停止、相机状态查询等 HTTP 控制请求的超时时间。")},
        {QStringLiteral("video.autoStart"), QStringLiteral("程序启动进入首页后是否自动打开默认视频预览流。")},
        {QStringLiteral("video.scaleContents"), QStringLiteral("是否把视频画面自适应拉伸到显示区域。关闭后更接近原始比例。")},

        {QStringLiteral("gimbal.enabled"), QStringLiteral("是否启用首页云台控制。关闭后云台按钮和键盘控制不会向 PLC 发送动作。")},
        {QStringLiteral("gimbal.plcHost"), QStringLiteral("云台/升降 PLC 的 Modbus TCP 地址。PLC IP 变化或网络切换时修改。")},
        {QStringLiteral("gimbal.plcPort"), QStringLiteral("云台/升降 PLC 的 Modbus TCP 端口，通常为 502。")},
        {QStringLiteral("gimbal.unitId"), QStringLiteral("Modbus 从站 ID，需要与 PLC 通信配置一致。")},
        {QStringLiteral("gimbal.requestTimeoutMs"), QStringLiteral("云台 PLC 单次读写超时时间。太小容易误判失败，太大影响按键响应。")},
        {QStringLiteral("gimbal.statusPollIntervalMs"), QStringLiteral("云台状态反馈轮询周期。数值越小反馈越实时，但 PLC 通信更频繁。")},
        {QStringLiteral("gimbal.heightControlAddress"), QStringLiteral("升降控制寄存器地址。只有 PLC 寄存器映射变化时才应修改。")},
        {QStringLiteral("gimbal.pitchControlAddress"), QStringLiteral("俯仰控制寄存器地址。只有 PLC 寄存器映射变化时才应修改。")},
        {QStringLiteral("gimbal.yawControlAddress"), QStringLiteral("水平旋转控制寄存器地址。只有 PLC 寄存器映射变化时才应修改。")},
        {QStringLiteral("gimbal.statusStartAddress"), QStringLiteral("云台状态反馈起始寄存器地址，用于读取高度、水平角、俯仰角等反馈值。")},
        {QStringLiteral("gimbal.statusRegisterCount"), QStringLiteral("从状态起始地址连续读取的寄存器数量。数量不足会导致部分反馈缺失。")},
        {QStringLiteral("gimbal.minHeight"), QStringLiteral("升降最低软限位，基于 PLC 反馈高度值判断。用于阻止继续下降，不能替代机械限位。")},
        {QStringLiteral("gimbal.maxHeight"), QStringLiteral("升降最高软限位，基于 PLC 反馈高度值判断。用于阻止继续上升，不能替代机械限位。")},
        {QStringLiteral("gimbal.minYaw"), QStringLiteral("水平右旋方向软限位，基于 PLC 反馈水平角判断。设置前需先用遥控确认安全范围。")},
        {QStringLiteral("gimbal.maxYaw"), QStringLiteral("水平左旋方向软限位，基于 PLC 反馈水平角判断。设置前需先用遥控确认安全范围。")},
        {QStringLiteral("gimbal.minPitch"), QStringLiteral("俯仰上仰方向软限位，基于 PLC 反馈俯仰角判断。设置前需确认相机和线缆不会干涉。")},
        {QStringLiteral("gimbal.maxPitch"), QStringLiteral("俯仰下俯方向软限位，基于 PLC 反馈俯仰角判断。设置前需确认相机和线缆不会干涉。")},
        {QStringLiteral("gimbal.safetyStopTimeoutMs"), QStringLiteral("点动安全超时。按键/按钮持续按下超过该时间会自动停止，防止松开事件丢失后机构持续运动。")},

        {QStringLiteral("rowWork.enabled"), QStringLiteral("是否启用地图页直线/多垄作业服务。关闭后不再轮询或下发作业命令。")},
        {QStringLiteral("rowWork.gatewayBaseUrl"), QStringLiteral("工控机作业服务基地址，用于下发作业计划、启动/停止任务和查询作业状态。")},
        {QStringLiteral("rowWork.statusPollIntervalMs"), QStringLiteral("作业状态轮询周期。越小状态越实时，但工控机接口请求更频繁。")},
        {QStringLiteral("rowWork.commandTimeoutMs"), QStringLiteral("作业命令请求超时时间，包括计划下发、启动、暂停、停止等操作。")},
        {QStringLiteral("rowWork.autoRefreshPlanStatus"), QStringLiteral("地图页是否自动刷新作业状态。现场联调时可关闭以减少请求干扰。")},

        {QStringLiteral("geo.baseLatitudeDeg"), QStringLiteral("地图本地坐标换算的基准纬度。基准点变化会影响后续坐标换算，不会自动重投影已有点。")},
        {QStringLiteral("geo.baseLongitudeDeg"), QStringLiteral("地图本地坐标换算的基准经度。应与现场地图/定位坐标系保持一致。")},
        {QStringLiteral("control.arrivalDistanceThreshold"), QStringLiteral("距离目标点小于该值时认为到点。过大可能提前到点，过小可能在目标附近反复调整。")},
        {QStringLiteral("control.arrivalAngleThresholdDeg"), QStringLiteral("航向误差小于该角度时认为朝向满足到点条件。")},
        {QStringLiteral("control.maxLinearSpeed"), QStringLiteral("路径跟踪允许的最大前进线速度。现场颠簸、定位抖动或狭窄垄间应适当降低。")},
        {QStringLiteral("control.maxAngularSpeed"), QStringLiteral("路径跟踪允许的最大角速度。过大会导致转向激烈，过小会导致纠偏慢。")},
        {QStringLiteral("control.linearGain"), QStringLiteral("线速度控制增益。越大越积极接近目标，但可能引起速度波动。")},
        {QStringLiteral("control.angularGain"), QStringLiteral("角速度控制增益。越大纠偏越快，但定位噪声大时容易左右摆动。")},
        {QStringLiteral("control.headingStopThresholdDeg"), QStringLiteral("航向误差超过该阈值时停止前进，优先修正朝向，避免偏航较大时继续冲出轨迹。")},
        {QStringLiteral("control.headingSlowdownThresholdDeg"), QStringLiteral("航向误差超过该阈值后开始降低线速度，用于提升大偏差时的行驶稳定性。")},
        {QStringLiteral("control.headingSlowdownFactor"), QStringLiteral("航向减速系数。数值越小，大航向误差时前进速度降得越多。")},
        {QStringLiteral("control.nearTargetDistanceMultiplier"), QStringLiteral("近目标区距离倍率。到点阈值乘以该倍率后进入近目标减速区。")},
        {QStringLiteral("control.nearTargetSpeedMultiplier"), QStringLiteral("近目标区速度倍率。越小越稳，但到点耗时更长。")},
        {QStringLiteral("control.linearAccelerationLimit"), QStringLiteral("线速度加速限制，用于避免前进速度突变导致车体冲击或打滑。")},
        {QStringLiteral("control.linearDecelerationLimit"), QStringLiteral("线速度减速限制，用于控制减速平顺性。过小会导致停车距离变长。")},
        {QStringLiteral("control.angularAccelerationLimit"), QStringLiteral("角速度加速限制，用于避免转向指令突变。")},
        {QStringLiteral("control.angularDecelerationLimit"), QStringLiteral("角速度减速限制，用于控制停止转向时的平顺性。")},
        {QStringLiteral("control.finalAdjustLinearSpeed"), QStringLiteral("终点姿态微调阶段允许的线速度。通常应小于正常最大线速度。")},
        {QStringLiteral("control.finalAdjustAngularSpeed"), QStringLiteral("终点姿态微调阶段允许的角速度。用于最后对准目标朝向。")},
        {QStringLiteral("control.routeFollowerUpdateIntervalMs"), QStringLiteral("路径跟踪控制循环周期。周期越短响应越快，但控制计算和通信更频繁。")},
        {QStringLiteral("control.manualMotionRepeatIntervalMs"), QStringLiteral("首页手动速度指令长按连发周期。越小遥控响应越连续，但底盘通信压力越高。")},

        {QStringLiteral("vehicle.wheelBaseMeters"), QStringLiteral("左右轮中心距。用于车辆几何标定和后续里程计/控制换算，应按实车测量填写。")},
        {QStringLiteral("vehicle.wheelDiameterMeters"), QStringLiteral("车轮直径。用于速度、里程和轮速换算，应按实际轮胎外径填写。")},
        {QStringLiteral("vehicle.gearReduction"), QStringLiteral("电机到车轮的总减速比。用于后续电机转速与车轮速度换算。")},

        {QStringLiteral("logging.level"), QStringLiteral("运行日志最低输出等级。debug 最详细，info 适合调试，warn/error 适合现场稳定运行。")},
        {QStringLiteral("logging.consoleEnabled"), QStringLiteral("是否输出到 Qt Creator 应用程序输出或终端。现场发布版可关闭以减少控制台噪声。")},
        {QStringLiteral("logging.fileEnabled"), QStringLiteral("是否写入运行日志文件。生产维护建议开启。")},
        {QStringLiteral("logging.includeSourceLocation"), QStringLiteral("是否记录源码文件和行号。定位开发问题有用，但会增加日志长度。")},
        {QStringLiteral("logging.includeThreadId"), QStringLiteral("是否记录线程 ID。排查网络、视频、云台等多线程问题时建议开启。")},
        {QStringLiteral("logging.includeCategory"), QStringLiteral("是否记录模块分类，例如 tenco.net.status 或 tenco.gimbal.control。")},
        {QStringLiteral("logging.maxFileBytes"), QStringLiteral("单个运行日志最大大小。达到上限后自动轮转保留历史日志。")},
        {QStringLiteral("logging.maxBackupFiles"), QStringLiteral("运行日志最多保留数量。数量越大越利于回溯，但占用磁盘更多。")},
        {QStringLiteral("logging.perSessionFile"), QStringLiteral("每次程序启动额外生成独立会话日志，便于定位一次现场运行过程。")},
        {QStringLiteral("logging.auditEnabled"), QStringLiteral("是否启用操作审计日志。用于记录配置应用、诊断导出和关键控制操作。")},
        {QStringLiteral("logging.auditMaxFileBytes"), QStringLiteral("单个审计日志最大大小。达到上限后自动轮转。")},
        {QStringLiteral("logging.auditMaxBackupFiles"), QStringLiteral("审计日志最多保留数量。")},
        {QStringLiteral("logging.redactSensitiveData"), QStringLiteral("是否对 token、Authorization、password、secret 等敏感信息脱敏。生产环境必须开启。")},
        {QStringLiteral("logging.categoryRules"), QStringLiteral("Qt 分类日志过滤规则，每行一条，例如 tenco.net.status.debug=false。高级维护人员使用。")},
        {QStringLiteral("logging.currentLevel"), QStringLiteral("当前运行中实际生效的日志等级。")},
        {QStringLiteral("logging.currentFile"), QStringLiteral("当前运行日志文件路径。")},
        {QStringLiteral("logging.auditFile"), QStringLiteral("当前审计日志文件路径。")},
        {QStringLiteral("logging.actions"), QStringLiteral("日志维护操作：刷新日志状态、打开日志目录和导出现场诊断包。")},

        {QStringLiteral("production.configPath"), QStringLiteral("当前程序实际读取和写入的 config.json 路径，用于确认是否改到了正确配置文件。")},
        {QStringLiteral("production.loadedFromFile"), QStringLiteral("显示当前配置是否来自磁盘文件；否表示程序使用了内置默认配置。")},
        {QStringLiteral("production.runtimeDir"), QStringLiteral("当前程序可执行文件所在目录，用于排查运行目录和配置文件路径问题。")},
        {QStringLiteral("production.backupDir"), QStringLiteral("启动自动备份和手动恢复使用的配置备份目录。")},
        {QStringLiteral("production.backupCount"), QStringLiteral("当前保留的配置备份数量，最多保留 10 个。")},
        {QStringLiteral("production.latestBackup"), QStringLiteral("最近一次配置备份文件，可用于恢复上次配置。")},
        {QStringLiteral("production.actions"), QStringLiteral("生产维护快捷操作：刷新状态、打开目录和执行配置自检。")}
    };

    return descriptions.value(key);
}

void addDescribedRow(QFormLayout *layout, const QString &key, const QString &labelText, QWidget *field)
{
    if (!layout || !field) {
        return;
    }

    const QString description = parameterDescription(key);
    auto *label = new QLabel(labelText);
    label->setObjectName(QStringLiteral("maintenanceFieldLabel"));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setWordWrap(true);

    auto *fieldBox = new QWidget();
    auto *fieldLayout = new QVBoxLayout(fieldBox);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(4);
    fieldLayout->addWidget(field);
    fieldBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    if (!description.isEmpty()) {
        auto *hint = new QLabel(description, fieldBox);
        hint->setObjectName(QStringLiteral("maintenanceFieldHint"));
        hint->setWordWrap(true);
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        fieldLayout->addWidget(hint);
        label->setToolTip(description);
        field->setToolTip(description);
        field->setAccessibleDescription(description);
        fieldBox->setToolTip(description);
    }

    layout->addRow(label, fieldBox);
}
}

Maintenance::Maintenance(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
{
    buildUi();
    loadFromConfig();
}

void Maintenance::buildUi()
{
    if (!ui || !ui->maintenancePage) {
        return;
    }

    if (ui->maintenanceLabel) {
        ui->maintenanceLabel->hide();
    }

    auto *rootLayout = qobject_cast<QVBoxLayout *>(ui->maintenancePage->layout());
    if (!rootLayout) {
        rootLayout = new QVBoxLayout(ui->maintenancePage);
        ui->maintenancePage->setLayout(rootLayout);
    }
    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(16);
    ui->maintenancePage->setObjectName(QStringLiteral("maintenanceRoot"));
    ui->maintenancePage->setStyleSheet(QString::fromUtf8(kMaintenanceStyle));

    auto *hero = new QFrame(ui->maintenancePage);
    hero->setObjectName(QStringLiteral("maintenanceHero"));
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(22, 18, 22, 18);
    heroLayout->setSpacing(18);
    auto *titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(8);
    auto *title = new QLabel(tr("运行配置维护"), hero);
    title->setObjectName(QStringLiteral("maintenanceTitle"));
    auto *desc = new QLabel(tr("维护运行中的基础参数，支持保存到文件或直接热加载到首页、地图和控制模块。"), hero);
    desc->setObjectName(QStringLiteral("maintenanceSubtitle"));
    desc->setWordWrap(true);
    titleBlock->addWidget(title);
    titleBlock->addWidget(desc);
    heroLayout->addLayout(titleBlock, 1);
    auto *hint = new QLabel(tr("建议在修改通信地址、云台参数和作业服务前先确认设备在线。"), hero);
    hint->setObjectName(QStringLiteral("maintenanceHint"));
    hint->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    heroLayout->addWidget(hint);
    rootLayout->addWidget(hero);

    auto *tabs = new QTabWidget(ui->maintenancePage);
    auto wrapScrollable = [](QWidget *content) {
        auto *scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setWidget(content);
        return scroll;
    };
    tabs->addTab(wrapScrollable(createNetworkPage()), tr("通信"));
    tabs->addTab(wrapScrollable(createVideoPage()), tr("视频/相机"));
    tabs->addTab(wrapScrollable(createGimbalPage()), tr("云台"));
    tabs->addTab(wrapScrollable(createRowWorkPage()), tr("直线作业"));
    tabs->addTab(wrapScrollable(createControlPage()), tr("定位/控制"));
    tabs->addTab(wrapScrollable(createVehiclePage()), tr("车辆标定"));
    tabs->addTab(wrapScrollable(createLoggingPage()), tr("日志"));
    tabs->addTab(wrapScrollable(createProductionPage()), tr("生产维护"));
    rootLayout->addWidget(tabs, 1);

    auto *footer = new QFrame(ui->maintenancePage);
    footer->setObjectName(QStringLiteral("maintenanceCard"));
    auto *buttonRow = new QHBoxLayout(footer);
    buttonRow->setContentsMargins(16, 14, 16, 14);
    buttonRow->setSpacing(10);
    m_statusLabel = new QLabel(ui->maintenancePage);
    m_statusLabel->setObjectName(QStringLiteral("maintenanceStatusLabel"));
    m_statusLabel->setWordWrap(true);
    buttonRow->addWidget(m_statusLabel, 1);

    m_loadConfigButton = new QPushButton(tr("加载配置文件"), ui->maintenancePage);
    m_loadConfigButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *reloadButton = new QPushButton(tr("还原运行配置"), ui->maintenancePage);
    reloadButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_restoreBackupButton = new QPushButton(tr("恢复上次配置"), ui->maintenancePage);
    m_restoreBackupButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_saveButton = new QPushButton(tr("保存"), ui->maintenancePage);
    m_saveButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_applyButton = new QPushButton(tr("保存并应用"), ui->maintenancePage);
    m_applyButton->setObjectName(QStringLiteral("maintenancePrimaryButton"));
    m_applyButton->setDefault(true);
    buttonRow->addWidget(m_loadConfigButton);
    buttonRow->addWidget(reloadButton);
    buttonRow->addWidget(m_restoreBackupButton);
    buttonRow->addWidget(m_saveButton);
    buttonRow->addWidget(m_applyButton);
    rootLayout->addWidget(footer);

    connect(m_loadConfigButton, &QPushButton::clicked, this, &Maintenance::loadConfigFile);
    connect(reloadButton, &QPushButton::clicked, this, &Maintenance::loadFromConfig);
    connect(m_restoreBackupButton, &QPushButton::clicked, this, &Maintenance::restoreLatestBackup);
    connect(m_saveButton, &QPushButton::clicked, this, [this]() { saveConfig(false); });
    connect(m_applyButton, &QPushButton::clicked, this, [this]() { saveConfig(true); });
}

QWidget *Maintenance::createFormPage(const QString &titleText, const QString &description, QFormLayout **outLayout) const
{
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(18, 18, 18, 18);
    pageLayout->setSpacing(14);

    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("maintenanceCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 18, 20, 20);
    cardLayout->setSpacing(12);

    auto *pageTitle = new QLabel(titleText, card);
    pageTitle->setObjectName(QStringLiteral("maintenancePageTitle"));
    auto *pageDesc = new QLabel(description, card);
    pageDesc->setObjectName(QStringLiteral("maintenancePageDesc"));
    pageDesc->setWordWrap(true);
    cardLayout->addWidget(pageTitle);
    cardLayout->addWidget(pageDesc);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(20);
    form->setVerticalSpacing(14);
    cardLayout->addLayout(form);
    pageLayout->addWidget(card);

    if (outLayout) {
        *outLayout = form;
    }
    return page;
}

QWidget *Maintenance::createNetworkPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("通信参数"),
                                tr("包含底盘、状态轮询、写寄存器和地图保存的接口地址，以及认证 token 和轮询间隔。"),
                                &layout);
    addLineEdit(layout, QStringLiteral("network.websocketUrl"), tr("底盘 WebSocket"));
    addLineEdit(layout, QStringLiteral("network.statusReadUrl"), tr("状态读取 HTTP"));
    addLineEdit(layout, QStringLiteral("network.writeInsUrl"), tr("写寄存器 HTTP"));
    addLineEdit(layout, QStringLiteral("network.saveFileUrl"), tr("地图保存 HTTP"));
    addLineEdit(layout, QStringLiteral("network.authToken"), tr("认证 Token"));
    addSpinBox(layout, QStringLiteral("network.statusPollIntervalMs"), tr("状态轮询间隔"), 50, 5000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.statusRequestTimeoutMs"), tr("状态请求超时"), 500, 30000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.statusMaxBackoffMs"), tr("状态失败退避上限"), 500, 60000, QStringLiteral(" ms"));
    addCheckBox(layout, QStringLiteral("network.chassisAutoReconnect"), tr("底盘自动重连"));
    addSpinBox(layout, QStringLiteral("network.chassisReconnectIntervalMs"), tr("底盘重连基础间隔"), 200, 60000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.chassisReconnectMaxIntervalMs"), tr("底盘重连最大间隔"), 200, 120000, QStringLiteral(" ms"));
    return page;
}

QWidget *Maintenance::createVideoPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("视频与相机"),
                                tr("控制首页预览流、工控机相机控制地址和视频流选项。支持 MJPEG 与 OAK 后端切换。"),
                                &layout);
    addComboBox(layout, QStringLiteral("video.backend"), tr("视频后端"), {QStringLiteral("mjpeg_http"), QStringLiteral("oak_depthai")});
    addLineEdit(layout, QStringLiteral("video.streamUrl"), tr("默认预览流 URL"));
    addLineEdit(layout, QStringLiteral("video.controlBaseUrl"), tr("相机控制基地址"));
    addPlainTextEdit(layout, QStringLiteral("video.streamOptions"), tr("视频流选项 JSON"), tr("[{\"name\":\"前置彩色相机\",\"url\":\"http://...\"}]"));
    addLineEdit(layout, QStringLiteral("video.deviceId"), tr("OAK 设备 ID"));
    addSpinBox(layout, QStringLiteral("video.previewWidth"), tr("预览宽度"), 320, 4096);
    addSpinBox(layout, QStringLiteral("video.previewHeight"), tr("预览高度"), 240, 3040);
    addSpinBox(layout, QStringLiteral("video.previewFps"), tr("预览帧率"), 1, 120);
    addComboBox(layout, QStringLiteral("video.recordMode"), tr("录像模式"), {QStringLiteral("host_opencv")});
    addLineEdit(layout, QStringLiteral("video.recordCodec"), tr("录像编码"));
    addSpinBox(layout, QStringLiteral("video.reconnectIntervalMs"), tr("重连间隔"), 200, 30000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("video.cameraRequestTimeoutMs"), tr("相机控制请求超时"), 1000, 30000, QStringLiteral(" ms"));
    addCheckBox(layout, QStringLiteral("video.autoStart"), tr("启动后自动预览"));
    addCheckBox(layout, QStringLiteral("video.scaleContents"), tr("视频自适应拉伸"));
    return page;
}

QWidget *Maintenance::createGimbalPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("云台控制"),
                                tr("配置 PLC 地址、Modbus 参数、控制寄存器与软限位。修改后会立即影响云台控制。"),
                                &layout);
    addCheckBox(layout, QStringLiteral("gimbal.enabled"), tr("启用云台控制"));
    addLineEdit(layout, QStringLiteral("gimbal.plcHost"), tr("PLC 地址"));
    addSpinBox(layout, QStringLiteral("gimbal.plcPort"), tr("PLC 端口"), 1, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.unitId"), tr("Modbus Unit ID"), 1, 255);
    addSpinBox(layout, QStringLiteral("gimbal.requestTimeoutMs"), tr("请求超时"), 200, 10000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("gimbal.statusPollIntervalMs"), tr("状态轮询间隔"), 100, 10000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("gimbal.heightControlAddress"), tr("升降控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.pitchControlAddress"), tr("俯仰控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.yawControlAddress"), tr("水平控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.statusStartAddress"), tr("状态起始寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.statusRegisterCount"), tr("状态寄存器数量"), 7, 64);
    addSpinBox(layout, QStringLiteral("gimbal.minHeight"), tr("最低高度软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.maxHeight"), tr("最高高度软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.minYaw"), tr("右旋软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.maxYaw"), tr("左旋软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.minPitch"), tr("上仰软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.maxPitch"), tr("下俯软限位"), -32768, 32767);
    addSpinBox(layout, QStringLiteral("gimbal.safetyStopTimeoutMs"), tr("云台点动安全超时"), 300, 30000, QStringLiteral(" ms"));
    return page;
}

QWidget *Maintenance::createRowWorkPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("直线作业"),
                                tr("配置工控机作业服务地址、状态轮询和命令超时。适用于行间作业状态同步。"),
                                &layout);
    addCheckBox(layout, QStringLiteral("rowWork.enabled"), tr("启用作业服务"));
    addLineEdit(layout, QStringLiteral("rowWork.gatewayBaseUrl"), tr("工控机作业服务"));
    addSpinBox(layout, QStringLiteral("rowWork.statusPollIntervalMs"), tr("状态轮询间隔"), 100, 10000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("rowWork.commandTimeoutMs"), tr("命令超时"), 1000, 20000, QStringLiteral(" ms"));
    addCheckBox(layout, QStringLiteral("rowWork.autoRefreshPlanStatus"), tr("自动刷新作业状态"));
    return page;
}

QWidget *Maintenance::createControlPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("定位与控制"),
                                tr("包含地图基准坐标和路径跟踪控制参数。修改后会影响地图换算和跟踪控制。"),
                                &layout);
    addDoubleSpinBox(layout, QStringLiteral("geo.baseLatitudeDeg"), tr("地图基准纬度"), -90.0, 90.0, 8);
    addDoubleSpinBox(layout, QStringLiteral("geo.baseLongitudeDeg"), tr("地图基准经度"), -180.0, 180.0, 8);
    addDoubleSpinBox(layout, QStringLiteral("control.arrivalDistanceThreshold"), tr("到点距离阈值"), 0.01, 10.0, 3, QStringLiteral(" m"));
    addDoubleSpinBox(layout, QStringLiteral("control.arrivalAngleThresholdDeg"), tr("到点角度阈值"), 0.0, 180.0, 2, QStringLiteral(" deg"));
    addDoubleSpinBox(layout, QStringLiteral("control.maxLinearSpeed"), tr("最大线速度"), 0.0, 5.0, 3, QStringLiteral(" m/s"));
    addDoubleSpinBox(layout, QStringLiteral("control.maxAngularSpeed"), tr("最大角速度"), 0.0, 5.0, 3, QStringLiteral(" rad/s"));
    addDoubleSpinBox(layout, QStringLiteral("control.linearGain"), tr("线速度增益"), 0.0, 20.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.angularGain"), tr("角速度增益"), 0.0, 20.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.headingStopThresholdDeg"), tr("航向停止阈值"), 0.0, 180.0, 2, QStringLiteral(" deg"));
    addDoubleSpinBox(layout, QStringLiteral("control.headingSlowdownThresholdDeg"), tr("航向减速阈值"), 0.0, 180.0, 2, QStringLiteral(" deg"));
    addDoubleSpinBox(layout, QStringLiteral("control.headingSlowdownFactor"), tr("航向减速系数"), 0.0, 1.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.nearTargetDistanceMultiplier"), tr("近目标距离倍率"), 1.0, 20.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.nearTargetSpeedMultiplier"), tr("近目标速度倍率"), 0.0, 1.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.linearAccelerationLimit"), tr("线加速度限制"), 0.0, 10.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.linearDecelerationLimit"), tr("线减速度限制"), 0.0, 10.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.angularAccelerationLimit"), tr("角加速度限制"), 0.0, 10.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.angularDecelerationLimit"), tr("角减速度限制"), 0.0, 10.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.finalAdjustLinearSpeed"), tr("终点调整线速度"), 0.0, 5.0, 3);
    addDoubleSpinBox(layout, QStringLiteral("control.finalAdjustAngularSpeed"), tr("终点调整角速度"), 0.0, 5.0, 3);
    addSpinBox(layout, QStringLiteral("control.routeFollowerUpdateIntervalMs"), tr("路径跟踪更新周期"), 20, 1000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("control.manualMotionRepeatIntervalMs"), tr("手动速度连发周期"), 20, 1000, QStringLiteral(" ms"));
    return page;
}

QWidget *Maintenance::createVehiclePage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("车辆标定"),
                                tr("维护底盘几何和传动基础参数。当前主要用于配置留档和后续控制/里程计换算。"),
                                &layout);
    addDoubleSpinBox(layout, QStringLiteral("vehicle.wheelBaseMeters"), tr("轮距"), 0.01, 10.0, 3, QStringLiteral(" m"));
    addDoubleSpinBox(layout, QStringLiteral("vehicle.wheelDiameterMeters"), tr("轮径"), 0.01, 2.0, 3, QStringLiteral(" m"));
    addDoubleSpinBox(layout, QStringLiteral("vehicle.gearReduction"), tr("减速比"), 0.01, 500.0, 3);
    return page;
}

QWidget *Maintenance::createLoggingPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("日志系统"),
                                tr("配置运行日志、审计日志和分类过滤规则。修改后可立即热加载生效。"),
                                &layout);
    addComboBox(layout,
                QStringLiteral("logging.level"),
                tr("最低日志等级"),
                {QStringLiteral("debug"), QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("fatal")});
    addCheckBox(layout, QStringLiteral("logging.consoleEnabled"), tr("输出到控制台"));
    addCheckBox(layout, QStringLiteral("logging.fileEnabled"), tr("写入运行日志文件"));
    addCheckBox(layout, QStringLiteral("logging.includeSourceLocation"), tr("记录源码位置"));
    addCheckBox(layout, QStringLiteral("logging.includeThreadId"), tr("记录线程 ID"));
    addCheckBox(layout, QStringLiteral("logging.includeCategory"), tr("记录分类名称"));
    auto *runtimeSize = addSpinBox(layout, QStringLiteral("logging.maxFileBytes"), tr("运行日志单文件大小"), 256 * 1024, 256 * 1024 * 1024, QStringLiteral(" bytes"));
    if (runtimeSize) {
        runtimeSize->setSingleStep(1024 * 1024);
    }
    addSpinBox(layout, QStringLiteral("logging.maxBackupFiles"), tr("运行日志保留数量"), 1, 99);
    addCheckBox(layout, QStringLiteral("logging.perSessionFile"), tr("生成会话日志文件"));
    addCheckBox(layout, QStringLiteral("logging.auditEnabled"), tr("启用审计日志"));
    auto *auditSize = addSpinBox(layout, QStringLiteral("logging.auditMaxFileBytes"), tr("审计日志单文件大小"), 256 * 1024, 256 * 1024 * 1024, QStringLiteral(" bytes"));
    if (auditSize) {
        auditSize->setSingleStep(1024 * 1024);
    }
    addSpinBox(layout, QStringLiteral("logging.auditMaxBackupFiles"), tr("审计日志保留数量"), 1, 99);
    addCheckBox(layout, QStringLiteral("logging.redactSensitiveData"), tr("敏感信息脱敏"));
    addPlainTextEdit(layout, QStringLiteral("logging.categoryRules"), tr("分类过滤规则"), tr("每行一条，例如：tenco.net.status.debug=false"));

    m_loggingCurrentLevelValue = new QLabel(page);
    m_loggingCurrentFileValue = new QLabel(page);
    m_loggingAuditFileValue = new QLabel(page);
    const QList<QLabel *> labels{m_loggingCurrentLevelValue, m_loggingCurrentFileValue, m_loggingAuditFileValue};
    for (QLabel *label : labels) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        label->setObjectName(QStringLiteral("maintenanceReadonlyValue"));
    }
    addDescribedRow(layout, QStringLiteral("logging.currentLevel"), tr("当前运行等级"), m_loggingCurrentLevelValue);
    addDescribedRow(layout, QStringLiteral("logging.currentFile"), tr("当前运行日志文件"), m_loggingCurrentFileValue);
    addDescribedRow(layout, QStringLiteral("logging.auditFile"), tr("当前审计日志文件"), m_loggingAuditFileValue);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto *refreshButton = new QPushButton(tr("刷新日志状态"), page);
    refreshButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *openLogDirButton = new QPushButton(tr("打开日志目录"), page);
    openLogDirButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *exportDiagButton = new QPushButton(tr("导出诊断包"), page);
    exportDiagButton->setObjectName(QStringLiteral("maintenancePrimaryButton"));
    buttonRow->addWidget(refreshButton);
    buttonRow->addWidget(openLogDirButton);
    buttonRow->addWidget(exportDiagButton);
    buttonRow->addStretch(1);
    auto *buttonContainer = new QWidget(page);
    buttonContainer->setLayout(buttonRow);
    addDescribedRow(layout, QStringLiteral("logging.actions"), tr("日志维护操作"), buttonContainer);

    connect(refreshButton, &QPushButton::clicked, this, &Maintenance::refreshLoggingInfo);
    connect(openLogDirButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(LoggingManager::logDirectoryPath()));
    });
    connect(exportDiagButton, &QPushButton::clicked, this, [this]() {
        QString error;
        const QString target = LoggingManager::exportDiagnostics(QString(), &error);
        if (target.isEmpty()) {
            QMessageBox::critical(ui ? ui->maintenancePage : nullptr, tr("导出诊断包失败"), error);
            return;
        }
        QMessageBox::information(ui ? ui->maintenancePage : nullptr,
                                 tr("导出成功"),
                                 tr("诊断信息已导出到：%1").arg(target));
    });

    refreshLoggingInfo();
    return page;
}

QWidget *Maintenance::createProductionPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("生产维护状态"),
                                tr("展示当前运行目录、配置文件、备份目录和最近备份。该页只读，用于现场确认程序实际使用的配置来源。"),
                                &layout);
    m_configPathValue = new QLabel(page);
    m_backupDirValue = new QLabel(page);
    m_backupCountValue = new QLabel(page);
    m_latestBackupValue = new QLabel(page);
    m_runtimeDirValue = new QLabel(page);
    m_loadedFromFileValue = new QLabel(page);
    const QList<QLabel *> labels{m_configPathValue, m_backupDirValue, m_backupCountValue, m_latestBackupValue, m_runtimeDirValue, m_loadedFromFileValue};
    for (QLabel *label : labels) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        label->setObjectName(QStringLiteral("maintenanceReadonlyValue"));
    }
    addDescribedRow(layout, QStringLiteral("production.configPath"), tr("当前配置文件"), m_configPathValue);
    addDescribedRow(layout, QStringLiteral("production.loadedFromFile"), tr("配置是否来自文件"), m_loadedFromFileValue);
    addDescribedRow(layout, QStringLiteral("production.runtimeDir"), tr("程序运行目录"), m_runtimeDirValue);
    addDescribedRow(layout, QStringLiteral("production.backupDir"), tr("备份目录"), m_backupDirValue);
    addDescribedRow(layout, QStringLiteral("production.backupCount"), tr("备份数量"), m_backupCountValue);
    addDescribedRow(layout, QStringLiteral("production.latestBackup"), tr("最近备份"), m_latestBackupValue);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto *refreshButton = new QPushButton(tr("刷新状态"), page);
    refreshButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *openConfigDirButton = new QPushButton(tr("打开配置目录"), page);
    openConfigDirButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *openBackupDirButton = new QPushButton(tr("打开备份目录"), page);
    openBackupDirButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *selfCheckButton = new QPushButton(tr("配置自检"), page);
    selfCheckButton->setObjectName(QStringLiteral("maintenancePrimaryButton"));
    buttonRow->addWidget(refreshButton);
    buttonRow->addWidget(openConfigDirButton);
    buttonRow->addWidget(openBackupDirButton);
    buttonRow->addWidget(selfCheckButton);
    buttonRow->addStretch(1);
    auto *buttonContainer = new QWidget(page);
    buttonContainer->setLayout(buttonRow);
    addDescribedRow(layout, QStringLiteral("production.actions"), tr("维护操作"), buttonContainer);

    connect(refreshButton, &QPushButton::clicked, this, &Maintenance::refreshProductionInfo);
    connect(selfCheckButton, &QPushButton::clicked, this, &Maintenance::runConfigSelfCheck);
    connect(openConfigDirButton, &QPushButton::clicked, this, []() {
        const QFileInfo configInfo(ConfigManager::instance().configFilePath());
        QDesktopServices::openUrl(QUrl::fromLocalFile(configInfo.absolutePath()));
    });
    connect(openBackupDirButton, &QPushButton::clicked, this, []() {
        const QString backupDir = ConfigManager::instance().backupDirectoryPath();
        QDir().mkpath(backupDir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(backupDir));
    });

    refreshProductionInfo();
    return page;
}

void Maintenance::runConfigSelfCheck()
{
    const auto snap = collectSnapshot();
    QString error;
    if (!validateSnapshot(snap, &error)) {
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("配置自检未通过"), error);
        return;
    }

    QStringList warnings;
    if (!snap.video.streamOptions.isEmpty()) {
        for (const auto &option : snap.video.streamOptions) {
            if (!QUrl(option.url).isValid()) {
                warnings << tr("视频流选项 URL 无效：%1").arg(option.url);
            }
        }
    }
    if (snap.video.backend == QStringLiteral("oak_depthai") && snap.video.deviceId.trimmed().isEmpty()) {
        warnings << tr("OAK 后端未指定设备 ID：单设备可为空，多设备现场建议指定。");
    }
    if (snap.vehicle.wheelBaseMeters <= 0.0 || snap.vehicle.wheelDiameterMeters <= 0.0 || snap.vehicle.gearReduction <= 0.0) {
        warnings << tr("车辆标定参数必须大于 0。");
    }

    const QString message = warnings.isEmpty()
                                ? tr("配置自检通过。当前表单参数格式、主要范围和路径设置未发现明显问题。")
                                : tr("配置自检完成，但存在以下建议检查项：\n\n%1").arg(warnings.join(QStringLiteral("\n")));
    QMessageBox::information(ui ? ui->maintenancePage : nullptr, tr("配置自检"), message);
}

void Maintenance::refreshProductionInfo()
{
    const ConfigManager &config = ConfigManager::instance();
    const QStringList backups = config.backupFilePaths();
    const QString latest = config.latestBackupFilePath();
    if (m_configPathValue) {
        m_configPathValue->setText(config.configFilePath());
    }
    if (m_loadedFromFileValue) {
        m_loadedFromFileValue->setText(config.loadedFromFile() ? tr("是") : tr("否，当前使用内置默认配置"));
    }
    if (m_runtimeDirValue) {
        m_runtimeDirValue->setText(QCoreApplication::applicationDirPath());
    }
    if (m_backupDirValue) {
        m_backupDirValue->setText(config.backupDirectoryPath());
    }
    if (m_backupCountValue) {
        m_backupCountValue->setText(tr("%1 / 10").arg(backups.size()));
    }
    if (m_latestBackupValue) {
        m_latestBackupValue->setText(latest.isEmpty() ? tr("暂无备份") : latest);
    }
}

QLineEdit *Maintenance::addLineEdit(QFormLayout *layout, const QString &key, const QString &label, const QString &placeholder)
{
    auto *edit = new QLineEdit();
    edit->setPlaceholderText(placeholder);
    applyClickOnlyFocus(edit, this);
    applyFieldSize(edit);
    connect(edit, &QLineEdit::textChanged, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, edit);
    m_lineEdits.insert(key, edit);
    return edit;
}

QSpinBox *Maintenance::addSpinBox(QFormLayout *layout, const QString &key, const QString &label, int min, int max, const QString &suffix)
{
    auto *spin = new QSpinBox();
    spin->setRange(min, max);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    applyClickOnlyFocus(spin, this);
    applyFieldSize(spin);
    connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, createUnitField(spin, suffix));
    m_spinBoxes.insert(key, spin);
    return spin;
}

QDoubleSpinBox *Maintenance::addDoubleSpinBox(QFormLayout *layout, const QString &key, const QString &label, double min, double max, int decimals, const QString &suffix)
{
    auto *spin = new QDoubleSpinBox();
    spin->setRange(min, max);
    spin->setDecimals(decimals);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    applyClickOnlyFocus(spin, this);
    applyFieldSize(spin);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, createUnitField(spin, suffix));
    m_doubleSpinBoxes.insert(key, spin);
    return spin;
}

QCheckBox *Maintenance::addCheckBox(QFormLayout *layout, const QString &key, const QString &label)
{
    auto *check = new QCheckBox();
    applyClickOnlyFocus(check, this);
    connect(check, &QCheckBox::toggled, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, check);
    m_checkBoxes.insert(key, check);
    return check;
}

QComboBox *Maintenance::addComboBox(QFormLayout *layout, const QString &key, const QString &label, const QStringList &items)
{
    auto *combo = new QComboBox();
    combo->addItems(items);
    applyClickOnlyFocus(combo, this);
    applyFieldSize(combo);
    connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, combo);
    m_comboBoxes.insert(key, combo);
    return combo;
}

QPlainTextEdit *Maintenance::addPlainTextEdit(QFormLayout *layout, const QString &key, const QString &label, const QString &placeholder)
{
    auto *edit = new QPlainTextEdit();
    edit->setPlaceholderText(placeholder);
    applyClickOnlyFocus(edit, this);
    edit->setMinimumHeight(110);
    edit->setMinimumWidth(kFieldMinWidth);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(edit, &QPlainTextEdit::textChanged, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, edit);
    m_plainTextEdits.insert(key, edit);
    return edit;
}

bool Maintenance::eventFilter(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (event->type() == QEvent::Wheel && widget && isEditableInput(watched)) {
        clearEditableFocusAround(widget);
        if (scrollParentArea(widget, static_cast<QWheelEvent *>(event))) {
            event->accept();
            return true;
        }
        event->ignore();
        return true;
    }

    if (event->type() == QEvent::FocusIn && widget && isEditableInput(watched)) {
        auto *focusEvent = static_cast<QFocusEvent *>(event);
        if (focusEvent->reason() != Qt::MouseFocusReason &&
            focusEvent->reason() != Qt::ShortcutFocusReason &&
            focusEvent->reason() != Qt::PopupFocusReason) {
            widget->clearFocus();
            event->accept();
            return true;
        }
    }

    return QObject::eventFilter(watched, event);
}

void Maintenance::loadFromConfig()
{
    const auto snap = ConfigManager::instance().snapshot();
    loadSnapshotToForm(snap);
    if (m_statusLabel) {
        m_statusLabel->setText(tr("当前配置文件：%1").arg(ConfigManager::instance().configFilePath()));
    }
}

void Maintenance::loadSnapshotToForm(const ConfigManager::ConfigSnapshot &snap)
{
    m_loading = true;
    setLineValue(QStringLiteral("network.websocketUrl"), snap.network.websocketUrl);
    setLineValue(QStringLiteral("network.statusReadUrl"), snap.network.statusReadUrl);
    setLineValue(QStringLiteral("network.writeInsUrl"), snap.network.writeInsUrl);
    setLineValue(QStringLiteral("network.saveFileUrl"), snap.network.saveFileUrl);
    setLineValue(QStringLiteral("network.authToken"), snap.network.authToken);
    setIntValue(QStringLiteral("network.statusPollIntervalMs"), snap.network.statusPollIntervalMs);
    setIntValue(QStringLiteral("network.statusRequestTimeoutMs"), snap.network.statusRequestTimeoutMs);
    setIntValue(QStringLiteral("network.statusMaxBackoffMs"), snap.network.statusMaxBackoffMs);
    setBoolValue(QStringLiteral("network.chassisAutoReconnect"), snap.network.chassisAutoReconnect);
    setIntValue(QStringLiteral("network.chassisReconnectIntervalMs"), snap.network.chassisReconnectIntervalMs);
    setIntValue(QStringLiteral("network.chassisReconnectMaxIntervalMs"), snap.network.chassisReconnectMaxIntervalMs);

    setComboValue(QStringLiteral("video.backend"), snap.video.backend);
    setLineValue(QStringLiteral("video.streamUrl"), snap.video.streamUrl);
    setLineValue(QStringLiteral("video.controlBaseUrl"), snap.video.controlBaseUrl);
    QJsonArray streamOptions;
    for (const auto &option : snap.video.streamOptions) {
        streamOptions.append(QJsonObject{{QStringLiteral("name"), option.name}, {QStringLiteral("url"), option.url}});
    }
    setPlainTextValue(QStringLiteral("video.streamOptions"),
                      QString::fromUtf8(QJsonDocument(streamOptions).toJson(QJsonDocument::Indented)));
    setLineValue(QStringLiteral("video.deviceId"), snap.video.deviceId);
    setIntValue(QStringLiteral("video.previewWidth"), snap.video.previewWidth);
    setIntValue(QStringLiteral("video.previewHeight"), snap.video.previewHeight);
    setIntValue(QStringLiteral("video.previewFps"), snap.video.previewFps);
    setComboValue(QStringLiteral("video.recordMode"), snap.video.recordMode);
    setLineValue(QStringLiteral("video.recordCodec"), snap.video.recordCodec);
    setIntValue(QStringLiteral("video.reconnectIntervalMs"), snap.video.reconnectIntervalMs);
    setIntValue(QStringLiteral("video.cameraRequestTimeoutMs"), snap.video.cameraRequestTimeoutMs);
    setBoolValue(QStringLiteral("video.autoStart"), snap.video.autoStart);
    setBoolValue(QStringLiteral("video.scaleContents"), snap.video.scaleContents);

    setBoolValue(QStringLiteral("gimbal.enabled"), snap.gimbal.enabled);
    setLineValue(QStringLiteral("gimbal.plcHost"), snap.gimbal.plcHost);
    setIntValue(QStringLiteral("gimbal.plcPort"), snap.gimbal.plcPort);
    setIntValue(QStringLiteral("gimbal.unitId"), snap.gimbal.unitId);
    setIntValue(QStringLiteral("gimbal.requestTimeoutMs"), snap.gimbal.requestTimeoutMs);
    setIntValue(QStringLiteral("gimbal.statusPollIntervalMs"), snap.gimbal.statusPollIntervalMs);
    setIntValue(QStringLiteral("gimbal.heightControlAddress"), snap.gimbal.heightControlAddress);
    setIntValue(QStringLiteral("gimbal.pitchControlAddress"), snap.gimbal.pitchControlAddress);
    setIntValue(QStringLiteral("gimbal.yawControlAddress"), snap.gimbal.yawControlAddress);
    setIntValue(QStringLiteral("gimbal.statusStartAddress"), snap.gimbal.statusStartAddress);
    setIntValue(QStringLiteral("gimbal.statusRegisterCount"), snap.gimbal.statusRegisterCount);
    setIntValue(QStringLiteral("gimbal.minHeight"), snap.gimbal.minHeight);
    setIntValue(QStringLiteral("gimbal.maxHeight"), snap.gimbal.maxHeight);
    setIntValue(QStringLiteral("gimbal.minYaw"), snap.gimbal.minYaw);
    setIntValue(QStringLiteral("gimbal.maxYaw"), snap.gimbal.maxYaw);
    setIntValue(QStringLiteral("gimbal.minPitch"), snap.gimbal.minPitch);
    setIntValue(QStringLiteral("gimbal.maxPitch"), snap.gimbal.maxPitch);
    setIntValue(QStringLiteral("gimbal.safetyStopTimeoutMs"), snap.gimbal.safetyStopTimeoutMs);

    setBoolValue(QStringLiteral("rowWork.enabled"), snap.rowWork.enabled);
    setLineValue(QStringLiteral("rowWork.gatewayBaseUrl"), snap.rowWork.gatewayBaseUrl);
    setIntValue(QStringLiteral("rowWork.statusPollIntervalMs"), snap.rowWork.statusPollIntervalMs);
    setIntValue(QStringLiteral("rowWork.commandTimeoutMs"), snap.rowWork.commandTimeoutMs);
    setBoolValue(QStringLiteral("rowWork.autoRefreshPlanStatus"), snap.rowWork.autoRefreshPlanStatus);

    setComboValue(QStringLiteral("logging.level"), snap.logging.level);
    setBoolValue(QStringLiteral("logging.consoleEnabled"), snap.logging.consoleEnabled);
    setBoolValue(QStringLiteral("logging.fileEnabled"), snap.logging.fileEnabled);
    setBoolValue(QStringLiteral("logging.includeSourceLocation"), snap.logging.includeSourceLocation);
    setBoolValue(QStringLiteral("logging.includeThreadId"), snap.logging.includeThreadId);
    setBoolValue(QStringLiteral("logging.includeCategory"), snap.logging.includeCategory);
    setIntValue(QStringLiteral("logging.maxFileBytes"), static_cast<int>(qMin<qint64>(snap.logging.maxFileBytes, std::numeric_limits<int>::max())));
    setIntValue(QStringLiteral("logging.maxBackupFiles"), snap.logging.maxBackupFiles);
    setBoolValue(QStringLiteral("logging.perSessionFile"), snap.logging.perSessionFile);
    setBoolValue(QStringLiteral("logging.auditEnabled"), snap.logging.auditEnabled);
    setIntValue(QStringLiteral("logging.auditMaxFileBytes"), static_cast<int>(qMin<qint64>(snap.logging.auditMaxFileBytes, std::numeric_limits<int>::max())));
    setIntValue(QStringLiteral("logging.auditMaxBackupFiles"), snap.logging.auditMaxBackupFiles);
    setBoolValue(QStringLiteral("logging.redactSensitiveData"), snap.logging.redactSensitiveData);
    setPlainTextValue(QStringLiteral("logging.categoryRules"), snap.logging.categoryRules.join(QStringLiteral("\n")));

    setDoubleValue(QStringLiteral("geo.baseLatitudeDeg"), snap.geo.baseLatitudeDeg);
    setDoubleValue(QStringLiteral("geo.baseLongitudeDeg"), snap.geo.baseLongitudeDeg);
    setDoubleValue(QStringLiteral("control.arrivalDistanceThreshold"), snap.control.arrivalDistanceThreshold);
    setDoubleValue(QStringLiteral("control.arrivalAngleThresholdDeg"), snap.control.arrivalAngleThresholdDeg);
    setDoubleValue(QStringLiteral("control.maxLinearSpeed"), snap.control.maxLinearSpeed);
    setDoubleValue(QStringLiteral("control.maxAngularSpeed"), snap.control.maxAngularSpeed);
    setDoubleValue(QStringLiteral("control.linearGain"), snap.control.linearGain);
    setDoubleValue(QStringLiteral("control.angularGain"), snap.control.angularGain);
    setDoubleValue(QStringLiteral("control.headingStopThresholdDeg"), snap.control.headingStopThresholdDeg);
    setDoubleValue(QStringLiteral("control.headingSlowdownThresholdDeg"), snap.control.headingSlowdownThresholdDeg);
    setDoubleValue(QStringLiteral("control.headingSlowdownFactor"), snap.control.headingSlowdownFactor);
    setDoubleValue(QStringLiteral("control.nearTargetDistanceMultiplier"), snap.control.nearTargetDistanceMultiplier);
    setDoubleValue(QStringLiteral("control.nearTargetSpeedMultiplier"), snap.control.nearTargetSpeedMultiplier);
    setDoubleValue(QStringLiteral("control.linearAccelerationLimit"), snap.control.linearAccelerationLimit);
    setDoubleValue(QStringLiteral("control.linearDecelerationLimit"), snap.control.linearDecelerationLimit);
    setDoubleValue(QStringLiteral("control.angularAccelerationLimit"), snap.control.angularAccelerationLimit);
    setDoubleValue(QStringLiteral("control.angularDecelerationLimit"), snap.control.angularDecelerationLimit);
    setDoubleValue(QStringLiteral("control.finalAdjustLinearSpeed"), snap.control.finalAdjustLinearSpeed);
    setDoubleValue(QStringLiteral("control.finalAdjustAngularSpeed"), snap.control.finalAdjustAngularSpeed);
    setIntValue(QStringLiteral("control.routeFollowerUpdateIntervalMs"), snap.control.routeFollowerUpdateIntervalMs);
    setIntValue(QStringLiteral("control.manualMotionRepeatIntervalMs"), snap.control.manualMotionRepeatIntervalMs);

    setDoubleValue(QStringLiteral("vehicle.wheelBaseMeters"), snap.vehicle.wheelBaseMeters);
    setDoubleValue(QStringLiteral("vehicle.wheelDiameterMeters"), snap.vehicle.wheelDiameterMeters);
    setDoubleValue(QStringLiteral("vehicle.gearReduction"), snap.vehicle.gearReduction);

    m_cleanFingerprint = snapshotFingerprint(snap);
    m_loading = false;
    updateActionState();
    refreshProductionInfo();
}

ConfigManager::ConfigSnapshot Maintenance::collectSnapshot() const
{
    auto snap = ConfigManager::instance().snapshot();
    snap.network.websocketUrl = lineValue(QStringLiteral("network.websocketUrl"));
    snap.network.statusReadUrl = lineValue(QStringLiteral("network.statusReadUrl"));
    snap.network.writeInsUrl = lineValue(QStringLiteral("network.writeInsUrl"));
    snap.network.saveFileUrl = lineValue(QStringLiteral("network.saveFileUrl"));
    snap.network.authToken = lineValue(QStringLiteral("network.authToken"));
    snap.network.statusPollIntervalMs = intValue(QStringLiteral("network.statusPollIntervalMs"));
    snap.network.statusRequestTimeoutMs = intValue(QStringLiteral("network.statusRequestTimeoutMs"));
    snap.network.statusMaxBackoffMs = intValue(QStringLiteral("network.statusMaxBackoffMs"));
    snap.network.chassisAutoReconnect = boolValue(QStringLiteral("network.chassisAutoReconnect"));
    snap.network.chassisReconnectIntervalMs = intValue(QStringLiteral("network.chassisReconnectIntervalMs"));
    snap.network.chassisReconnectMaxIntervalMs = intValue(QStringLiteral("network.chassisReconnectMaxIntervalMs"));

    snap.video.backend = comboValue(QStringLiteral("video.backend"));
    snap.video.streamUrl = lineValue(QStringLiteral("video.streamUrl"));
    snap.video.controlBaseUrl = lineValue(QStringLiteral("video.controlBaseUrl"));
    snap.video.deviceId = lineValue(QStringLiteral("video.deviceId"));
    snap.video.previewWidth = intValue(QStringLiteral("video.previewWidth"));
    snap.video.previewHeight = intValue(QStringLiteral("video.previewHeight"));
    snap.video.previewFps = intValue(QStringLiteral("video.previewFps"));
    snap.video.recordMode = comboValue(QStringLiteral("video.recordMode"));
    snap.video.recordCodec = lineValue(QStringLiteral("video.recordCodec"));
    snap.video.reconnectIntervalMs = intValue(QStringLiteral("video.reconnectIntervalMs"));
    snap.video.cameraRequestTimeoutMs = intValue(QStringLiteral("video.cameraRequestTimeoutMs"));
    snap.video.autoStart = boolValue(QStringLiteral("video.autoStart"));
    snap.video.scaleContents = boolValue(QStringLiteral("video.scaleContents"));
    snap.video.streamOptions.clear();
    const QJsonDocument streamDoc = QJsonDocument::fromJson(plainTextValue(QStringLiteral("video.streamOptions")).toUtf8());
    if (streamDoc.isArray()) {
        for (const QJsonValue &value : streamDoc.array()) {
            const QJsonObject obj = value.toObject();
            if (obj.isEmpty()) {
                continue;
            }
            ConfigManager::VideoConfig::StreamOption option;
            option.name = obj.value(QStringLiteral("name")).toString().trimmed();
            option.url = obj.value(QStringLiteral("url")).toString().trimmed();
            snap.video.streamOptions.push_back(option);
        }
    }

    snap.gimbal.enabled = boolValue(QStringLiteral("gimbal.enabled"));
    snap.gimbal.plcHost = lineValue(QStringLiteral("gimbal.plcHost"));
    snap.gimbal.plcPort = intValue(QStringLiteral("gimbal.plcPort"));
    snap.gimbal.unitId = intValue(QStringLiteral("gimbal.unitId"));
    snap.gimbal.requestTimeoutMs = intValue(QStringLiteral("gimbal.requestTimeoutMs"));
    snap.gimbal.statusPollIntervalMs = intValue(QStringLiteral("gimbal.statusPollIntervalMs"));
    snap.gimbal.heightControlAddress = intValue(QStringLiteral("gimbal.heightControlAddress"));
    snap.gimbal.pitchControlAddress = intValue(QStringLiteral("gimbal.pitchControlAddress"));
    snap.gimbal.yawControlAddress = intValue(QStringLiteral("gimbal.yawControlAddress"));
    snap.gimbal.statusStartAddress = intValue(QStringLiteral("gimbal.statusStartAddress"));
    snap.gimbal.statusRegisterCount = intValue(QStringLiteral("gimbal.statusRegisterCount"));
    snap.gimbal.minHeight = intValue(QStringLiteral("gimbal.minHeight"));
    snap.gimbal.maxHeight = intValue(QStringLiteral("gimbal.maxHeight"));
    snap.gimbal.minYaw = intValue(QStringLiteral("gimbal.minYaw"));
    snap.gimbal.maxYaw = intValue(QStringLiteral("gimbal.maxYaw"));
    snap.gimbal.minPitch = intValue(QStringLiteral("gimbal.minPitch"));
    snap.gimbal.maxPitch = intValue(QStringLiteral("gimbal.maxPitch"));
    snap.gimbal.safetyStopTimeoutMs = intValue(QStringLiteral("gimbal.safetyStopTimeoutMs"));

    snap.rowWork.enabled = boolValue(QStringLiteral("rowWork.enabled"));
    snap.rowWork.gatewayBaseUrl = lineValue(QStringLiteral("rowWork.gatewayBaseUrl"));
    snap.rowWork.statusPollIntervalMs = intValue(QStringLiteral("rowWork.statusPollIntervalMs"));
    snap.rowWork.commandTimeoutMs = intValue(QStringLiteral("rowWork.commandTimeoutMs"));
    snap.rowWork.autoRefreshPlanStatus = boolValue(QStringLiteral("rowWork.autoRefreshPlanStatus"));

    snap.logging.level = comboValue(QStringLiteral("logging.level"));
    snap.logging.consoleEnabled = boolValue(QStringLiteral("logging.consoleEnabled"));
    snap.logging.fileEnabled = boolValue(QStringLiteral("logging.fileEnabled"));
    snap.logging.includeSourceLocation = boolValue(QStringLiteral("logging.includeSourceLocation"));
    snap.logging.includeThreadId = boolValue(QStringLiteral("logging.includeThreadId"));
    snap.logging.includeCategory = boolValue(QStringLiteral("logging.includeCategory"));
    snap.logging.maxFileBytes = intValue(QStringLiteral("logging.maxFileBytes"));
    snap.logging.maxBackupFiles = intValue(QStringLiteral("logging.maxBackupFiles"));
    snap.logging.perSessionFile = boolValue(QStringLiteral("logging.perSessionFile"));
    snap.logging.auditEnabled = boolValue(QStringLiteral("logging.auditEnabled"));
    snap.logging.auditMaxFileBytes = intValue(QStringLiteral("logging.auditMaxFileBytes"));
    snap.logging.auditMaxBackupFiles = intValue(QStringLiteral("logging.auditMaxBackupFiles"));
    snap.logging.redactSensitiveData = boolValue(QStringLiteral("logging.redactSensitiveData"));
    snap.logging.categoryRules.clear();
    const QStringList ruleLines = plainTextValue(QStringLiteral("logging.categoryRules")).split(QRegularExpression(QStringLiteral("[\r\n]+")),
                                                                                             Qt::SkipEmptyParts);
    for (const QString &rule : ruleLines) {
        const QString trimmed = rule.trimmed();
        if (!trimmed.isEmpty()) {
            snap.logging.categoryRules.push_back(trimmed);
        }
    }

    snap.geo.baseLatitudeDeg = doubleValue(QStringLiteral("geo.baseLatitudeDeg"));
    snap.geo.baseLongitudeDeg = doubleValue(QStringLiteral("geo.baseLongitudeDeg"));
    snap.control.arrivalDistanceThreshold = doubleValue(QStringLiteral("control.arrivalDistanceThreshold"));
    snap.control.arrivalAngleThresholdDeg = doubleValue(QStringLiteral("control.arrivalAngleThresholdDeg"));
    snap.control.maxLinearSpeed = doubleValue(QStringLiteral("control.maxLinearSpeed"));
    snap.control.maxAngularSpeed = doubleValue(QStringLiteral("control.maxAngularSpeed"));
    snap.control.linearGain = doubleValue(QStringLiteral("control.linearGain"));
    snap.control.angularGain = doubleValue(QStringLiteral("control.angularGain"));
    snap.control.headingStopThresholdDeg = doubleValue(QStringLiteral("control.headingStopThresholdDeg"));
    snap.control.headingSlowdownThresholdDeg = doubleValue(QStringLiteral("control.headingSlowdownThresholdDeg"));
    snap.control.headingSlowdownFactor = doubleValue(QStringLiteral("control.headingSlowdownFactor"));
    snap.control.nearTargetDistanceMultiplier = doubleValue(QStringLiteral("control.nearTargetDistanceMultiplier"));
    snap.control.nearTargetSpeedMultiplier = doubleValue(QStringLiteral("control.nearTargetSpeedMultiplier"));
    snap.control.linearAccelerationLimit = doubleValue(QStringLiteral("control.linearAccelerationLimit"));
    snap.control.linearDecelerationLimit = doubleValue(QStringLiteral("control.linearDecelerationLimit"));
    snap.control.angularAccelerationLimit = doubleValue(QStringLiteral("control.angularAccelerationLimit"));
    snap.control.angularDecelerationLimit = doubleValue(QStringLiteral("control.angularDecelerationLimit"));
    snap.control.finalAdjustLinearSpeed = doubleValue(QStringLiteral("control.finalAdjustLinearSpeed"));
    snap.control.finalAdjustAngularSpeed = doubleValue(QStringLiteral("control.finalAdjustAngularSpeed"));
    snap.control.routeFollowerUpdateIntervalMs = intValue(QStringLiteral("control.routeFollowerUpdateIntervalMs"));
    snap.control.manualMotionRepeatIntervalMs = intValue(QStringLiteral("control.manualMotionRepeatIntervalMs"));
    snap.vehicle.wheelBaseMeters = doubleValue(QStringLiteral("vehicle.wheelBaseMeters"));
    snap.vehicle.wheelDiameterMeters = doubleValue(QStringLiteral("vehicle.wheelDiameterMeters"));
    snap.vehicle.gearReduction = doubleValue(QStringLiteral("vehicle.gearReduction"));
    return snap;
}

bool Maintenance::validateSnapshot(const ConfigManager::ConfigSnapshot &snapshot, QString *errorMessage) const
{
    auto requireUrl = [&](const QString &value, const QString &name, bool allowEmpty = false) {
        if (allowEmpty && value.trimmed().isEmpty()) {
            return true;
        }
        const QUrl url(value.trimmed());
        if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty()) {
            if (errorMessage) {
                *errorMessage = tr("%1 无效：%2").arg(name, value);
            }
            return false;
        }
        return true;
    };

    if (!requireUrl(snapshot.network.websocketUrl, tr("底盘 WebSocket"))) {
        return false;
    }
    if (!requireUrl(snapshot.network.statusReadUrl, tr("状态读取 HTTP"))) {
        return false;
    }
    if (!requireUrl(snapshot.network.writeInsUrl, tr("写寄存器 HTTP"))) {
        return false;
    }
    if (!requireUrl(snapshot.network.saveFileUrl, tr("地图保存 HTTP"))) {
        return false;
    }
    if (!requireUrl(snapshot.video.streamUrl, tr("默认预览流 URL"), true)) {
        return false;
    }
    if (!requireUrl(snapshot.video.controlBaseUrl, tr("相机控制基地址"), true)) {
        return false;
    }
    if (snapshot.rowWork.enabled && !requireUrl(snapshot.rowWork.gatewayBaseUrl, tr("作业服务地址"))) {
        return false;
    }
    if (snapshot.gimbal.enabled && snapshot.gimbal.plcHost.trimmed().isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("启用云台控制时 PLC 地址不能为空");
        }
        return false;
    }
    if (snapshot.network.statusMaxBackoffMs < snapshot.network.statusPollIntervalMs) {
        if (errorMessage) {
            *errorMessage = tr("状态失败退避上限不能小于状态轮询间隔");
        }
        return false;
    }
    if (snapshot.network.chassisReconnectMaxIntervalMs < snapshot.network.chassisReconnectIntervalMs) {
        if (errorMessage) {
            *errorMessage = tr("底盘重连最大间隔不能小于基础间隔");
        }
        return false;
    }
    if (snapshot.gimbal.minHeight > snapshot.gimbal.maxHeight ||
        snapshot.gimbal.minYaw > snapshot.gimbal.maxYaw ||
        snapshot.gimbal.minPitch > snapshot.gimbal.maxPitch) {
        if (errorMessage) {
            *errorMessage = tr("云台软限位最小值不能大于最大值");
        }
        return false;
    }
    if (snapshot.vehicle.wheelBaseMeters <= 0.0 ||
        snapshot.vehicle.wheelDiameterMeters <= 0.0 ||
        snapshot.vehicle.gearReduction <= 0.0) {
        if (errorMessage) {
            *errorMessage = tr("车辆标定参数必须大于 0");
        }
        return false;
    }
    if (snapshot.logging.maxFileBytes < 256 * 1024 || snapshot.logging.auditMaxFileBytes < 256 * 1024) {
        if (errorMessage) {
            *errorMessage = tr("日志文件大小不能小于 256 KB");
        }
        return false;
    }
    if (snapshot.logging.maxBackupFiles < 1 || snapshot.logging.auditMaxBackupFiles < 1) {
        if (errorMessage) {
            *errorMessage = tr("日志保留数量必须大于 0");
        }
        return false;
    }

    const QJsonDocument streamDoc = QJsonDocument::fromJson(plainTextValue(QStringLiteral("video.streamOptions")).toUtf8());
    if (!plainTextValue(QStringLiteral("video.streamOptions")).trimmed().isEmpty() && !streamDoc.isArray()) {
        if (errorMessage) {
            *errorMessage = tr("视频流选项必须是 JSON 数组");
        }
        return false;
    }
    return true;
}

bool Maintenance::saveConfig(bool applyAfterSave)
{
    const auto snap = collectSnapshot();
    QString error;
    if (!validateSnapshot(snap, &error)) {
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("配置校验失败"), error);
        if (m_statusLabel) {
            m_statusLabel->setText(tr("保存失败：%1").arg(error));
        }
        return false;
    }

    if (!ConfigManager::instance().saveSnapshot(snap, &error, applyAfterSave)) {
        QMessageBox::critical(ui ? ui->maintenancePage : nullptr, tr("保存配置失败"), error);
        if (m_statusLabel) {
            m_statusLabel->setText(tr("保存失败：%1").arg(error));
        }
        return false;
    }

    const QString message = applyAfterSave ? tr("配置已保存并应用") : tr("配置已保存，未应用到运行中模块");
    m_cleanFingerprint = snapshotFingerprint(snap);
    updateActionState();
    refreshProductionInfo();
    refreshLoggingInfo();
    LoggingManager::audit(QStringLiteral("config.save"),
                          applyAfterSave ? QStringLiteral("applied") : QStringLiteral("saved"),
                          {{QStringLiteral("target"), ConfigManager::instance().configFilePath()},
                           {QStringLiteral("dirty"), QStringLiteral("false")}});
    if (m_statusLabel) {
        m_statusLabel->setText(tr("%1：%2；文件：%3")
                                   .arg(message,
                                        QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")),
                                        ConfigManager::instance().configFilePath()));
    }
    QMessageBox::information(ui ? ui->maintenancePage : nullptr, tr("成功"), message);
    return true;
}

void Maintenance::loadConfigFile()
{
    const QString filePath = QFileDialog::getOpenFileName(ui ? ui->maintenancePage : nullptr,
                                                          tr("选择配置文件"),
                                                          ConfigManager::instance().backupDirectoryPath(),
                                                          tr("配置文件 (*.json)"));
    if (filePath.isEmpty()) {
        return;
    }

    ConfigManager::ConfigSnapshot snap;
    QString error;
    if (!ConfigManager::instance().loadSnapshotFromFile(filePath, &snap, &error)) {
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("加载配置失败"), error);
        return;
    }

    loadSnapshotToForm(snap);
    m_cleanFingerprint = snapshotFingerprint(ConfigManager::instance().snapshot());
    updateActionState();
    refreshLoggingInfo();
    LoggingManager::audit(QStringLiteral("config.load"),
                          QStringLiteral("preview"),
                          {{QStringLiteral("source"), QFileInfo(filePath).fileName()}});
    if (m_statusLabel) {
        m_statusLabel->setText(tr("已加载配置到表单，尚未保存或应用：%1").arg(QFileInfo(filePath).fileName()));
    }
}

void Maintenance::restoreLatestBackup()
{
    QString error;
    const QString latestPath = ConfigManager::instance().latestBackupFilePath();
    if (latestPath.isEmpty()) {
        QMessageBox::information(ui ? ui->maintenancePage : nullptr, tr("提示"), tr("没有可恢复的配置备份"));
        updateActionState();
        return;
    }

    const auto answer = QMessageBox::question(ui ? ui->maintenancePage : nullptr,
                                              tr("恢复上次配置"),
                                              tr("将恢复最近一次备份并立即应用。\n\n备份文件：%1\n\n是否继续？")
                                                  .arg(QFileInfo(latestPath).fileName()));
    if (answer != QMessageBox::Yes) {
        return;
    }

    if (!ConfigManager::instance().restoreLatestBackup(&error, true)) {
        QMessageBox::critical(ui ? ui->maintenancePage : nullptr, tr("恢复失败"), error);
        return;
    }

    loadFromConfig();
    refreshProductionInfo();
    refreshLoggingInfo();
    LoggingManager::audit(QStringLiteral("config.restore_latest"),
                          QStringLiteral("success"),
                          {{QStringLiteral("backup"), QFileInfo(latestPath).fileName()}});
    if (m_statusLabel) {
        m_statusLabel->setText(tr("已恢复并应用上次配置：%1").arg(QFileInfo(latestPath).fileName()));
    }
}

bool Maintenance::hasUnsavedChanges() const
{
    return snapshotFingerprint(collectSnapshot()) != m_cleanFingerprint;
}

void Maintenance::markDirty()
{
    if (m_loading) {
        return;
    }
    updateActionState();
}

void Maintenance::updateActionState()
{
    const bool dirty = !m_loading && hasUnsavedChanges();
    const bool hasBackup = !ConfigManager::instance().latestBackupFilePath().isEmpty();
    const bool canRestoreBackup = hasBackup && (dirty || !ConfigManager::instance().currentConfigMatchesLatestBackup());
    if (m_saveButton) {
        m_saveButton->setEnabled(dirty);
    }
    if (m_applyButton) {
        m_applyButton->setEnabled(dirty);
    }
    if (m_restoreBackupButton) {
        m_restoreBackupButton->setEnabled(canRestoreBackup);
    }
}

bool Maintenance::confirmLeaveIfDirty(QWidget *parentWidget)
{
    if (!hasUnsavedChanges()) {
        return true;
    }

    QMessageBox box(parentWidget ? parentWidget : (ui ? ui->maintenancePage : nullptr));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("是否保存本次修改"));
    box.setText(tr("是否保存本次修改？"));
    QPushButton *discardButton = box.addButton(tr("取消"), QMessageBox::DestructiveRole);
    QPushButton *saveButton = box.addButton(tr("保存"), QMessageBox::AcceptRole);
    QPushButton *applyButton = box.addButton(tr("保存并应用"), QMessageBox::AcceptRole);
    box.setDefaultButton(applyButton);
    box.exec();

    if (box.clickedButton() == discardButton) {
        loadFromConfig();
        return true;
    }
    if (box.clickedButton() == saveButton) {
        return saveConfig(false);
    }
    if (box.clickedButton() == applyButton) {
        return saveConfig(true);
    }
    return false;
}

QJsonObject Maintenance::snapshotFingerprint(const ConfigManager::ConfigSnapshot &snapshot) const
{
    QJsonArray streamOptions;
    for (const auto &option : snapshot.video.streamOptions) {
        streamOptions.append(QJsonObject{{QStringLiteral("name"), option.name.trimmed()},
                                         {QStringLiteral("url"), option.url.trimmed()}});
    }

    return QJsonObject{
        {QStringLiteral("geo"),
         QJsonObject{{QStringLiteral("baseLatitudeDeg"), snapshot.geo.baseLatitudeDeg},
                     {QStringLiteral("baseLongitudeDeg"), snapshot.geo.baseLongitudeDeg}}},
        {QStringLiteral("network"),
         QJsonObject{{QStringLiteral("websocketUrl"), snapshot.network.websocketUrl.trimmed()},
                     {QStringLiteral("statusReadUrl"), snapshot.network.statusReadUrl.trimmed()},
                     {QStringLiteral("writeInsUrl"), snapshot.network.writeInsUrl.trimmed()},
                     {QStringLiteral("saveFileUrl"), snapshot.network.saveFileUrl.trimmed()},
                     {QStringLiteral("authToken"), snapshot.network.authToken.trimmed()},
                     {QStringLiteral("statusPollIntervalMs"), snapshot.network.statusPollIntervalMs},
                     {QStringLiteral("statusRequestTimeoutMs"), snapshot.network.statusRequestTimeoutMs},
                     {QStringLiteral("statusMaxBackoffMs"), snapshot.network.statusMaxBackoffMs},
                     {QStringLiteral("chassisAutoReconnect"), snapshot.network.chassisAutoReconnect},
                     {QStringLiteral("chassisReconnectIntervalMs"), snapshot.network.chassisReconnectIntervalMs},
                     {QStringLiteral("chassisReconnectMaxIntervalMs"), snapshot.network.chassisReconnectMaxIntervalMs}}},
        {QStringLiteral("video"),
         QJsonObject{{QStringLiteral("backend"), snapshot.video.backend.trimmed()},
                     {QStringLiteral("streamUrl"), snapshot.video.streamUrl.trimmed()},
                     {QStringLiteral("controlBaseUrl"), snapshot.video.controlBaseUrl.trimmed()},
                     {QStringLiteral("streamOptions"), streamOptions},
                     {QStringLiteral("deviceId"), snapshot.video.deviceId.trimmed()},
                     {QStringLiteral("previewWidth"), snapshot.video.previewWidth},
                     {QStringLiteral("previewHeight"), snapshot.video.previewHeight},
                     {QStringLiteral("previewFps"), snapshot.video.previewFps},
                     {QStringLiteral("recordMode"), snapshot.video.recordMode.trimmed()},
                     {QStringLiteral("recordCodec"), snapshot.video.recordCodec.trimmed()},
                     {QStringLiteral("reconnectIntervalMs"), snapshot.video.reconnectIntervalMs},
                     {QStringLiteral("cameraRequestTimeoutMs"), snapshot.video.cameraRequestTimeoutMs},
                     {QStringLiteral("autoStart"), snapshot.video.autoStart},
                     {QStringLiteral("scaleContents"), snapshot.video.scaleContents}}},
        {QStringLiteral("gimbal"),
         QJsonObject{{QStringLiteral("enabled"), snapshot.gimbal.enabled},
                     {QStringLiteral("plcHost"), snapshot.gimbal.plcHost.trimmed()},
                     {QStringLiteral("plcPort"), snapshot.gimbal.plcPort},
                     {QStringLiteral("unitId"), snapshot.gimbal.unitId},
                     {QStringLiteral("requestTimeoutMs"), snapshot.gimbal.requestTimeoutMs},
                     {QStringLiteral("statusPollIntervalMs"), snapshot.gimbal.statusPollIntervalMs},
                     {QStringLiteral("heightControlAddress"), snapshot.gimbal.heightControlAddress},
                     {QStringLiteral("pitchControlAddress"), snapshot.gimbal.pitchControlAddress},
                     {QStringLiteral("yawControlAddress"), snapshot.gimbal.yawControlAddress},
                     {QStringLiteral("statusStartAddress"), snapshot.gimbal.statusStartAddress},
                     {QStringLiteral("statusRegisterCount"), snapshot.gimbal.statusRegisterCount},
                     {QStringLiteral("minHeight"), snapshot.gimbal.minHeight},
                     {QStringLiteral("maxHeight"), snapshot.gimbal.maxHeight},
                     {QStringLiteral("minYaw"), snapshot.gimbal.minYaw},
                     {QStringLiteral("maxYaw"), snapshot.gimbal.maxYaw},
                     {QStringLiteral("minPitch"), snapshot.gimbal.minPitch},
                     {QStringLiteral("maxPitch"), snapshot.gimbal.maxPitch},
                     {QStringLiteral("safetyStopTimeoutMs"), snapshot.gimbal.safetyStopTimeoutMs}}},
        {QStringLiteral("rowWork"),
         QJsonObject{{QStringLiteral("enabled"), snapshot.rowWork.enabled},
                     {QStringLiteral("gatewayBaseUrl"), snapshot.rowWork.gatewayBaseUrl.trimmed()},
                     {QStringLiteral("statusPollIntervalMs"), snapshot.rowWork.statusPollIntervalMs},
                     {QStringLiteral("commandTimeoutMs"), snapshot.rowWork.commandTimeoutMs},
                     {QStringLiteral("autoRefreshPlanStatus"), snapshot.rowWork.autoRefreshPlanStatus}}},
        {QStringLiteral("logging"),
         QJsonObject{{QStringLiteral("level"), snapshot.logging.level.trimmed()},
                     {QStringLiteral("consoleEnabled"), snapshot.logging.consoleEnabled},
                     {QStringLiteral("fileEnabled"), snapshot.logging.fileEnabled},
                     {QStringLiteral("includeSourceLocation"), snapshot.logging.includeSourceLocation},
                     {QStringLiteral("includeThreadId"), snapshot.logging.includeThreadId},
                     {QStringLiteral("includeCategory"), snapshot.logging.includeCategory},
                     {QStringLiteral("maxFileBytes"), static_cast<double>(snapshot.logging.maxFileBytes)},
                     {QStringLiteral("maxBackupFiles"), snapshot.logging.maxBackupFiles},
                     {QStringLiteral("perSessionFile"), snapshot.logging.perSessionFile},
                     {QStringLiteral("auditEnabled"), snapshot.logging.auditEnabled},
                     {QStringLiteral("auditMaxFileBytes"), static_cast<double>(snapshot.logging.auditMaxFileBytes)},
                     {QStringLiteral("auditMaxBackupFiles"), snapshot.logging.auditMaxBackupFiles},
                     {QStringLiteral("redactSensitiveData"), snapshot.logging.redactSensitiveData},
                     {QStringLiteral("categoryRules"),
                      QJsonArray::fromStringList(snapshot.logging.categoryRules)}}},
        {QStringLiteral("control"),
         QJsonObject{{QStringLiteral("arrivalDistanceThreshold"), snapshot.control.arrivalDistanceThreshold},
                     {QStringLiteral("arrivalAngleThresholdDeg"), snapshot.control.arrivalAngleThresholdDeg},
                     {QStringLiteral("maxLinearSpeed"), snapshot.control.maxLinearSpeed},
                     {QStringLiteral("maxAngularSpeed"), snapshot.control.maxAngularSpeed},
                     {QStringLiteral("linearGain"), snapshot.control.linearGain},
                     {QStringLiteral("angularGain"), snapshot.control.angularGain},
                     {QStringLiteral("headingStopThresholdDeg"), snapshot.control.headingStopThresholdDeg},
                     {QStringLiteral("headingSlowdownThresholdDeg"), snapshot.control.headingSlowdownThresholdDeg},
                     {QStringLiteral("headingSlowdownFactor"), snapshot.control.headingSlowdownFactor},
                     {QStringLiteral("nearTargetDistanceMultiplier"), snapshot.control.nearTargetDistanceMultiplier},
                     {QStringLiteral("nearTargetSpeedMultiplier"), snapshot.control.nearTargetSpeedMultiplier},
                     {QStringLiteral("linearAccelerationLimit"), snapshot.control.linearAccelerationLimit},
                     {QStringLiteral("linearDecelerationLimit"), snapshot.control.linearDecelerationLimit},
                     {QStringLiteral("angularAccelerationLimit"), snapshot.control.angularAccelerationLimit},
                     {QStringLiteral("angularDecelerationLimit"), snapshot.control.angularDecelerationLimit},
                     {QStringLiteral("finalAdjustLinearSpeed"), snapshot.control.finalAdjustLinearSpeed},
                     {QStringLiteral("finalAdjustAngularSpeed"), snapshot.control.finalAdjustAngularSpeed},
                     {QStringLiteral("routeFollowerUpdateIntervalMs"), snapshot.control.routeFollowerUpdateIntervalMs},
                     {QStringLiteral("manualMotionRepeatIntervalMs"), snapshot.control.manualMotionRepeatIntervalMs}}},
        {QStringLiteral("vehicle"),
         QJsonObject{{QStringLiteral("wheelBaseMeters"), snapshot.vehicle.wheelBaseMeters},
                     {QStringLiteral("wheelDiameterMeters"), snapshot.vehicle.wheelDiameterMeters},
                     {QStringLiteral("gearReduction"), snapshot.vehicle.gearReduction}}}
    };
}

void Maintenance::refreshLoggingInfo()
{
    const auto settings = LoggingManager::currentSettings();
    if (m_loggingCurrentLevelValue) {
        m_loggingCurrentLevelValue->setText(settings.level);
    }
    if (m_loggingCurrentFileValue) {
        const QString path = LoggingManager::currentLogFilePath();
        m_loggingCurrentFileValue->setText(path.isEmpty() ? tr("未初始化") : path);
    }
    if (m_loggingAuditFileValue) {
        const QString path = LoggingManager::auditLogFilePath();
        m_loggingAuditFileValue->setText(path.isEmpty() ? tr("未初始化") : path);
    }
}

QString Maintenance::lineValue(const QString &key) const { return m_lineEdits.value(key)->text().trimmed(); }
int Maintenance::intValue(const QString &key) const { return m_spinBoxes.value(key)->value(); }
double Maintenance::doubleValue(const QString &key) const { return m_doubleSpinBoxes.value(key)->value(); }
bool Maintenance::boolValue(const QString &key) const { return m_checkBoxes.value(key)->isChecked(); }
QString Maintenance::comboValue(const QString &key) const { return m_comboBoxes.value(key)->currentText().trimmed(); }
QString Maintenance::plainTextValue(const QString &key) const { return m_plainTextEdits.value(key)->toPlainText().trimmed(); }

void Maintenance::setLineValue(const QString &key, const QString &value) { if (auto *w = m_lineEdits.value(key)) w->setText(value); }
void Maintenance::setIntValue(const QString &key, int value) { if (auto *w = m_spinBoxes.value(key)) w->setValue(value); }
void Maintenance::setDoubleValue(const QString &key, double value) { if (auto *w = m_doubleSpinBoxes.value(key)) w->setValue(value); }
void Maintenance::setBoolValue(const QString &key, bool value) { if (auto *w = m_checkBoxes.value(key)) w->setChecked(value); }
void Maintenance::setComboValue(const QString &key, const QString &value)
{
    if (auto *w = m_comboBoxes.value(key)) {
        const int index = w->findText(value);
        if (index >= 0) {
            w->setCurrentIndex(index);
        }
    }
}
void Maintenance::setPlainTextValue(const QString &key, const QString &value)
{
    if (auto *w = m_plainTextEdits.value(key)) {
        w->setPlainText(value);
    }
}

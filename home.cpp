#include "home.h"
#include "statusclient.h"
#include "chassisclient.h"
#include "cameracontrolclient.h"
#include "gimbalcontrolclient.h"
#include "abstractvideosource.h"
#include "mjpegvideosource.h"
#include "oakcameravideosource.h"
#include "motioncommandarbiter.h"
#include "home_status_presenter.h"
#include "homevideopresenter.h"
#include "camerapreviewselection.h"
#include "parameterwheelguard.h"
#include "statusprotocol.h"
#include "configmanager.h"
#include "loggingmanager.h"
#include "ui_mainwindow.h"         
#include "imageswitch.h"         
#include <QDir>              // 读取录像目录时需要主目录定位
#include <QCoreApplication>
#include <QFile>
#include <QDateTime>
#include <QJsonArray>        // 构造批量寄存器读取的请求体
#include <QJsonDocument>     // 将 JSON 请求/响应序列化与解析
#include <QJsonObject>       // 处理单条键值信息
#include <QMessageBox>       // 输入校验提示与结果反馈
#include <QLineEdit>         // 对经纬度输入框执行焦点与选中操作
#include <QComboBox>       // 相机预览选择控件
#include <QDoubleSpinBox>
#include <QRegularExpression>// 校验经纬度格式
#include <QNetworkRequest>   // 配置 HTTP 请求头与目标地址
#include <QNetworkAccessManager> // HTTP 管理器
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <QPushButton>       // 访问 UI 中的按钮控件
#include <QString>           // UI 文本与网络地址处理
#include <QTimer>            // 周期性任务与长按控制的定时器
#include <QUrl>              // 解析 WebSocket 与 HTTP 地址
#include <QKeyEvent>         // 处理 W/A/S/D 键盘事件
#include <QDebug>            // 调试输出
#include <QProgressDialog>   // 展示控制器重启的倒计时
#include <QVector>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

using ManualDirection = HomeManualInputState::Direction;
using ManualSource = HomeManualInputState::Source;
using GimbalKeyAction = HomeGimbalKeyState::Action;

QString gimbalAxisAuditName(GimbalControlClient::Axis axis)
{
    switch (axis) {
    case GimbalControlClient::Axis::Height:
        return QStringLiteral("height");
    case GimbalControlClient::Axis::Pitch:
        return QStringLiteral("pitch");
    case GimbalControlClient::Axis::Yaw:
        return QStringLiteral("yaw");
    }
    return QStringLiteral("unknown");
}

QString gimbalDirectionAuditName(GimbalControlClient::Direction direction)
{
    switch (direction) {
    case GimbalControlClient::Direction::Value1:
        return QStringLiteral("value1");
    case GimbalControlClient::Direction::Value2:
        return QStringLiteral("value2");
    }
    return QStringLiteral("unknown");
}

}

// 构造函数：缓存 UI 指针并准备网络与定时资源
Home::Home(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
    , restartCheckTimer(new QTimer(this))
    , rebootCountdownTimer(new QTimer(this))
    , cameraStatusTimer(new QTimer(this))
    , gimbalSafetyStopTimer(new QTimer(this))
    , rebootProgressDialog(nullptr)
    , m_statusClient(new StatusClient(this))
    , m_chassisClient(new ChassisClient(this))
    , m_motionArbiter(new MotionCommandArbiter(this))
    , rebootRemainingSeconds(0)
{
    initialize();
    m_videoPresenter = std::make_unique<HomeVideoPresenter>(ui ? ui->videoDisplay : nullptr);
    initializeMotionControl();
    ConfigManager &config = ConfigManager::instance();

    m_statusPresenter = std::make_unique<HomeStatusPresenter>(
        ui,
        [this](const QString &text) { logMessage(text); });

    // Status polling (HTTP, worker thread).
    if (m_statusClient) {
        connect(m_statusClient, &StatusClient::statusReceived, this, &Home::handleStatusPacket);
        connect(m_statusClient, &StatusClient::requestFailed, this, &Home::handleNetworkFailure);
    }

    // Chassis WebSocket client.
    if (m_chassisClient) {
        connect(m_chassisClient, &ChassisClient::connected, this, &Home::handleChassisConnected);
        connect(m_chassisClient, &ChassisClient::disconnected, this, &Home::handleChassisDisconnected);
        connect(m_chassisClient, &ChassisClient::errorOccurred, this, &Home::handleChassisError);
    }

    connect(&config, &ConfigManager::configChanged, this, &Home::applyRuntimeConfig);

    rebootCountdownTimer->setInterval(1000);
    rebootCountdownTimer->setSingleShot(false);
    connect(rebootCountdownTimer, &QTimer::timeout, this, &Home::updateRebootProgress);
    gimbalSafetyStopTimer->setSingleShot(true);
    connect(gimbalSafetyStopTimer, &QTimer::timeout, this, [this]() {
        if (m_gimbalMoving) {
            logMessage(tr("云台点动超时，已自动停止"));
            stopGimbal();
        }
    });

    applyRuntimeConfig();
    logMessage(tr("首页模块已初始化，等待操作…"));
}
Home::~Home()
{
    stopMotionForSafety(QStringLiteral("home_closing"));
    if (m_videoSource) {
        m_videoSource->stop();
        m_videoSource->stopRecording();
    }
}
void Home::initialize()
{
    if (!ui) {
        return;
    }
    ParameterWheelGuard::install(ui->homePage);
    ui->battery->setValue(10);
    setupImageSwitches();
    restartCheckTimer->setInterval(500);
    connect(restartCheckTimer, &QTimer::timeout, this, &Home::checkConnectionRestored);
    connect(ui->recordButton, &QPushButton::clicked, this, &Home::record);
    connect(ui->captureButton, &QPushButton::clicked, this, &Home::photo);
    connect(ui->pushButton_restart, &QPushButton::clicked, this, &Home::restartControl);
    ui->lineEdit->setReadOnly(true);
    ui->lineEdit_8->setReadOnly(true);
    ui->lineEdit->setPlaceholderText(tr("待连接工控机"));
    ui->lineEdit_8->setPlaceholderText(tr("待连接工控机"));
    ui->pushButton_9->setText(tr("工控机维护"));
    ui->pushButton_9->setToolTip(tr("启动或重启融合时，统一确认本次基准站坐标"));
    connect(ui->pushButton_9, &QPushButton::clicked, this, &Home::ipcMaintenanceRequested);
    updateOriginDisplay();
    connect(ui->cameraPreviewKind, qOverload<int>(&QComboBox::currentIndexChanged), this, &Home::handlePreviewSelectionChanged);
    connect(ui->videoPreviewButton, &QPushButton::toggled, this, &Home::handlePreviewToggled);
    ui->recordButton->setToolTip(tr("拍照和录像保存彩色 RGB，不受预览类型影响。工控机媒体保存在相机服务 Capture 目录。"));
    ui->captureButton->setToolTip(ui->recordButton->toolTip());
    ui->pushButton_restart->setToolTip(tr("重启底盘控制器，会短时中断底盘通信；不是重启工控机导航服务。"));
    const QString manualSpeedHelp = tr("仅用于首页独立手动控制。自动任务速度和跟踪参数请在维护 → 工控机维护中调整。");
    ui->doubleSpinBox->setToolTip(manualSpeedHelp);
    ui->doubleSpinBox_2->setToolTip(manualSpeedHelp);
    ui->imageSwitch1->setToolTip(tr("启用后按住方向按钮点动，松开停止；不会启动或恢复自动任务。"));
    ui->imageSwitch2->setToolTip(tr("启用后按住 W/A/S/D 手动控制，松开停止；输入框编辑时不触发运动。"));
    ui->stopButton->setToolTip(tr("停止首页手动控制和云台，清除当前按键输入。自动任务请使用地图页“暂停任务”或“中止任务”。"));
    ui->stopButton->setAccessibleName(tr("停止手动控制和云台"));
    for (auto *button : {ui->forwardButton, ui->backwardButton, ui->leftButton, ui->rightButton})
        button->setToolTip(tr("需先开启运动控制；按住点动，松开停止。"));
    connect(ui->forwardButton, &QPushButton::pressed, this, &Home::handleForwardButtonPressed);
    connect(ui->forwardButton, &QPushButton::released, this, &Home::handleForwardButtonReleased);
    connect(ui->backwardButton, &QPushButton::pressed, this, &Home::handleBackwardButtonPressed);
    connect(ui->backwardButton, &QPushButton::released, this, &Home::handleBackwardButtonReleased);
    connect(ui->leftButton, &QPushButton::pressed, this, &Home::handleTurnLeftButtonPressed);
    connect(ui->leftButton, &QPushButton::released, this, &Home::handleTurnLeftButtonReleased);
    connect(ui->rightButton, &QPushButton::pressed, this, &Home::handleTurnRightButtonPressed);
    connect(ui->rightButton, &QPushButton::released, this, &Home::handleTurnRightButtonReleased);
    connect(ui->stopButton, &QPushButton::clicked, this, &Home::handleStopButtonClicked);
    connect(ui->doubleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        updateManualCommandConfig();
    });
    connect(ui->doubleSpinBox_2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double) {
        updateManualCommandConfig();
    });
    connect(ui->pushButton_3, &QPushButton::pressed, this, &Home::handleGimbalUpPressed);
    connect(ui->pushButton_3, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
    connect(ui->pushButton_4, &QPushButton::pressed, this, &Home::handleGimbalDownPressed);
    connect(ui->pushButton_4, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
    connect(ui->pushButton_5, &QPushButton::pressed, this, &Home::handleGimbalLeftPressed);
    connect(ui->pushButton_5, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
    connect(ui->pushButton_6, &QPushButton::pressed, this, &Home::handleGimbalRightPressed);
    connect(ui->pushButton_6, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
    connect(ui->pushButton_7, &QPushButton::pressed, this, &Home::handleGimbalPitchUpPressed);
    connect(ui->pushButton_7, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
    connect(ui->pushButton_8, &QPushButton::pressed, this, &Home::handleGimbalPitchDownPressed);
    connect(ui->pushButton_8, &QPushButton::released, this, &Home::handleGimbalButtonReleased);
}
void Home::setupImageSwitches()
{
    ui->imageSwitch1->setChecked(false);  // “imageSwitch1” 控制按钮授权，默认关闭
    ui->imageSwitch2->setChecked(false);  // “imageSwitch2” 控制键盘授权，默认关闭
    ui->imageSwitch1->setFixedSize(87, 30);  // 调整第一个开关的视觉尺寸以匹配 UI 布局
    ui->imageSwitch2->setFixedSize(87, 30);  // 第二个开关保持与第一个一致的尺寸
    ui->imageSwitch1->setButtonStyle(ImageSwitch::ButtonStyle_1);  // 使用亮色样式区分按钮模式开关
    ui->imageSwitch2->setButtonStyle(ImageSwitch::ButtonStyle_2);  // 键盘模式开关采用另一套配色便于辨识
    connect(ui->imageSwitch1, &ImageSwitch::checkedChanged, this, &Home::handleImageSwitchToggled);  // 开关 1 切换后更新按钮手动控制权限
    connect(ui->imageSwitch2, &ImageSwitch::checkedChanged, this, &Home::handleImageSwitchToggled);  // 开关 2 切换后更新键盘手动控制权限
}
void Home::initializeMotionControl()
{
    if (!m_motionArbiter) {
        return;
    }

    m_motionArbiter->setHeartbeatIntervalMs(m_manualMotionRepeatIntervalMs);
    connect(m_motionArbiter, &MotionCommandArbiter::velocityCommand, this, &Home::sendVelocityCommand);
    connect(m_motionArbiter, &MotionCommandArbiter::safetyStopRequested, this, [this](const QString &reason) {
        LoggingManager::audit(QStringLiteral("safety.motion_stop"),
                              QStringLiteral("accepted"),
                              {{QStringLiteral("reason"), reason}});
        logMessage(tr("运动安全停车：%1").arg(reason));
    });
    updateManualCommandConfig();
}

void Home::applyRuntimeConfig()
{
    applyManualControlConfig();
    applyStatusClientConfig();
    applyChassisClientConfig();
    applyVideoConfig();
    applyCameraControlConfig();
    applyGimbalControlConfig();
    logMessage(tr("运行配置已应用"));
}

void Home::applyManualControlConfig()
{
    const auto &ctrl = ConfigManager::instance().manualControl();
    m_manualMotionRepeatIntervalMs = qBound(20, ctrl.manualMotionRepeatIntervalMs, 100);
    if (ui && ui->doubleSpinBox) {
        ui->doubleSpinBox->setRange(ctrl.maxLinearSpeed > 0 ? VehicleParameters::minOperatingSpeedMps : 0,
                                    ctrl.maxLinearSpeed);
    }
    if (ui && ui->doubleSpinBox_2) ui->doubleSpinBox_2->setRange(0, ctrl.maxAngularSpeed);
    if (m_motionArbiter) {
        m_motionArbiter->setHeartbeatIntervalMs(m_manualMotionRepeatIntervalMs);
    }
    updateManualCommandConfig();
}

void Home::applyStatusClientConfig()
{
    if (!m_statusClient) {
        return;
    }
    const auto &netCfg = ConfigManager::instance().network();
    const QUrl statusUrl(netCfg.statusReadUrl.trimmed());
    if (!statusUrl.isValid()) {
        logMessage(tr("状态轮询地址无效：%1").arg(netCfg.statusReadUrl));
    }
    m_statusClient->configure(statusUrl,
                              StatusProtocol::defaultReadRequests(),
                              netCfg.statusPollIntervalMs,
                              netCfg.authToken,
                              netCfg.statusRequestTimeoutMs,
                              netCfg.statusMaxBackoffMs);
    m_statusClient->start();
}

void Home::applyChassisClientConfig()
{
    if (!m_chassisClient) {
        return;
    }
    const auto &netCfg = ConfigManager::instance().network();
    const QUrl wsUrl(netCfg.websocketUrl.trimmed());
    m_chassisClient->disconnectFromHost();
    m_chassisClient->setUrl(wsUrl);
    m_chassisClient->setAuthorizationToken(netCfg.authToken);
    m_chassisClient->setAutoReconnect(netCfg.chassisAutoReconnect);
    m_chassisClient->setReconnectIntervalMs(netCfg.chassisReconnectIntervalMs);
    m_chassisClient->setReconnectMaxIntervalMs(netCfg.chassisReconnectMaxIntervalMs);
    m_chassisClient->connectToHost();
}

void Home::applyVideoConfig()
{
    if (!isVideoDisplayReady()) {
        return;
    }
    if (!m_previewConfigured) m_previewEnabled = ConfigManager::instance().video().autoStart;
    m_previewConfigured = true;
    resetPreviewSource();
    populateVideoStreamSelector();
    initializeVideoDisplay();
}

void Home::applyCameraControlConfig()
{
    initializeCameraControl();
    initializeCameraStatusPolling();
}

void Home::applyGimbalControlConfig()
{
    initializeGimbalControl();
}

void Home::initializeVideoDisplay()
{
    if (!isVideoDisplayReady()) {
        return;
    }
    const auto &videoCfg = ConfigManager::instance().video();
    if (m_videoPresenter) {
        m_videoPresenter->setLabel(ui->videoDisplay);
        m_videoPresenter->setScaleContents(videoCfg.scaleContents);
    }

    auto *selector = ui->cameraPreviewKind;
    const int index = selector ? selector->currentIndex() : -1;
    const QString unavailable = index < 0 ? tr("未配置相机预览") : selector->itemData(index, Qt::UserRole + 2).toString();
    if (!unavailable.isEmpty()) {
        m_previewEnabled = false;
        updatePreviewButton();
        updateVideoPlaceholder(unavailable);
        return;
    }
    if (!m_videoSource) {
        if (videoCfg.backend == QStringLiteral("oak_depthai")) {
            m_videoSource = new OakCameraVideoSource(videoCfg, this);
        } else {
            m_videoSource = new MjpegVideoSource(this);
        }
        const auto generation = m_previewGeneration;
        auto *source = m_videoSource;
        connect(source, &AbstractVideoSource::frameReceived, this, [this, source, generation](const QImage &frame) {
            if (generation != m_previewGeneration || source != m_videoSource || !m_previewEnabled || !source->isActive()) return;
            handleVideoFrameReceived(frame);
        });
        connect(source, &AbstractVideoSource::stateChanged, this, [this, source, generation](AbstractVideoSource::State state, const QString &message) {
            if (generation != m_previewGeneration || source != m_videoSource || !m_previewEnabled) return;
            if (state == AbstractVideoSource::State::Streaming && !source->isActive()) return;
            const QString type = m_previewKind == "depth" ? tr("深度预览") : tr("彩色预览");
            if (state == AbstractVideoSource::State::Streaming) {
                m_previewMessage.clear();
            } else {
                m_previewMessage = (state == AbstractVideoSource::State::Error || state == AbstractVideoSource::State::Stopped) ?
                    tr("%1暂不可用，等待重连\n请检查相机服务和连接地址").arg(type) : tr("正在连接%1…").arg(type);
                updateVideoPlaceholder(m_previewMessage);
                if (ui && ui->videoDisplay) ui->videoDisplay->setToolTip(message);
            }
            if (!message.isEmpty()) logMessage(type + " · " + message);
        });
    }

    if (!m_videoSource) {
        updateVideoPlaceholder(tr("视频模块未初始化"));
        return;
    }

    m_videoSource->setReconnectIntervalMs(videoCfg.reconnectIntervalMs);
    m_videoSource->setAutoReconnect(true);
    if (auto *mjpegSource = qobject_cast<MjpegVideoSource *>(m_videoSource)) {
        mjpegSource->setMaxDisplayFps(videoCfg.previewFps);
    }

    if (videoCfg.backend != QStringLiteral("oak_depthai"))
        m_videoSource->setStreamUrl(QUrl(selector->currentData().toString()));
    updatePreviewButton();
    if (m_previewEnabled && m_videoSource->isConfigured()) {
        updateVideoPlaceholder(m_previewKind == "depth" ? tr("正在连接深度预览…") : tr("正在连接彩色预览…"));
        m_videoSource->start();
    } else updateVideoPlaceholder(m_previewKind == "depth" ? tr("深度预览未开启") : tr("彩色预览未开启"));
}

void Home::resetPreviewSource()
{
    ++m_previewGeneration;
    m_previewHasFrame = false;
    m_previewMessage.clear();
    if (!m_videoSource) return;
    // Disconnect before stopping: reset/worker notifications from the previous
    // stream cannot repaint an RGB frame under the newly selected depth label.
    disconnect(m_videoSource, nullptr, this, nullptr);
    m_videoSource->setAutoReconnect(false);
    m_videoSource->stop();
    m_videoSource->deleteLater();
    m_videoSource = nullptr;
    updateVideoPlaceholder(tr("预览切换中…"));
}

void Home::updatePreviewButton()
{
    if (!ui || !ui->videoPreviewButton) return;
    const QSignalBlocker blocked(ui->videoPreviewButton);
    ui->videoPreviewButton->setChecked(m_previewEnabled);
    ui->videoPreviewButton->setText(m_previewEnabled ? tr("关闭预览") : tr("开启预览"));
    ui->videoPreviewButton->setToolTip(tr("只控制实时预览，不影响工控机正在进行的录像。"));
}

void Home::initializeCameraStatusPolling()
{
    if (!cameraStatusTimer) {
        return;
    }
    cameraStatusTimer->setSingleShot(false);
    cameraStatusTimer->setInterval(qMax(1000, ConfigManager::instance().video().reconnectIntervalMs));
    if (!cameraStatusTimer->property("configured").toBool()) {
        connect(cameraStatusTimer, &QTimer::timeout, this, &Home::requestRemoteCameraStatus);
        cameraStatusTimer->setProperty("configured", true);
    }
    cameraStatusTimer->stop();
    if (usesRemoteCameraControl()) {
        cameraStatusTimer->start();
    }
}

void Home::initializeGimbalControl()
{
    const auto &gimbalCfg = ConfigManager::instance().gimbal();
    if (!m_gimbalControlClient) {
        m_gimbalControlClient = new GimbalControlClient(this);
        connect(m_gimbalControlClient, &GimbalControlClient::connectionStateChanged, this, [this](bool connected) {
            logMessage(connected ? tr("云台 PLC 已连接") : tr("云台 PLC 已断开"));
        });
        connect(m_gimbalControlClient, &GimbalControlClient::statusReceived, this, &Home::updateGimbalStatus);
        connect(m_gimbalControlClient, &GimbalControlClient::commandSucceeded, this, [this](const QString &operation) {
            if (operation == QStringLiteral("gimbal_status")) {
                return;
            }
            if (!m_activeGimbalAction.isEmpty()) {
                logMessage(tr("%1 成功").arg(m_activeGimbalAction));
                m_activeGimbalAction.clear();
            } else if (operation != QStringLiteral("gimbal_stop_all")) {
                logMessage(tr("%1 成功").arg(operation));
            }
        });
        connect(m_gimbalControlClient, &GimbalControlClient::commandFailed, this, [this](const QString &operation, const QString &message) {
            if (operation == QStringLiteral("gimbal_status")) {
                const QString text = message.isEmpty() ? tr("云台状态暂不可用") : tr("云台状态暂不可用：%1").arg(message);
                logMessage(text);
                if (m_gimbalMoving) {
                    logMessage(tr("云台运动中状态反馈中断，已请求停止"));
                    stopGimbal();
                }
                updateGimbalButtonState();
                return;
            }
            m_gimbalMoving = false;
            if (gimbalSafetyStopTimer) {
                gimbalSafetyStopTimer->stop();
            }
            const QString text = message.isEmpty() ? tr("%1 失败").arg(operation) : tr("%1 失败：%2").arg(operation, message);
            logMessage(text);
            m_activeGimbalAction.clear();
            updateGimbalButtonState();
        });
    }

    GimbalControlClient::Settings settings;
    settings.enabled = gimbalCfg.enabled;
    settings.host = gimbalCfg.plcHost;
    settings.port = gimbalCfg.plcPort;
    settings.unitId = gimbalCfg.unitId;
    settings.requestTimeoutMs = gimbalCfg.requestTimeoutMs;
    settings.statusPollIntervalMs = gimbalCfg.statusPollIntervalMs;
    settings.heightControlAddress = gimbalCfg.heightControlAddress;
    settings.pitchControlAddress = gimbalCfg.pitchControlAddress;
    settings.yawControlAddress = gimbalCfg.yawControlAddress;
    settings.statusStartAddress = gimbalCfg.statusStartAddress;
    settings.statusRegisterCount = gimbalCfg.statusRegisterCount;
    m_gimbalControlClient->stopStatusPolling();
    m_gimbalControlClient->configure(settings);

    updateGimbalButtonState();
    if (gimbalCfg.enabled) {
        logMessage(tr("云台控制已启用：%1:%2").arg(gimbalCfg.plcHost).arg(gimbalCfg.plcPort));
        m_gimbalControlClient->startStatusPolling();
    } else {
        logMessage(tr("云台控制未启用"));
    }
}

void Home::populateVideoStreamSelector()
{
    if (!ui || !ui->cameraPreviewKind) return;
    auto *selector = ui->cameraPreviewKind;
    const QSignalBlocker blocked(selector);
    selector->clear();
    const auto &config = ConfigManager::instance().video();
    const auto streams = CameraPreviewSelection::streams(config.backend, config.streamUrl, config.controlBaseUrl);
    int selected = 0;
    for (const auto &stream : streams) {
        const int index = selector->count();
        selector->addItem(stream.label, stream.url.toString(QUrl::FullyEncoded));
        selector->setItemData(index, stream.kind, Qt::UserRole + 1);
        selector->setItemData(index, stream.reason, Qt::UserRole + 2);
        selector->setItemData(index, stream.available ? tr("仅切换预览；拍照和录像始终保存彩色 RGB。") : stream.reason, Qt::ToolTipRole);
        if (auto *model = qobject_cast<QStandardItemModel *>(selector->model()))
            if (auto *item = model->item(index)) item->setEnabled(stream.available);
        if (stream.kind == m_previewKind && stream.available) selected = index;
    }
    selector->setCurrentIndex(selected);
    m_previewKind = selector->currentData(Qt::UserRole + 1).toString();
    selector->setToolTip(config.backend == "oak_depthai" ? tr("本机 OAK 仅提供彩色预览，深度需要工控机相机服务。") :
        tr("选择彩色或深度预览；预览关闭时只更新选择。拍照和录像始终保存彩色 RGB。"));
}

void Home::initializeCameraControl()
{
    const auto &cfg = ConfigManager::instance();
    const QUrl controlUrl(cfg.video().controlBaseUrl.trimmed());
    if (!controlUrl.isValid() || controlUrl.isEmpty()) {
        if (cameraStatusTimer) {
            cameraStatusTimer->stop();
        }
        if (m_cameraControlClient) {
            m_cameraControlClient->setBaseUrl(QUrl());
        }
        m_remoteCameraServiceAvailable = false;
        m_remoteCameraConnected = false;
        m_remoteCameraRecording = false;
        m_recordFilePath.clear();
        m_hasLoggedRemoteCameraStatus = false;
        updateRecordButtonText(false);
        updateRemoteCameraUiState();
        return;
    }

    if (!m_cameraControlClient) {
        m_cameraControlClient = new CameraControlClient(this);
        connect(m_cameraControlClient, &CameraControlClient::recordingStateChanged, this, &Home::updateRecordButtonText);
        connect(m_cameraControlClient, &CameraControlClient::photoSaved, this, [this](const QString &path, const QString &message) {
            m_remoteCameraServiceAvailable = true;
            QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
            const QString finalMessage = message.isEmpty() ? tr("工控机拍照成功") : message;
            QMessageBox::information(parentWidget,
                                     tr("成功"),
                                     path.isEmpty() ? finalMessage : tr("%1\n\n保存路径：\n%2").arg(finalMessage, path));
            logMessage(path.isEmpty() ? finalMessage : tr("%1：%2").arg(finalMessage, path));
        });
        connect(m_cameraControlClient, &CameraControlClient::recordingStarted, this, [this](const QString &path, const QString &message) {
            m_remoteCameraServiceAvailable = true;
            m_remoteCameraRecording = true;
            const QString finalMessage = message.isEmpty() ? tr("工控机已开始录像") : message;
            m_recordFilePath = path;
            logMessage(path.isEmpty() ? finalMessage : tr("%1：%2").arg(finalMessage, path));
            updateRemoteCameraUiState();
        });
        connect(m_cameraControlClient, &CameraControlClient::recordingStopped, this, [this](const QString &path, const QString &message) {
            m_remoteCameraServiceAvailable = true;
            m_remoteCameraRecording = false;
            QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
            const QString finalMessage = message.isEmpty() ? tr("工控机已停止录像") : message;
            m_recordFilePath.clear();
            QMessageBox::information(parentWidget,
                                     tr("成功"),
                                     path.isEmpty() ? finalMessage : tr("%1\n\n保存路径：\n%2").arg(finalMessage, path));
            logMessage(path.isEmpty() ? finalMessage : tr("%1：%2").arg(finalMessage, path));
            updateRemoteCameraUiState();
        });
        connect(m_cameraControlClient, &CameraControlClient::statusReceived, this, [this](bool cameraConnected, bool recording, const QString &message) {
            const bool serviceRecovered = !m_remoteCameraServiceAvailable;
            m_remoteCameraServiceAvailable = true;
            m_remoteCameraConnected = cameraConnected;
            m_remoteCameraRecording = recording;
            logCameraStatusChange(message);
            if (serviceRecovered) {
                logMessage(tr("工控机相机服务已恢复"));
            }
            updateRecordButtonText(recording);
            updateRemoteCameraUiState();
        });
        connect(m_cameraControlClient, &CameraControlClient::requestFailed, this, [this](const QString &operation, const QString &message) {
            const QString text = tr("远程相机操作失败（%1）：%2").arg(operation, message);
            if (operation == QStringLiteral("status")) {
                if (m_remoteCameraServiceAvailable) {
                    logMessage(text);
                }
                m_remoteCameraServiceAvailable = false;
                m_remoteCameraConnected = false;
                m_remoteCameraRecording = false;
                updateRemoteCameraUiState();
                return;
            }
            logMessage(text);
            QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
            QMessageBox::warning(parentWidget, tr("失败"), text);
        });
    }

    m_cameraControlClient->setBaseUrl(controlUrl);
    m_cameraControlClient->setAuthorizationToken(cfg.network().authToken);
    m_cameraControlClient->setRequestTimeoutMs(cfg.video().cameraRequestTimeoutMs);

    updateRecordButtonText(m_cameraControlClient->isRecording());
    requestRemoteCameraStatus();
}

bool Home::isVideoDisplayReady() const
{
    return m_videoPresenter && m_videoPresenter->isReady();
}

bool Home::usesRemoteCameraControl() const
{
    return ConfigManager::instance().video().backend != "oak_depthai" &&
        m_cameraControlClient && m_cameraControlClient->isConfigured();
}

void Home::updateVideoPlaceholder(const QString &message)
{
    m_previewHasFrame = false;
    if (m_videoPresenter) {
        m_videoPresenter->showPlaceholder(message);
    }
}

void Home::displayVideoFrame(const QImage &image)
{
    if (image.isNull()) return;
    m_previewHasFrame = true;
    if (m_videoPresenter) {
        m_videoPresenter->showFrame(image);
    }
}

void Home::updateRecordButtonText(bool remoteRecordingActive)
{
    if (!ui || !ui->recordButton) {
        return;
    }
    ui->recordButton->setText(remoteRecordingActive ? tr("停止录像") : tr("开始录像"));
}

void Home::updateRemoteCameraUiState()
{
    if (!ui) {
        return;
    }

    if (ui->captureButton) {
        ui->captureButton->setEnabled(!usesRemoteCameraControl() ||
                                      (m_remoteCameraServiceAvailable && m_remoteCameraConnected));
    }
    if (ui->recordButton) {
        ui->recordButton->setEnabled(!usesRemoteCameraControl() ||
                                     (m_remoteCameraServiceAvailable && m_remoteCameraConnected));
    }

    if (!usesRemoteCameraControl()) {
        return;
    }
    if (!m_previewEnabled) return;

    if (!m_remoteCameraServiceAvailable) {
        updateVideoPlaceholder(tr("工控机相机服务不可用，等待恢复..."));
        return;
    }
    if (!m_remoteCameraConnected) {
        updateVideoPlaceholder(tr("工控机相机服务在线，但相机未连接"));
        return;
    }
    if (!m_previewHasFrame) updateVideoPlaceholder(!m_previewMessage.isEmpty() ? m_previewMessage :
        (m_previewKind == "depth" ? tr("深度预览等待画面…") : tr("彩色预览等待画面…")));
}

void Home::requestRemoteCameraStatus()
{
    if (!usesRemoteCameraControl() || !m_cameraControlClient || m_cameraControlClient->isBusy()) {
        return;
    }
    m_cameraControlClient->requestStatus();
}

void Home::logCameraStatusChange(const QString &message)
{
    const QString normalizedMessage = message.trimmed();
    const bool shouldLog = !m_hasLoggedRemoteCameraStatus ||
                           m_lastLoggedRemoteCameraConnected != m_remoteCameraConnected ||
                           m_lastLoggedRemoteCameraRecording != m_remoteCameraRecording ||
                           m_lastRemoteCameraStatusMessage != normalizedMessage;
    if (!shouldLog) {
        return;
    }
    if (!normalizedMessage.isEmpty()) {
        logMessage(normalizedMessage);
    }
    m_lastLoggedRemoteCameraConnected = m_remoteCameraConnected;
    m_lastLoggedRemoteCameraRecording = m_remoteCameraRecording;
    m_lastRemoteCameraStatusMessage = normalizedMessage;
    m_hasLoggedRemoteCameraStatus = true;
}

void Home::updateGimbalStatus(const GimbalStatus &status)
{
    if (!status.valid) {
        return;
    }
    m_lastGimbalStatus = status;
    m_hasGimbalStatus = true;
    if (m_gimbalMoving && m_gimbalControlClient) {
        QString reason;
        const QString activeAction = m_activeGimbalAction;
        if (!canJogGimbal(m_activeGimbalAxis, m_activeGimbalDirection, &reason)) {
            logMessage(activeAction.isEmpty() ? reason : tr("%1 已停止：%2").arg(activeAction, reason));
            stopGimbal();
        }
    }
    updateGimbalButtonState();
}

void Home::jogGimbal(GimbalControlClient::Axis axis, GimbalControlClient::Direction direction, const QString &actionText)
{
    if (!m_gimbalControlClient || !m_gimbalControlClient->isConfigured()) {
        logMessage(tr("云台控制未启用或未配置"));
        LoggingManager::audit(QStringLiteral("gimbal.jog"),
                              QStringLiteral("rejected"),
                              {{QStringLiteral("axis"), gimbalAxisAuditName(axis)},
                               {QStringLiteral("direction"), gimbalDirectionAuditName(direction)},
                               {QStringLiteral("action"), actionText},
                               {QStringLiteral("reason"), QStringLiteral("not_configured")}});
        return;
    }

    QString reason;
    if (!canJogGimbal(axis, direction, &reason)) {
        logMessage(reason);
        LoggingManager::audit(QStringLiteral("gimbal.jog"),
                              QStringLiteral("rejected"),
                              {{QStringLiteral("axis"), gimbalAxisAuditName(axis)},
                               {QStringLiteral("direction"), gimbalDirectionAuditName(direction)},
                               {QStringLiteral("action"), actionText},
                               {QStringLiteral("reason"), reason}});
        stopGimbal();
        return;
    }

    LoggingManager::audit(QStringLiteral("gimbal.jog"),
                          QStringLiteral("accepted"),
                          {{QStringLiteral("axis"), gimbalAxisAuditName(axis)},
                           {QStringLiteral("direction"), gimbalDirectionAuditName(direction)},
                           {QStringLiteral("action"), actionText},
                           {QStringLiteral("height"), QString::number(m_lastGimbalStatus.height)},
                           {QStringLiteral("yaw"), QString::number(m_lastGimbalStatus.yaw)},
                           {QStringLiteral("pitch"), QString::number(m_lastGimbalStatus.pitch)}});
    m_gimbalMoving = true;
    m_activeGimbalAxis = axis;
    m_activeGimbalDirection = direction;
    m_activeGimbalAction = actionText;
    m_gimbalControlClient->jog(axis, direction);
    if (gimbalSafetyStopTimer) {
        if (axis == GimbalControlClient::Axis::Height) {
            gimbalSafetyStopTimer->stop();
        } else {
            gimbalSafetyStopTimer->start(ConfigManager::instance().gimbal().safetyStopTimeoutMs);
        }
    }
    updateGimbalButtonState();
}

void Home::stopGimbal()
{
    const bool wasMoving = m_gimbalMoving;
    const QString action = m_activeGimbalAction;
    const GimbalControlClient::Axis axis = m_activeGimbalAxis;
    const GimbalControlClient::Direction direction = m_activeGimbalDirection;

    m_gimbalMoving = false;
    m_gimbalKeyState.clearActiveAction();
    m_activeGimbalAction.clear();
    if (gimbalSafetyStopTimer) {
        gimbalSafetyStopTimer->stop();
    }
    if (m_gimbalControlClient && m_gimbalControlClient->isConfigured()) {
        m_gimbalControlClient->stopAll();
    }
    if (wasMoving) {
        LoggingManager::audit(QStringLiteral("gimbal.stop"),
                              QStringLiteral("accepted"),
                              {{QStringLiteral("axis"), gimbalAxisAuditName(axis)},
                               {QStringLiteral("direction"), gimbalDirectionAuditName(direction)},
                               {QStringLiteral("action"), action}});
    }
    updateGimbalButtonState();
}

bool Home::canJogGimbal(GimbalControlClient::Axis axis, GimbalControlClient::Direction direction, QString *reason) const
{
    const auto &cfg = ConfigManager::instance().gimbal();
    if (!cfg.enabled) {
        if (reason) {
            *reason = tr("云台控制未启用");
        }
        return false;
    }
    if (!m_hasGimbalStatus) {
        if (reason) {
            *reason = tr("尚未获取云台状态，拒绝动作");
        }
        return false;
    }

    auto reject = [&](const QString &text) {
        if (reason) {
            *reason = text;
        }
        return false;
    };

    switch (axis) {
    case GimbalControlClient::Axis::Height:
        if (direction == GimbalControlClient::Direction::Value1 && m_lastGimbalStatus.height >= cfg.maxHeight) {
            return reject(tr("云台已接近上升软限位：%1").arg(m_lastGimbalStatus.height));
        }
        if (direction == GimbalControlClient::Direction::Value2 && m_lastGimbalStatus.height <= cfg.minHeight) {
            return reject(tr("云台已接近下降软限位：%1").arg(m_lastGimbalStatus.height));
        }
        break;
    case GimbalControlClient::Axis::Pitch:
        if (direction == GimbalControlClient::Direction::Value1 && m_lastGimbalStatus.pitch >= cfg.maxPitch) {
            return reject(tr("云台已接近俯向软限位：%1").arg(m_lastGimbalStatus.pitch));
        }
        if (direction == GimbalControlClient::Direction::Value2 && m_lastGimbalStatus.pitch <= cfg.minPitch) {
            return reject(tr("云台已接近仰向软限位：%1").arg(m_lastGimbalStatus.pitch));
        }
        break;
    case GimbalControlClient::Axis::Yaw:
        if (direction == GimbalControlClient::Direction::Value1 && m_lastGimbalStatus.yaw <= cfg.minYaw) {
            return reject(tr("云台已接近右旋软限位：%1").arg(m_lastGimbalStatus.yaw));
        }
        if (direction == GimbalControlClient::Direction::Value2 && m_lastGimbalStatus.yaw >= cfg.maxYaw) {
            return reject(tr("云台已接近左旋软限位：%1").arg(m_lastGimbalStatus.yaw));
        }
        break;
    }
    return true;
}

void Home::updateGimbalButtonState()
{
    if (!ui) {
        return;
    }

    const bool configured = m_gimbalControlClient && m_gimbalControlClient->isConfigured();
    const bool enabled = configured;

    ui->pushButton_3->setEnabled(enabled);
    ui->pushButton_4->setEnabled(enabled);
    ui->pushButton_5->setEnabled(enabled);
    ui->pushButton_6->setEnabled(enabled);
    ui->pushButton_7->setEnabled(enabled);
    ui->pushButton_8->setEnabled(enabled);
}

void Home::logMessage(const QString &text)
{
    if (!ui || !ui->plainTextEdit) {
        return;
    }
    const QString normalizedText = LoggingManager::redact(text.trimmed());
    if (normalizedText.isEmpty()) {
        return;
    }
    if (m_lastHomeLogMessage == normalizedText) {
        return;
    }
    m_lastHomeLogMessage = normalizedText;
    // Keep UI history and disk writes bounded; motion commands are samples,
    // while capture, connection and failure messages are useful run events.
    ui->plainTextEdit->setMaximumBlockCount(500);
    static const QRegularExpression motionMessage(QStringLiteral("^(前进|后退|左转|右转)(开始|停止)（(按钮|键盘)）$"));
    if (!motionMessage.match(normalizedText).hasMatch()) qInfo().noquote() << tr("首页：") << normalizedText;
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")),
                                  normalizedText);
    ui->plainTextEdit->appendPlainText(line);
}

void Home::handleChassisConnected()
{
    if (m_motionArbiter && m_chassisClient) {
        m_motionArbiter->setChassisConnected(m_chassisClient->isConnected());
    }
    logMessage(tr("WebSocket 已连接"));
}

void Home::handleChassisDisconnected()
{
    if (m_motionArbiter) {
        m_motionArbiter->setChassisConnected(false);
    }
    logMessage(tr("WebSocket 已断开，正在尝试重连..."));
}

void Home::handleChassisError(const QString &errorString)
{
    if (errorString.isEmpty()) {
        logMessage(tr("WebSocket 发生错误"));
        return;
    }
    logMessage(tr("WebSocket 错误：%1").arg(errorString));
}

void Home::handleVideoFrameReceived(const QImage &frame)
{
    displayVideoFrame(frame);
}

void Home::handlePreviewSelectionChanged(int index)
{
    if (!ui || !ui->cameraPreviewKind || index < 0) return;
    const auto reason = ui->cameraPreviewKind->itemData(index, Qt::UserRole + 2).toString();
    if (!reason.isEmpty()) {
        m_previewEnabled = false;
        resetPreviewSource();
        updatePreviewButton();
        updateVideoPlaceholder(reason);
        return;
    }
    m_previewKind = ui->cameraPreviewKind->itemData(index, Qt::UserRole + 1).toString();
    resetPreviewSource();
    initializeVideoDisplay();
}

void Home::handlePreviewToggled(bool enabled)
{
    m_previewEnabled = enabled;
    // A fresh source cancels reconnect timers, drains old decode work and gives
    // every callback a new generation, including a quick stop/start sequence.
    resetPreviewSource();
    initializeVideoDisplay();
}
// 判断按钮手动控制是否被授权
bool Home::isManualControlEnabledForButtons() const
{
    return ui && ui->imageSwitch1->getChecked();  // 开关1 打开后才允许使用界面按钮
}
// 判断键盘 W/A/S/D 控制是否可用
bool Home::isManualControlEnabledForKeys() const
{
    return ui && ui->imageSwitch1->getChecked() && ui->imageSwitch2->getChecked();  // 同时打开两个开关才允许键盘输入
}

void Home::updateManualCommandConfig()
{
    if (!m_motionArbiter) {
        return;
    }

    m_motionArbiter->setManualCommandConfig(
        MotionCommandArbiter::ManualCommandConfig{ui && ui->doubleSpinBox ? ui->doubleSpinBox->value() : 0.0,
                                                    ui && ui->doubleSpinBox_2 ? ui->doubleSpinBox_2->value() : 0.0,
                                                    isManualControlEnabledForButtons(),
                                                    isManualControlEnabledForKeys()});
}

// 判断是否需要继续推送前进速度
bool Home::shouldSendForward() const
{
    return m_manualInputState.isActive(ManualDirection::Forward,
                                       isManualControlEnabledForButtons(),
                                       isManualControlEnabledForKeys());
}
// 判断是否要发送后退速度
bool Home::shouldSendBackward() const
{
    return m_manualInputState.isActive(ManualDirection::Backward,
                                       isManualControlEnabledForButtons(),
                                       isManualControlEnabledForKeys());
}
// 判断是否要发送左转角速度
bool Home::shouldSendTurnLeft() const
{
    return m_manualInputState.isActive(ManualDirection::TurnLeft,
                                       isManualControlEnabledForButtons(),
                                       isManualControlEnabledForKeys());
}
// 判断是否要发送右转角速度
bool Home::shouldSendTurnRight() const
{
    return m_manualInputState.isActive(ManualDirection::TurnRight,
                                       isManualControlEnabledForButtons(),
                                       isManualControlEnabledForKeys());
}
// 通过 WebSocket 向底盘发送速度指令 JSON
void Home::sendVelocityCommand(double xVel, double thetaVel)
{
    if (!m_chassisClient) {
        return;
    }
    m_chassisClient->sendVelocityCommand(xVel, thetaVel);
}
// 开始录像
void Home::startRecording()
{
    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    if (!m_videoSource) {
        return;
    }

    QString path;
    if (!m_videoSource->startRecording(localCaptureDirectory(), &path)) {
        QMessageBox::warning(parentWidget, tr("提示"), tr("无法开始录像：无法写入文件。"));
        return;
    }

    m_recordFilePath = path;
    if (ui && ui->recordButton) {
        ui->recordButton->setText(tr("停止录像"));
    }
    logMessage(tr("开始录像：%1").arg(m_recordFilePath));
}

// 停止录像并保存文件
void Home::stopRecordingAndSave()
{
    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    if (!m_videoSource) {
        return;
    }

    const QString savedPath = m_videoSource->stopRecording();
    if (ui && ui->recordButton) {
        ui->recordButton->setText(tr("开始录像"));
    }
    if (!savedPath.isEmpty()) {
        QMessageBox::information(parentWidget, tr("成功"), tr("录像已保存到:\n%1").arg(savedPath));
        logMessage(tr("录像已保存到 %1").arg(savedPath));
    }
    m_recordFilePath.clear();
}

QString Home::localCaptureDirectory() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("Capture"));
}
// 更新重启倒计时弹窗的显示内容
void Home::updateRebootProgress()
{
    if (!rebootProgressDialog) {  // 对话框可能已经被关闭，停止倒计时
        rebootCountdownTimer->stop();  // 避免定时器空转
        return;
    }
    if (rebootRemainingSeconds > 0) {  // 倒计时进行中，更新剩余时间
        rebootRemainingSeconds--;  // 每秒减少一次
        const int elapsed = 53 - rebootRemainingSeconds;  // 计算已用时间用于进度条
        rebootProgressDialog->setValue(elapsed);  // 同步进度条当前值
        // 刷新提示文字，包含剩余秒数
        // 展示剩余秒数，提醒操作员
        rebootProgressDialog->setLabelText(tr("控制器重启中,请耐心等候..... (%1s)").arg(rebootRemainingSeconds));
    }
    if (rebootRemainingSeconds <= 0) {  // 倒计时结束，立刻清理对话框
        rebootCountdownTimer->stop();  // 停止继续触发 timeout
        rebootProgressDialog->setValue(53);  // 将进度条补齐到 53 秒
        rebootProgressDialog->hide();  // 隐藏进度对话框
    }
}
// 向 HTTP 接口请求最新的运行数据
void Home::handleStatusPacket(const QJsonObject &resp)
{
    if (!m_statusPresenter) {
        return;
    }
    m_statusPresenter->handleStatusPacket(resp);
    checkConnectionRestored();
}

void Home::handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody)
{
    if (!m_statusPresenter) {
        return;
    }
    stopMotionForSafety(QStringLiteral("status_network_failure"));
    if (m_statusPresenter->handleNetworkFailure(httpStatus, errorString, responseBody)) {
        restartCheckTimer->start();
    }
}
// 切换手动控制开关后，更新定时器及可能的运动命令
void Home::handleImageSwitchToggled(bool checked)
{
    Q_UNUSED(checked);  // 仅通过 sender() 判断来源，checked 值不直接使用
    updateManualCommandConfig();
}
// 前进按钮按下：立即发送一次指令并启动连发
void Home::handleForwardButtonPressed()
{
    m_manualInputState.setHeld(ManualDirection::Forward, ManualSource::Button, true);
    updateManualCommandConfig();
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("start"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("forward")},
                           {QStringLiteral("xVel"), QString::number(ui ? ui->doubleSpinBox->value() : 0.0, 'f', 3)},
                           {QStringLiteral("thetaVel"), QStringLiteral("0")}});
    logMessage(tr("前进开始（按钮）"));
}
// 前进按钮抬起：停止连发
void Home::handleForwardButtonReleased()
{
    m_manualInputState.setHeld(ManualDirection::Forward, ManualSource::Button, false);
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Forward, false, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("stop"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("forward")}});
    logMessage(tr("前进停止（按钮）"));
}
// 后退按钮按下：立即发送一次后退命令
void Home::handleBackwardButtonPressed()
{
    m_manualInputState.setHeld(ManualDirection::Backward, ManualSource::Button, true);
    updateManualCommandConfig();
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Backward, true, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("start"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("backward")},
                           {QStringLiteral("xVel"), QString::number(ui ? -ui->doubleSpinBox->value() : 0.0, 'f', 3)},
                           {QStringLiteral("thetaVel"), QStringLiteral("0")}});
    logMessage(tr("后退开始（按钮）"));
}
// 后退按钮抬起：停止后退连发
void Home::handleBackwardButtonReleased()
{
    m_manualInputState.setHeld(ManualDirection::Backward, ManualSource::Button, false);
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Backward, false, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("stop"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("backward")}});
    logMessage(tr("后退停止（按钮）"));
}
// 左转按钮按下：立即推送左转角速度
void Home::handleTurnLeftButtonPressed()
{
    m_manualInputState.setHeld(ManualDirection::TurnLeft, ManualSource::Button, true);
    updateManualCommandConfig();
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, true, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("start"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("turn_left")},
                           {QStringLiteral("xVel"), QStringLiteral("0")},
                           {QStringLiteral("thetaVel"), QString::number(ui ? ui->doubleSpinBox_2->value() : 0.0, 'f', 3)}});
    logMessage(tr("左转开始（按钮）"));
}
// 左转按钮抬起：停止左转连发
void Home::handleTurnLeftButtonReleased()
{
    m_manualInputState.setHeld(ManualDirection::TurnLeft, ManualSource::Button, false);
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, false, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("stop"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("turn_left")}});
    logMessage(tr("左转停止（按钮）"));
}
// 右转按钮按下：立即推送右转角速度
void Home::handleTurnRightButtonPressed()
{
    m_manualInputState.setHeld(ManualDirection::TurnRight, ManualSource::Button, true);
    updateManualCommandConfig();
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, true, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("start"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("turn_right")},
                           {QStringLiteral("xVel"), QStringLiteral("0")},
                           {QStringLiteral("thetaVel"), QString::number(ui ? -ui->doubleSpinBox_2->value() : 0.0, 'f', 3)}});
    logMessage(tr("右转开始（按钮）"));
}
// 右转按钮抬起：停止右转连发
void Home::handleTurnRightButtonReleased()
{
    m_manualInputState.setHeld(ManualDirection::TurnRight, ManualSource::Button, false);
    if (m_motionArbiter) {
        m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, false, false);
    }
    LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                          QStringLiteral("stop"),
                          {{QStringLiteral("source"), QStringLiteral("button")},
                           {QStringLiteral("direction"), QStringLiteral("turn_right")}});
    logMessage(tr("右转停止（按钮）"));
}
// 急停按钮：清除所有输入并发送零速度
void Home::handleStopButtonClicked()
{
    stopMotionForSafety(QStringLiteral("home_stop_button"));
    stopGimbal();
    LoggingManager::audit(QStringLiteral("safety.estop"),
                          QStringLiteral("accepted"),
                          {{QStringLiteral("source"), QStringLiteral("home_stop_button")}});
    logMessage(tr("急停"));
}

void Home::handleGimbalUpPressed()
{
    jogGimbal(GimbalControlClient::Axis::Height,
              GimbalControlClient::Direction::Value1,
              tr("云台上升"));
}

void Home::handleGimbalDownPressed()
{
    jogGimbal(GimbalControlClient::Axis::Height,
              GimbalControlClient::Direction::Value2,
              tr("云台下降"));
}

void Home::handleGimbalLeftPressed()
{
    jogGimbal(GimbalControlClient::Axis::Yaw,
              GimbalControlClient::Direction::Value2,
              tr("云台左旋"));
}

void Home::handleGimbalRightPressed()
{
    jogGimbal(GimbalControlClient::Axis::Yaw,
              GimbalControlClient::Direction::Value1,
              tr("云台右旋"));
}

void Home::handleGimbalPitchUpPressed()
{
    jogGimbal(GimbalControlClient::Axis::Pitch,
              GimbalControlClient::Direction::Value2,
              tr("云台上仰"));
}

void Home::handleGimbalPitchDownPressed()
{
    jogGimbal(GimbalControlClient::Axis::Pitch,
              GimbalControlClient::Direction::Value1,
              tr("云台下俯"));
}

void Home::handleGimbalButtonReleased()
{
    stopGimbal();
}

// 处理 W/A/S/D 键按下，触发对应的运动指令
bool Home::handleKeyPress(int key, Qt::KeyboardModifiers modifiers, bool isAutoRepeat)
{
    if (isAutoRepeat) {  // 忽略长按产生的重复事件
        return false;
    }
    if (handleGimbalKeyPress(key, modifiers)) {
        return true;
    }
    // 未开启键盘控制时不处理也不记录
    if (!isManualControlEnabledForKeys()) {
        return false;
    }
    switch (key) {  // 根据物理按键决定方向
    case Qt::Key_W:  // W 键 -> 前进
        m_manualInputState.setHeld(ManualDirection::Forward, ManualSource::Keyboard, true);
        updateManualCommandConfig();
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Forward, true, true);
        }
        LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                              QStringLiteral("start"),
                              {{QStringLiteral("source"), QStringLiteral("keyboard")},
                               {QStringLiteral("direction"), QStringLiteral("forward")},
                               {QStringLiteral("xVel"), QString::number(ui ? ui->doubleSpinBox->value() : 0.0, 'f', 3)},
                               {QStringLiteral("thetaVel"), QStringLiteral("0")}});
        logMessage(tr("前进开始（键盘）"));
        return true;
    case Qt::Key_S:  // S 键 -> 后退
        m_manualInputState.setHeld(ManualDirection::Backward, ManualSource::Keyboard, true);
        updateManualCommandConfig();
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Backward, true, true);
        }
        LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                              QStringLiteral("start"),
                              {{QStringLiteral("source"), QStringLiteral("keyboard")},
                               {QStringLiteral("direction"), QStringLiteral("backward")},
                               {QStringLiteral("xVel"), QString::number(ui ? -ui->doubleSpinBox->value() : 0.0, 'f', 3)},
                               {QStringLiteral("thetaVel"), QStringLiteral("0")}});
        logMessage(tr("后退开始（键盘）"));
        return true;
    case Qt::Key_A:  // A 键 -> 左转
        m_manualInputState.setHeld(ManualDirection::TurnLeft, ManualSource::Keyboard, true);
        updateManualCommandConfig();
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, true, true);
        }
        LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                              QStringLiteral("start"),
                              {{QStringLiteral("source"), QStringLiteral("keyboard")},
                               {QStringLiteral("direction"), QStringLiteral("turn_left")},
                               {QStringLiteral("xVel"), QStringLiteral("0")},
                               {QStringLiteral("thetaVel"), QString::number(ui ? ui->doubleSpinBox_2->value() : 0.0, 'f', 3)}});
        logMessage(tr("左转开始（键盘）"));
        return true;
    case Qt::Key_D:  // D 键 -> 右转
        m_manualInputState.setHeld(ManualDirection::TurnRight, ManualSource::Keyboard, true);
        updateManualCommandConfig();
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, true, true);
        }
        LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                              QStringLiteral("start"),
                              {{QStringLiteral("source"), QStringLiteral("keyboard")},
                               {QStringLiteral("direction"), QStringLiteral("turn_right")},
                               {QStringLiteral("xVel"), QStringLiteral("0")},
                               {QStringLiteral("thetaVel"), QString::number(ui ? -ui->doubleSpinBox_2->value() : 0.0, 'f', 3)}});
        logMessage(tr("右转开始（键盘）"));
        return true;
    default:  // 其它按键交由基类处理
        return false;
    }
}
// 处理 W/A/S/D 键释放，关闭对应定时器
bool Home::handleKeyRelease(int key, Qt::KeyboardModifiers modifiers, bool isAutoRepeat)
{
    if (isAutoRepeat) {  // 忽略长按重复事件
        return false;
    }
    if (handleGimbalKeyRelease(key, modifiers)) {
        return true;
    }
    const bool keysEnabled = isManualControlEnabledForKeys();
    switch (key) {  // 根据释放的按键更新状态
    case Qt::Key_W:  // W 键抬起
    {
        const bool wasHeld = m_manualInputState.isHeld(ManualDirection::Forward, ManualSource::Keyboard);
        m_manualInputState.setHeld(ManualDirection::Forward, ManualSource::Keyboard, false);
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Forward, false, true);
        }
        if (wasHeld || keysEnabled) {
            LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                                  QStringLiteral("stop"),
                                  {{QStringLiteral("source"), QStringLiteral("keyboard")},
                                   {QStringLiteral("direction"), QStringLiteral("forward")}});
            logMessage(tr("前进停止（键盘）"));
            return true;
        }
        return false;
    }
    case Qt::Key_S:  // S 键抬起
    {
        const bool wasHeld = m_manualInputState.isHeld(ManualDirection::Backward, ManualSource::Keyboard);
        m_manualInputState.setHeld(ManualDirection::Backward, ManualSource::Keyboard, false);
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::Backward, false, true);
        }
        if (wasHeld || keysEnabled) {
            LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                                  QStringLiteral("stop"),
                                  {{QStringLiteral("source"), QStringLiteral("keyboard")},
                                   {QStringLiteral("direction"), QStringLiteral("backward")}});
            logMessage(tr("后退停止（键盘）"));
            return true;
        }
        return false;
    }
    case Qt::Key_A:  // A 键抬起
    {
        const bool wasHeld = m_manualInputState.isHeld(ManualDirection::TurnLeft, ManualSource::Keyboard);
        m_manualInputState.setHeld(ManualDirection::TurnLeft, ManualSource::Keyboard, false);
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnLeft, false, true);
        }
        if (wasHeld || keysEnabled) {
            LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                                  QStringLiteral("stop"),
                                  {{QStringLiteral("source"), QStringLiteral("keyboard")},
                                   {QStringLiteral("direction"), QStringLiteral("turn_left")}});
            logMessage(tr("左转停止（键盘）"));
            return true;
        }
        return false;
    }
    case Qt::Key_D:  // D 键抬起
    {
        const bool wasHeld = m_manualInputState.isHeld(ManualDirection::TurnRight, ManualSource::Keyboard);
        m_manualInputState.setHeld(ManualDirection::TurnRight, ManualSource::Keyboard, false);
        if (m_motionArbiter) {
            m_motionArbiter->setManualInputActive(MotionCommandArbiter::ManualInput::TurnRight, false, true);
        }
        if (wasHeld || keysEnabled) {
            LoggingManager::audit(QStringLiteral("chassis.manual_move"),
                                  QStringLiteral("stop"),
                                  {{QStringLiteral("source"), QStringLiteral("keyboard")},
                                   {QStringLiteral("direction"), QStringLiteral("turn_right")}});
            logMessage(tr("右转停止（键盘）"));
            return true;
        }
        return false;
    }
    default:  // 其它按键交由基类处理
        return false;
    }
}

bool Home::handleGimbalKeyPress(int key, Qt::KeyboardModifiers modifiers)
{
    if (m_gimbalKeyState.handlePress(key, modifiers)) {
        updateGimbalKeyboardMotion();
        return true;
    }
    return false;
}

bool Home::handleGimbalKeyRelease(int key, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(modifiers);
    if (m_gimbalKeyState.handleRelease(key)) {
        updateGimbalKeyboardMotion();
        return true;
    }
    return false;
}

void Home::updateGimbalKeyboardMotion()
{
    if (!m_gimbalControlClient || !m_gimbalControlClient->isConfigured()) {
        return;
    }

    const auto applyKeyboardMotion = [this](GimbalKeyAction action,
                                            GimbalControlClient::Axis axis,
                                            GimbalControlClient::Direction direction,
                                            const QString &actionText) {
        const bool motionChanged = m_gimbalKeyState.hasActiveMotion() &&
                                   m_gimbalKeyState.activeAction() != action;
        if (motionChanged) {
            stopGimbal();
        }
        m_gimbalKeyState.setActiveAction(action);
        jogGimbal(axis, direction, actionText);
    };

    switch (m_gimbalKeyState.currentAction()) {
    case GimbalKeyAction::PitchUp:
        applyKeyboardMotion(GimbalKeyAction::PitchUp,
                            GimbalControlClient::Axis::Pitch,
                            GimbalControlClient::Direction::Value2,
                            tr("云台上仰"));
        return;
    case GimbalKeyAction::PitchDown:
        applyKeyboardMotion(GimbalKeyAction::PitchDown,
                            GimbalControlClient::Axis::Pitch,
                            GimbalControlClient::Direction::Value1,
                            tr("云台下俯"));
        return;
    case GimbalKeyAction::HeightUp:
        applyKeyboardMotion(GimbalKeyAction::HeightUp,
                            GimbalControlClient::Axis::Height,
                            GimbalControlClient::Direction::Value1,
                            tr("云台上升"));
        return;
    case GimbalKeyAction::HeightDown:
        applyKeyboardMotion(GimbalKeyAction::HeightDown,
                            GimbalControlClient::Axis::Height,
                            GimbalControlClient::Direction::Value2,
                            tr("云台下降"));
        return;
    case GimbalKeyAction::YawLeft:
        applyKeyboardMotion(GimbalKeyAction::YawLeft,
                            GimbalControlClient::Axis::Yaw,
                            GimbalControlClient::Direction::Value2,
                            tr("云台左旋"));
        return;
    case GimbalKeyAction::YawRight:
        applyKeyboardMotion(GimbalKeyAction::YawRight,
                            GimbalControlClient::Axis::Yaw,
                            GimbalControlClient::Direction::Value1,
                            tr("云台右旋"));
        return;
    case GimbalKeyAction::None:
        break;
    }

    stopGimbalKeyboardMotion();
}

void Home::stopGimbalKeyboardMotion()
{
    if (!m_gimbalKeyState.hasActiveMotion()) {
        return;
    }
    m_gimbalKeyState.clearActiveAction();
    if (m_gimbalMoving) {
        stopGimbal();
    }
}
// 录像按钮占位实现：后续可接入实际录像逻辑
void Home::record()
{
    const bool remoteControl = usesRemoteCameraControl();
    if (!m_videoSource && !remoteControl) {
        return;
    }

    if (remoteControl) {
        if (!m_cameraControlClient) {
            return;
        }
        if (m_cameraControlClient->isBusy()) {
            QMessageBox::information(ui ? ui->centralwidget : nullptr, tr("提示"), tr("相机命令处理中，请稍后再试。"));
            return;
        }
        if (!m_cameraControlClient->isRecording()) {
            m_cameraControlClient->startRecording();
        } else {
            m_cameraControlClient->stopRecording();
        }
        return;
    }

    if (!m_videoSource) {
        return;
    }
    if (m_previewKind == "depth") {
        QMessageBox::information(ui ? ui->centralwidget : nullptr, tr("彩色录像"),
            tr("深度仅用于预览。请配置工控机相机控制地址，或切回彩色预览后录像。"));
        return;
    }

    // An unconfigured source cannot record.
    if (!m_videoSource->isConfigured()) {
        QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先选择/配置视频流后再录像。"));
        return;
    }

    if (!m_previewEnabled) handlePreviewToggled(true);
    if (!m_videoSource || !m_videoSource->isActive()) return;

    if (!m_videoSource->isRecording()) {
        startRecording();
    } else {
        stopRecordingAndSave();
    }
}
// 截图按钮占位实现
void Home::photo()
{
    const bool remoteControl = usesRemoteCameraControl();
    if (!m_videoSource && !remoteControl) {
        return;
    }

    if (remoteControl) {
        if (!m_cameraControlClient) {
            return;
        }
        if (m_cameraControlClient->isBusy()) {
            QMessageBox::information(ui ? ui->centralwidget : nullptr, tr("提示"), tr("相机命令处理中，请稍后再试。"));
            return;
        }
        m_cameraControlClient->capturePhoto();
        return;
    }

    if (!m_videoSource) {
        return;
    }
    if (m_previewKind == "depth") {
        QMessageBox::information(ui ? ui->centralwidget : nullptr, tr("彩色拍照"),
            tr("深度仅用于预览。请配置工控机相机控制地址，或切回彩色预览后拍照。"));
        return;
    }

    if (m_videoSource->lastFrame().isNull()) {
        QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先开启视频流后再拍照。"));
        return;
    }

    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    QString savedPath;
    if (m_videoSource->saveSnapshot(localCaptureDirectory(), &savedPath)) {
        QMessageBox::information(parentWidget, tr("成功"), tr("已保存到:\n%1").arg(savedPath));
        logMessage(tr("已拍照并保存到 %1").arg(savedPath));
        return;
    }

    QMessageBox::warning(parentWidget, tr("失败"), tr("保存图片失败"));
    logMessage(tr("拍照保存失败"));
}
// 发送控制器重启命令，并弹出 53 秒倒计时
void Home::restartControl()
{
    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    const QMessageBox::StandardButton answer = QMessageBox::question(parentWidget,
                                                                     tr("提示"),
                                                                     tr("是否要重启控制器？"),
                                                                     QMessageBox::Yes | QMessageBox::No,
                                                                     QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    logMessage(tr("正在下发控制器重启指令"));
    qDebug() << "Sending reboot command";
    if (m_chassisClient) {
        m_chassisClient->sendRebootCommand();
    }
    if (m_statusPresenter) {
        m_statusPresenter->clearConnectionRestored();
    }
    restartCheckTimer->start();
    if (!rebootProgressDialog) {
        rebootProgressDialog = new QProgressDialog(tr("控制器重启中..."), QString(), 0, 53, parentWidget ? parentWidget : ui->centralwidget);
        rebootProgressDialog->setWindowTitle(tr("重启控制器"));
        rebootProgressDialog->setWindowModality(Qt::WindowModal);
        rebootProgressDialog->setCancelButton(nullptr);
        rebootProgressDialog->setAutoClose(true);
    }
    rebootCountdownTimer->stop();
    rebootRemainingSeconds = 53;
    rebootProgressDialog->setMaximum(53);
    rebootProgressDialog->setValue(0);
    rebootProgressDialog->setLabelText(tr("控制器重启中... (%1s)").arg(rebootRemainingSeconds));
    rebootProgressDialog->setMinimumDuration(0);
    rebootProgressDialog->show();
    rebootProgressDialog->raise();
    rebootCountdownTimer->start(1000);
}
void Home::setSavedOriginSnapshot(const QJsonObject &origin)
{
    m_savedOriginSnapshot = origin;
    updateOriginDisplay();
}

void Home::setActiveOriginSnapshot(const QJsonObject &origin)
{
    m_activeOriginSnapshot = origin;
    updateOriginDisplay();
}

void Home::setNavigationStatusCurrent(bool current)
{
    m_navigationStatusCurrent = current;
    updateOriginDisplay();
}

void Home::setControlPoseStatus(const ControlPoseSnapshot &pose)
{
    if (m_statusPresenter) m_statusPresenter->setControlPoseStatus(pose);
}

void Home::setPoseMonitoringAvailability(bool current, const QString &reason)
{
    if (m_statusPresenter) m_statusPresenter->setPoseMonitoringAvailability(current, reason);
}

void Home::setTaskStatus(const TrackingSnapshot &status, bool current)
{
    if (m_statusPresenter) m_statusPresenter->setTaskStatus(status, current);
}

void Home::updateOriginDisplay()
{
    if (!ui) return;
    const auto valid = [](const QJsonObject &origin) {
        const auto lat = origin.value("latitude");
        const auto lon = origin.value("longitude");
        return lat.isDouble() && lon.isDouble() && std::isfinite(lat.toDouble()) &&
            std::isfinite(lon.toDouble()) && std::abs(lat.toDouble()) <= 90 && std::abs(lon.toDouble()) <= 180;
    };
    const auto saved = m_savedOriginSnapshot.value("saved").toObject();
    const bool active = valid(m_activeOriginSnapshot);
    const bool savedValid = valid(saved);
    const auto origin = active ? m_activeOriginSnapshot : saved;
    QString detail;
    if (active) {
        detail = (m_navigationStatusCurrent ? tr("导航服务当前运行原点；版本：%1") :
            tr("无线任务监视状态延迟，保留上次运行原点；当前是否生效待更新；版本：%1"))
            .arg(origin.value("originRevision").toString());
        if (savedValid && (saved.value("originRevision") != origin.value("originRevision") ||
            saved.value("latitude") != origin.value("latitude") || saved.value("longitude") != origin.value("longitude") ||
            saved.value("altitude") != origin.value("altitude")))
            detail += tr("\n工控机已保存不同原点，需在融合启动确认后重新绑定地图与任务。");
    } else if (savedValid) {
        detail = tr("工控机已保存原点；尚未确认导航运行生效。启动融合时需确认本次坐标。");
    } else {
        detail = tr("尚未读取工控机原点。请检查管理连接，在工控机维护启动融合时确认坐标。");
    }
    if (!m_savedOriginSnapshot.isEmpty() && !m_savedOriginSnapshot.value("consistent").toBool())
        detail += tr("\n配置需核对：%1").arg(m_savedOriginSnapshot.value("message").toString());
    const bool haveOrigin = active || savedValid;
    ui->label_8->setText(active ? (m_navigationStatusCurrent ? tr("运行经度") : tr("上次经度")) : tr("保存经度"));
    ui->label_11->setText(active ? (m_navigationStatusCurrent ? tr("运行纬度") : tr("上次纬度")) : tr("保存纬度"));
    ui->lineEdit->setText(haveOrigin ? QString::number(origin.value("longitude").toDouble(), 'f', 8) : QString());
    ui->lineEdit_8->setText(haveOrigin ? QString::number(origin.value("latitude").toDouble(), 'f', 8) : QString());
    ui->lineEdit->setToolTip(detail);
    ui->lineEdit_8->setToolTip(detail);
}
// 检查控制器是否已经重连成功
void Home::checkConnectionRestored()
{
    if (!m_statusPresenter || !m_statusPresenter->takeConnectionRestored()) {
        return;
    }
    qDebug() << "Connection restored";
    restartCheckTimer->stop();  // 停止重连轮询
}
void Home::stopMotionForSafety(const QString &reason)
{
    m_manualInputState.clearMotionInputs();

    if (m_motionArbiter) {
        m_motionArbiter->stopAll(reason, reason == QStringLiteral("home_stop_button"));
    }
}

#include "home.h"
#include "statusclient.h"
#include "chassisclient.h"
#include "cameracontrolclient.h"
#include "gimbalcontrolclient.h"
#include "abstractvideosource.h"
#include "mjpegvideosource.h"
#include "oakcameravideosource.h"
#include "routefollower.h"
#include "motioncommandarbiter.h"
#include "homecontrolcoordinator.h"
#include "home_status_presenter.h"
#include "homevideopresenter.h"
#include "statusprotocol.h"
#include "configmanager.h"
#include "loggingmanager.h"
#include "ui_mainwindow.h"         
#include "imageswitch.h"         
#include <QDir>              // 读取录像目录时需要主目录定位
#include <QFileDialog>       // 弹出保存路径选择对话框
#include <QFile>
#include <QDateTime>
#include <QJsonArray>        // 构造批量寄存器读取的请求体
#include <QJsonDocument>     // 将 JSON 请求/响应序列化与解析
#include <QJsonObject>       // 处理单条键值信息
#include <QMessageBox>       // 输入校验提示与结果反馈
#include <QLineEdit>         // 对经纬度输入框执行焦点与选中操作
#include <QComboBox>       // 模式选择控件
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
#include <QUrlQuery>
#include <QKeyEvent>         // 处理 W/A/S/D 键盘事件
#include <QDebug>            // 调试输出
#include <QProgressDialog>   // 展示控制器重启的倒计时
#include <QVector>
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
    , m_routeFollower(new RouteFollower(this))
    , m_motionArbiter(new MotionCommandArbiter(this))
    , rebootRemainingSeconds(0)
{
    initialize();
    m_videoPresenter = std::make_unique<HomeVideoPresenter>(ui ? ui->videoDisplay : nullptr);
    initializeMotionControl();
    ConfigManager &config = ConfigManager::instance();

    if (m_routeFollower) {
        connect(m_routeFollower, &RouteFollower::velocityCommand, this, [this](double linear, double angular) {
            if (m_motionArbiter) {
                m_motionArbiter->setRouteCommand(linear, angular);
            }
        });
        connect(m_routeFollower, &RouteFollower::segmentCompleted, this, &Home::routeSegmentCompleted);
    }

    m_statusPresenter = std::make_unique<HomeStatusPresenter>(
        ui,
        m_routeFollower,
        [this](const QString &text) { logMessage(text); },
        [this](double x, double y, double theta) { emit vehiclePoseUpdated(x, y, theta); });

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
    cancelRouteExecution();
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
    ui->battery->setValue(10);
    setupImageSwitches();
    restartCheckTimer->setInterval(500);
    connect(restartCheckTimer, &QTimer::timeout, this, &Home::checkConnectionRestored);
    connect(ui->recordButton, &QPushButton::clicked, this, &Home::record);
    connect(ui->captureButton, &QPushButton::clicked, this, &Home::photo);
    connect(ui->pushButton_restart, &QPushButton::clicked, this, &Home::restartControl);
    connect(ui->pushButton_9, &QPushButton::clicked, this, &Home::submitOriginCommand);
    connect(ui->pushButton, &QPushButton::clicked, this, &Home::submitModeCommand);
    connect(ui->savePathButton, &QPushButton::clicked, this, &Home::handleSavePathButtonClicked);
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
    applyRouteFollowerConfig();
    applyStatusClientConfig();
    applyChassisClientConfig();
    applyVideoConfig();
    applyCameraControlConfig();
    applyGimbalControlConfig();
    logMessage(tr("运行配置已应用"));
}

void Home::applyRouteFollowerConfig()
{
    const auto &ctrl = ConfigManager::instance().control();
    if (m_routeFollower) {
        m_routeFollower->setControlParams(HomeControlCoordinator::routeFollowerParamsFromConfig(ctrl));
        m_routeFollower->setUpdateIntervalMs(ctrl.routeFollowerUpdateIntervalMs);
    }
    m_manualMotionRepeatIntervalMs = HomeControlCoordinator::boundedManualHeartbeatMs(ctrl);
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
    const auto &videoCfg = ConfigManager::instance().video();
    const bool wasActive = m_videoSource && m_videoSource->isActive();
    const bool wantsOakBackend = videoCfg.backend == QStringLiteral("oak_depthai");
    const bool hasOakBackend = dynamic_cast<OakCameraVideoSource *>(m_videoSource) != nullptr;

    if (m_videoSource && wantsOakBackend != hasOakBackend) {
        m_videoSource->stop();
        m_videoSource->deleteLater();
        m_videoSource = nullptr;
    }

    initializeVideoDisplay();
    if (m_videoSource && (wasActive || videoCfg.autoStart) && m_videoSource->isConfigured()) {
        m_videoSource->start();
    }
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

    if (!m_videoSource) {
        if (videoCfg.backend == QStringLiteral("oak_depthai")) {
            m_videoSource = new OakCameraVideoSource(videoCfg, this);
        } else {
            m_videoSource = new MjpegVideoSource(this);
        }
        connect(m_videoSource, &AbstractVideoSource::frameReceived, this, &Home::handleVideoFrameReceived);
        connect(m_videoSource, &AbstractVideoSource::stateChanged, this, [this](AbstractVideoSource::State state, const QString &message) {
            if (!message.isEmpty()) {
                logMessage(message);
                if (state != AbstractVideoSource::State::Streaming) {
                    updateVideoPlaceholder(message);
                }
            }
        });
    }

    if (!m_videoSource) {
        updateVideoPlaceholder(tr("视频模块未初始化"));
        return;
    }

    m_videoSource->setStreamUrlTemplate(videoCfg.streamUrl);
    m_videoSource->setReconnectIntervalMs(videoCfg.reconnectIntervalMs);
    m_videoSource->setAutoReconnect(true);
    if (auto *mjpegSource = qobject_cast<MjpegVideoSource *>(m_videoSource)) {
        mjpegSource->setMaxDisplayFps(videoCfg.previewFps);
    }

    m_activeVideoTopic.clear();
    const bool isMjpegBackend = videoCfg.backend == QStringLiteral("mjpeg_http");
    const QUrl url(videoCfg.streamUrl.trimmed());
    populateVideoStreamSelector();
    QComboBox *topicCombo = ui->video_topic_name;
    if (topicCombo && topicCombo->count() > 1) {
        const bool selectionApplied = applyConfiguredVideoStreamSelection();
        if (!selectionApplied) {
            updateVideoPlaceholder(tr("请选择视频流以开启预览"));
        }
    } else {
        if (isMjpegBackend) {
            m_videoSource->setStreamUrl(url);
            updateVideoPlaceholder(url.isValid() ? tr("等待视频流...") : tr("未配置视频流 URL"));
        } else {
            updateVideoPlaceholder(tr("等待本地相机..."));
        }
        if (videoCfg.autoStart && (m_videoSource->isConfigured() || url.isValid())) {
            m_videoSource->start();
        }
    }
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
    if (!ui || !ui->video_topic_name) {
        return;
    }

    QComboBox *topicCombo = ui->video_topic_name;
    m_videoStreamOptions.clear();

    const auto &videoCfg = ConfigManager::instance().video();
    const bool useConfiguredStreamOptions = !videoCfg.streamOptions.isEmpty();
    const bool useTopicTemplateSelection = m_videoSource && m_videoSource->supportsTopics();

    topicCombo->blockSignals(true);

    if (useConfiguredStreamOptions || !useTopicTemplateSelection) {
        topicCombo->clear();
        topicCombo->addItem(tr("关闭视频"), QString());
    } else {
        if (topicCombo->count() == 0) {
            topicCombo->addItem(tr("关闭视频"), QString());
        } else {
            topicCombo->setItemData(0, QString());
        }
    }

    if (useConfiguredStreamOptions) {
        for (const auto &option : videoCfg.streamOptions) {
            topicCombo->addItem(option.name, option.url);
            m_videoStreamOptions.insert(option.name, option.url);
        }
    } else if (useTopicTemplateSelection) {
        for (int i = 1; i < topicCombo->count(); ++i) {
            const QString topicName = topicCombo->itemText(i).trimmed();
            if (!topicName.isEmpty()) {
                m_videoStreamOptions.insert(topicName, QString());
            }
        }
    } else if (!videoCfg.streamUrl.trimmed().isEmpty()) {
        topicCombo->addItem(tr("默认视频流"), videoCfg.streamUrl.trimmed());
        m_videoStreamOptions.insert(tr("默认视频流"), videoCfg.streamUrl.trimmed());
    }

    topicCombo->setEnabled(topicCombo->count() > 1);
    topicCombo->setCurrentIndex(0);
    topicCombo->blockSignals(false);
    topicCombo->disconnect(this);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(topicCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &Home::handleVideoTopicChanged);
#else
    connect(topicCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Home::handleVideoTopicChanged);
#endif
}

bool Home::applyConfiguredVideoStreamSelection()
{
    if (!ui || !ui->video_topic_name) {
        return false;
    }
    QComboBox *topicCombo = ui->video_topic_name;
    const QString configuredUrl = ConfigManager::instance().video().streamUrl.trimmed();
    if (configuredUrl.isEmpty() || topicCombo->count() <= 1) {
        return false;
    }

    const bool hasExplicitStreamOptions = !ConfigManager::instance().video().streamOptions.isEmpty();

    if (!hasExplicitStreamOptions && !m_videoSource->supportsTopics()) {
        topicCombo->setCurrentIndex(1);
        return true;
    }

    for (int i = 1; i < topicCombo->count(); ++i) {
        const QString itemUrl = topicCombo->itemData(i).toString().trimmed();
        if (itemUrl == configuredUrl) {
            topicCombo->setCurrentIndex(i);
            return true;
        }
    }

    if (!hasExplicitStreamOptions && m_videoSource && m_videoSource->supportsTopics()) {
        const QUrl configuredStreamUrl(configuredUrl);
        const QString configuredTopic = QUrlQuery(configuredStreamUrl).queryItemValue(QStringLiteral("topic")).trimmed();
        if (!configuredTopic.isEmpty()) {
            for (int i = 1; i < topicCombo->count(); ++i) {
                if (topicCombo->itemText(i).trimmed() == configuredTopic) {
                    topicCombo->setCurrentIndex(i);
                    return true;
                }
            }
        }
    }

    return false;
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
    return m_cameraControlClient && m_cameraControlClient->isConfigured();
}

void Home::updateVideoPlaceholder(const QString &message)
{
    if (m_videoPresenter) {
        m_videoPresenter->showPlaceholder(message);
    }
}

void Home::displayVideoFrame(const QImage &image)
{
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

    if (!m_remoteCameraServiceAvailable) {
        updateVideoPlaceholder(tr("工控机相机服务不可用，等待恢复..."));
        return;
    }
    if (!m_remoteCameraConnected) {
        updateVideoPlaceholder(tr("工控机相机服务在线，但相机未连接"));
        return;
    }
    if (!m_videoPresenter || m_videoPresenter->lastFrame().isNull()) {
        updateVideoPlaceholder(tr("相机已连接，等待预览画面..."));
    }
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
    const QString normalizedText = text.trimmed();
    if (normalizedText.isEmpty()) {
        return;
    }
    if (m_lastHomeLogMessage == normalizedText) {
        return;
    }
    m_lastHomeLogMessage = normalizedText;
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
    if (m_routeFollower) {
        m_routeFollower->cancel();
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

void Home::handleVideoTopicChanged(int index)
{
    if (!ui || !ui->video_topic_name) {
        return;
    }
    if (!m_videoSource) {
        return;
    }

    if (index <= 0) {
        m_activeVideoTopic.clear();
        m_videoSource->stop();
        updateVideoPlaceholder(tr("视频流已关闭"));
        return;
    }

    const QString topic = ui->video_topic_name->itemText(index).trimmed();
    if (topic.isEmpty()) {
        return;
    }

    QString nextUrlStr = ui->video_topic_name->itemData(index).toString().trimmed();
    if (nextUrlStr.isEmpty()) {
        nextUrlStr = m_videoSource->buildUrlForTopic(topic);
    }
    if (nextUrlStr.isEmpty()) {
        m_videoSource->stop();
        updateVideoPlaceholder(tr("视频流 URL 配置无效"));
        return;
    }

    m_activeVideoTopic = topic;
    const QUrl nextUrl(nextUrlStr);
    if (!nextUrl.isValid()) {
        m_videoSource->stop();
        updateVideoPlaceholder(tr("视频流 URL 配置无效"));
        return;
    }

    if (m_videoSource->streamUrl() == nextUrl && m_videoSource->isActive()) {
        return;
    }

    m_videoSource->stop();
    m_videoSource->setStreamUrl(nextUrl);
    m_videoSource->start();
    updateVideoPlaceholder(tr("正在切换视频流..."));
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
        HomeControlCoordinator::manualCommandConfig(ui && ui->doubleSpinBox ? ui->doubleSpinBox->value() : 0.0,
                                                    ui && ui->doubleSpinBox_2 ? ui->doubleSpinBox_2->value() : 0.0,
                                                    isManualControlEnabledForButtons(),
                                                    isManualControlEnabledForKeys()));
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

    if (m_saveDirectory.isEmpty()) {
        if (!ensureSaveDirectorySelected(parentWidget)) {
            logMessage(tr("已取消选择保存目录，未开始录像。"));
            return;
        }
    }

    QString path;
    if (!m_videoSource->startRecording(m_saveDirectory, &path)) {
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

// 确保选择了保存目录，返回是否成功选择
bool Home::ensureSaveDirectorySelected(QWidget *parentForDialog)
{
    const QString dir = QFileDialog::getExistingDirectory(parentForDialog,
                                                          tr("选择保存地址"),
                                                          QDir::homePath(),
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (dir.isEmpty()) {
        return false;
    }
    m_saveDirectory = dir;
    return true;
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
void Home::submitModeCommand()
{
    if (!ui) {
        return;
    }

    const QString writeUrlStr = ConfigManager::instance().network().writeInsUrl.trimmed();
    const QUrl writeUrl(writeUrlStr);
    if (!writeUrl.isValid()) {
        logMessage(tr("写寄存器接口地址无效：%1").arg(writeUrlStr));
        return;
    }

    QWidget *targetWidget = ui->comboBox_Mode;
    QString address = "3c", type = "int8";
    int len = 1;
    if (m_chassisClient) {
        m_chassisClient->sendStopLocation();
    }
    QJsonArray writeReq;
    QJsonObject dataObj;
    dataObj["address"] = address;
    dataObj["type"] = type;
    dataObj["len"] = len;
    auto combo = qobject_cast<QComboBox *>(targetWidget);
    dataObj["data"] = QJsonArray{combo->currentIndex()};
    writeReq.append(dataObj);
    QNetworkRequest req(writeUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const QString authToken = ConfigManager::instance().network().authToken.trimmed();
    if (!authToken.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + authToken.toUtf8());
    }
    auto manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->post(req, QJsonDocument(writeReq).toJson());
    connect(reply, &QNetworkReply::finished, this, [reply, manager]() {
        reply->deleteLater();
        manager->deleteLater();
    });
}
void Home::handleSavePathButtonClicked()
{
    if (!ui) {  // 缺少 UI 指针时直接返回
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(ui->savePathButton,  // 以保存路径按钮为父控件弹出目录选择
                                                          tr("选择保存地址"),
                                                          QDir::homePath(),
                                                          QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);  // 仅允许选择文件夹，不解析软链接
    if (!dir.isEmpty()) {  // 用户选择了有效目录
        m_saveDirectory = dir;
        qDebug() << "Selected directory:" << dir;  // 当前实现只记录路径，后续可扩展保存逻辑
        logMessage(tr("已选择保存目录：%1").arg(dir));
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

    // If the stream/source is not configured (e.g. topic not selected), stop early.
    if (!m_videoSource->isConfigured()) {
        QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先选择/配置视频流后再录像。"));
        return;
    }

    if (!m_videoSource->isActive()) {
        m_videoSource->start();
    }

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
        if (m_videoSource && m_videoSource->isConfigured() && !m_videoSource->isActive()) {
            m_videoSource->start();
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

    if (m_videoSource->lastFrame().isNull()) {
        QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先开启视频流后再拍照。"));
        return;
    }

    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    if (m_saveDirectory.isEmpty()) {
        if (!ensureSaveDirectorySelected(parentWidget)) {
            return;
        }
    }

    QString savedPath;
    if (m_videoSource->saveSnapshot(m_saveDirectory, &savedPath)) {
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
void Home::submitOriginCommand()
{
    if (!ui) {
        return;
    }
    QWidget *messageParent = ui->centralwidget;

    const QString saveUrlStr = ConfigManager::instance().network().saveFileUrl.trimmed();
    const QUrl saveUrl(saveUrlStr);
    if (!saveUrl.isValid()) {
        QMessageBox::critical(messageParent, tr("错误"), tr("保存文件接口地址无效：%1").arg(saveUrlStr));
        return;
    }

    const QString latStr = ui->lineEdit->text().trimmed();   // 基站纬度输入
    const QString lonStr = ui->lineEdit_8->text().trimmed(); // 基站经度输入
    static const QRegularExpression coordReg(QStringLiteral("^-?\\d+(?:\\.\\d+)?$"));
    auto reportInvalidInput = [&](QLineEdit *field, const QString &message) {
        QMessageBox::warning(messageParent, tr("输入错误"), message);
        if (field) {
            field->setFocus();
            field->selectAll();
        }
    };
    if (!coordReg.match(latStr).hasMatch() || !coordReg.match(lonStr).hasMatch()) {
        reportInvalidInput(nullptr, tr("请输入有效的经纬度数值。"));
        return;
    }
    bool okLat = false;
    bool okLon = false;
    const double latitude = latStr.toDouble(&okLat);
    const double longitude = lonStr.toDouble(&okLon);
    if (!okLat || !okLon) {
        reportInvalidInput(nullptr, tr("经纬度转换失败。"));
        return;
    }
    if (latitude < -90.0 || latitude > 90.0) {
        reportInvalidInput(ui->lineEdit, tr("纬度范围应在 -90 至 90 之间。"));
        return;
    }
    if (longitude < -180.0 || longitude > 180.0) {
        reportInvalidInput(ui->lineEdit_8, tr("经度范围应在 -180 至 180 之间。"));
        return;
    }
    QJsonObject jsonData;
    jsonData["accuracy"] = "4";
    jsonData["distanceCorrection"] = "1";
    jsonData["isUSB"] = "1";
    jsonData["latitude"] = QString::number(latitude, 'f', 7);
    jsonData["longitude"] = QString::number(longitude, 'f', 7);
    jsonData["lowVelAd"] = "1";
    jsonData["maxStar"] = "25";
    jsonData["poseTheta"] = "0";
    jsonData["poseX"] = "-0.3";
    jsonData["poseY"] = "0";
    jsonData["slave_usbproductIdentifier"] = "0xea60";
    jsonData["slave_usbvendorIdentifier"] = "0x10c4";
    jsonData["thetadir"] = "1";
    jsonData["twoGPS_offsetTheta"] = "0";
    jsonData["usbproductIdentifier"] = "0x23a3";
    jsonData["usbvendorIdentifier"] = "0x067b";
    jsonData["useGlobalTheta"] = "1";
    jsonData["usetwoGPS"] = "1";
    jsonData["xdir"] = "1";
    jsonData["ydir"] = "-1";
    const QJsonDocument jsonDoc(jsonData);
    const QString jsonBody = jsonDoc.toJson(QJsonDocument::Indented);
    QByteArray postBody;
    postBody.append("path=/home/ego/User/parameter/gps.json");
    postBody.append("&body=");
    postBody.append(jsonBody.toUtf8());
    QNetworkRequest request;
    request.setUrl(saveUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    const QString authToken = ConfigManager::instance().network().authToken.trimmed();
    if (!authToken.isEmpty()) {
        request.setRawHeader("Authorization", QByteArray("Bearer ") + authToken.toUtf8());
    }
    request.setRawHeader("Connection", "keep-alive");
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36");
    request.setHeader(QNetworkRequest::ContentLengthHeader, QByteArray::number(postBody.length()));
    auto manager = new QNetworkAccessManager(this);
    QNetworkReply *reply = manager->post(request, postBody);
    connect(reply, &QNetworkReply::finished, this, [reply, manager, messageParent]() {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 200) {
            QMessageBox::information(messageParent, tr("成功"), tr("基站位置已更新。"));
        } else {
            const QByteArray response = reply->isOpen() ? reply->readAll() : QByteArray();
            const QString errorMsg = response.isEmpty()
                                         ? tr("请求失败 (HTTP 状态: %1)").arg(httpStatus)
                                         : tr("服务器返回: %1 (HTTP 状态: %2)").arg(QString::fromUtf8(response)).arg(httpStatus);
            QMessageBox::critical(messageParent, tr("错误"), errorMsg);
        }
        reply->deleteLater();
        manager->deleteLater();
    });
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
void Home::followRouteSegment(int fromPointId, int toPointId, const QList<QPointF> &polyline, double startTheta, double endTheta)
{
    Q_UNUSED(fromPointId);
    Q_UNUSED(toPointId);
    updateManualCommandConfig();
    if (m_routeFollower) {
        m_routeFollower->enqueueSegment(polyline, startTheta, endTheta);
    }
}
void Home::handleRouteQueueCompleted()
{
    if (m_motionArbiter) {
        m_motionArbiter->stopAll(QStringLiteral("route_completed"));
    }
    if (m_routeFollower) {
        m_routeFollower->cancel();
    }
    if (m_motionArbiter) {
        m_motionArbiter->clearRouteCommand();
    } else {
        sendVelocityCommand(0.0, 0.0);
    }
}
void Home::cancelRouteExecution()
{
    if (m_motionArbiter) {
        m_motionArbiter->stopAll(QStringLiteral("route_cancelled"));
    }
    if (m_routeFollower) {
        m_routeFollower->cancel();
    }
    if (m_motionArbiter) {
        m_motionArbiter->clearRouteCommand();
    } else {
        sendVelocityCommand(0.0, 0.0);
    }
}

void Home::stopMotionForSafety(const QString &reason)
{
    m_manualInputState.clearMotionInputs();

    if (m_motionArbiter) {
        m_motionArbiter->stopAll(reason);
    }
    if (m_routeFollower) {
        m_routeFollower->cancel();
    }
    if (m_motionArbiter) {
        m_motionArbiter->clearRouteCommand();
    } else {
        sendVelocityCommand(0.0, 0.0);
    }
}

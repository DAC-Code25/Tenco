#include "home.h"
#include "statusclient.h"
#include "chassisclient.h"
#include "cameracontrolclient.h"
#include "abstractvideosource.h"
#include "mjpegvideosource.h"
#include "oakcameravideosource.h"
#include "routefollower.h"
#include "home_status_presenter.h"
#include "statusprotocol.h"
#include "configmanager.h"
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

// 构造函数：缓存 UI 指针并准备网络与定时资源
Home::Home(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
    , restartCheckTimer(new QTimer(this))
    , forwardRepeatTimer(new QTimer(this))
    , backwardRepeatTimer(new QTimer(this))
    , turnLeftRepeatTimer(new QTimer(this))
    , turnRightRepeatTimer(new QTimer(this))
    , rebootCountdownTimer(new QTimer(this))
    , cameraStatusTimer(new QTimer(this))
    , rebootProgressDialog(nullptr)
    , m_statusClient(new StatusClient(this))
    , m_chassisClient(new ChassisClient(this))
    , m_routeFollower(new RouteFollower(this))
    , forwardButtonHeld(false)
    , forwardKeyHeld(false)
    , backwardButtonHeld(false)
    , backwardKeyHeld(false)
    , turnLeftButtonHeld(false)
    , turnLeftKeyHeld(false)
    , turnRightButtonHeld(false)
    , turnRightKeyHeld(false)
    , rebootRemainingSeconds(0)
{
    initialize();
    initializeMotionTimers();
    const ConfigManager &config = ConfigManager::instance();

    // Route follower configuration (from config.json).
    if (m_routeFollower) {
        const auto &ctrl = config.control();
        RouteFollower::ControlParams params;
        params.maxLinearSpeed = ctrl.maxLinearSpeed;
        params.maxAngularSpeed = ctrl.maxAngularSpeed;
        params.arrivalDistanceThreshold = ctrl.arrivalDistanceThreshold;
        params.arrivalAngleThresholdRad = qDegreesToRadians(ctrl.arrivalAngleThresholdDeg);
        params.linearGain = ctrl.linearGain;
        params.angularGain = ctrl.angularGain;
        params.headingStopThresholdRad = qDegreesToRadians(ctrl.headingStopThresholdDeg);
        params.headingSlowdownThresholdRad = qDegreesToRadians(ctrl.headingSlowdownThresholdDeg);
        params.headingSlowdownFactor = ctrl.headingSlowdownFactor;
        params.nearTargetDistanceMultiplier = ctrl.nearTargetDistanceMultiplier;
        params.nearTargetSpeedMultiplier = ctrl.nearTargetSpeedMultiplier;
        params.linearAccelerationLimit = ctrl.linearAccelerationLimit;
        params.linearDecelerationLimit = ctrl.linearDecelerationLimit;
        params.angularAccelerationLimit = ctrl.angularAccelerationLimit;
        params.angularDecelerationLimit = ctrl.angularDecelerationLimit;
        params.finalAdjustLinearSpeed = ctrl.finalAdjustLinearSpeed;
        params.finalAdjustAngularSpeed = ctrl.finalAdjustAngularSpeed;
        m_routeFollower->setControlParams(params);
        m_routeFollower->setUpdateIntervalMs(100);
        connect(m_routeFollower, &RouteFollower::velocityCommand, this, &Home::sendVelocityCommand);
        connect(m_routeFollower, &RouteFollower::segmentCompleted, this, &Home::routeSegmentCompleted);
    }

    m_statusPresenter = std::make_unique<HomeStatusPresenter>(
        ui,
        m_routeFollower,
        [this](const QString &text) { logMessage(text); },
        [this](double x, double y, double theta) { emit vehiclePoseUpdated(x, y, theta); });

    // Status polling (HTTP, worker thread).
    if (m_statusClient) {
        const QJsonArray requests = StatusProtocol::defaultReadRequests();

        const auto &netCfg = config.network();
        const QUrl statusUrl(netCfg.statusReadUrl);
        if (!statusUrl.isValid()) {
            logMessage(tr("状态轮询地址无效：%1").arg(netCfg.statusReadUrl));
        }

        connect(m_statusClient, &StatusClient::statusReceived, this, &Home::handleStatusPacket);
        connect(m_statusClient, &StatusClient::requestFailed, this, &Home::handleNetworkFailure);
        m_statusClient->configure(statusUrl, requests, netCfg.statusPollIntervalMs, netCfg.authToken);
        m_statusClient->start();
    }

    // Chassis WebSocket client.
    if (m_chassisClient) {
        const auto &netCfg = config.network();
        const QUrl wsUrl(netCfg.websocketUrl.trimmed());
        m_chassisClient->setUrl(wsUrl);
        m_chassisClient->setAuthorizationToken(netCfg.authToken);
        connect(m_chassisClient, &ChassisClient::connected, this, &Home::handleChassisConnected);
        connect(m_chassisClient, &ChassisClient::disconnected, this, &Home::handleChassisDisconnected);
        connect(m_chassisClient, &ChassisClient::errorOccurred, this, &Home::handleChassisError);
        m_chassisClient->connectToHost();
    }

    rebootCountdownTimer->setInterval(1000);
    rebootCountdownTimer->setSingleShot(false);
    connect(rebootCountdownTimer, &QTimer::timeout, this, &Home::updateRebootProgress);
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
    initializeVideoDisplay();
    initializeCameraControl();
    initializeCameraStatusPolling();
    logMessage(tr("首页模块已初始化，等待操作…"));
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
// 为四种运动指令配置重复发送的 QTimer
void Home::initializeMotionTimers()
{
    const int interval = kMotionRepeatIntervalMs;  // 连发周期取常量，保证四个方向一致
    forwardRepeatTimer->setInterval(interval);  // 前进指令的重复发送周期
    forwardRepeatTimer->setSingleShot(false);  // 允许定时器持续触发
    connect(forwardRepeatTimer, &QTimer::timeout, this, &Home::sendForwardCommand);  // 定时重发 sendForwardCommand() 保持匀速
    backwardRepeatTimer->setInterval(interval);  // 后退同样采用统一周期
    backwardRepeatTimer->setSingleShot(false);  // 后退连发保持持续超时
    connect(backwardRepeatTimer, &QTimer::timeout, this, &Home::sendBackwardCommand);  // 定时重发后退速度命令
    turnLeftRepeatTimer->setInterval(interval);  // 左转加速度的重复周期
    turnLeftRepeatTimer->setSingleShot(false);  // 左转指令需要持续输出
    connect(turnLeftRepeatTimer, &QTimer::timeout, this, &Home::sendTurnLeftCommand);  // 定时重发左转角速度
    turnRightRepeatTimer->setInterval(interval);  // 右转共享同一重复周期
    turnRightRepeatTimer->setSingleShot(false);  // 右转同样持续触发
    connect(turnRightRepeatTimer, &QTimer::timeout, this, &Home::sendTurnRightCommand);  // 定时重发右转角速度
}

void Home::initializeVideoDisplay()
{
    if (!isVideoDisplayReady()) {
        return;
    }
    QLabel *videoLabel = ui->videoDisplay;
    videoLabel->setAlignment(Qt::AlignCenter);

    const auto &videoCfg = ConfigManager::instance().video();
    m_videoScaleContents = videoCfg.scaleContents;
    videoLabel->setScaledContents(m_videoScaleContents);

    if (!m_videoSource) {
        if (videoCfg.backend == QStringLiteral("oak_depthai")) {
            m_videoSource = new OakCameraVideoSource(videoCfg, this);
        } else {
            m_videoSource = new MjpegVideoSource(this);
        }
    }

    if (!m_videoSource) {
        updateVideoPlaceholder(tr("视频模块未初始化"));
        return;
    }

    m_videoSource->setStreamUrlTemplate(videoCfg.streamUrl);
    m_videoSource->setReconnectIntervalMs(videoCfg.reconnectIntervalMs);
    m_videoSource->setAutoReconnect(true);

    connect(m_videoSource, &AbstractVideoSource::frameReceived, this, &Home::handleVideoFrameReceived);
    connect(m_videoSource, &AbstractVideoSource::stateChanged, this, [this](AbstractVideoSource::State state, const QString &message) {
        if (!message.isEmpty()) {
            logMessage(message);
            if (state != AbstractVideoSource::State::Streaming) {
                updateVideoPlaceholder(message);
            }
        }
    });

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
    connect(cameraStatusTimer, &QTimer::timeout, this, &Home::requestRemoteCameraStatus);
    if (usesRemoteCameraControl()) {
        cameraStatusTimer->start();
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
        return;
    }

    if (!m_cameraControlClient) {
        m_cameraControlClient = new CameraControlClient(this);
    }

    m_cameraControlClient->setBaseUrl(controlUrl);
    m_cameraControlClient->setAuthorizationToken(cfg.network().authToken);

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

    updateRecordButtonText(m_cameraControlClient->isRecording());
    requestRemoteCameraStatus();
}

bool Home::isVideoDisplayReady() const
{
    return ui && ui->videoDisplay;
}

bool Home::usesRemoteCameraControl() const
{
    return m_cameraControlClient && m_cameraControlClient->isConfigured();
}

void Home::updateVideoPlaceholder(const QString &message)
{
    if (!isVideoDisplayReady()) {
        return;
    }
    QLabel *videoLabel = ui->videoDisplay;
    videoLabel->setScaledContents(m_videoScaleContents);
    videoLabel->setPixmap(QPixmap());
    videoLabel->setText(message);
}

void Home::displayVideoFrame(const QImage &image)
{
    if (!isVideoDisplayReady()) {
        return;
    }
    QLabel *videoLabel = ui->videoDisplay;
    QImage frame = image;
    if (frame.format() != QImage::Format_RGB32 && frame.format() != QImage::Format_ARGB32) {
        frame = frame.convertToFormat(QImage::Format_RGB32);
    }
    QPixmap pixmap = QPixmap::fromImage(frame);
    if (pixmap.isNull()) {
        return;
    }
    if (!m_videoScaleContents && !videoLabel->size().isEmpty()) {
        pixmap = pixmap.scaled(videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    videoLabel->setText(QString());
    videoLabel->setPixmap(pixmap);
    m_lastVideoFrame = frame;
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
    if (!usesRemoteCameraControl() || !ui) {
        return;
    }

    if (ui->captureButton) {
        ui->captureButton->setEnabled(m_remoteCameraServiceAvailable && m_remoteCameraConnected);
    }
    if (ui->recordButton) {
        ui->recordButton->setEnabled(m_remoteCameraServiceAvailable && m_remoteCameraConnected);
    }

    if (!m_remoteCameraServiceAvailable) {
        updateVideoPlaceholder(tr("工控机相机服务不可用，等待恢复..."));
        return;
    }
    if (!m_remoteCameraConnected) {
        updateVideoPlaceholder(tr("工控机相机服务在线，但相机未连接"));
        return;
    }
    if (m_lastVideoFrame.isNull()) {
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
                             .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                             .arg(normalizedText);
    ui->plainTextEdit->appendPlainText(line);
}

void Home::handleChassisConnected()
{
    logMessage(tr("WebSocket 已连接"));
}

void Home::handleChassisDisconnected()
{
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
// 根据按钮/键盘状态开启或关闭前进连发定时器
void Home::updateForwardTimer()
{
    if (shouldSendForward()) {  // 只要任一输入源要求前进，就保持定时器运行
        if (!forwardRepeatTimer->isActive()) {  // 避免重复启动，节省资源
            forwardRepeatTimer->start();  // 开始周期性发送前进命令
        }
    } else {  // 没有前进输入时立即停止连发
        forwardRepeatTimer->stop();  // 确保速度指令不会残留
    }
}
// 控制后退指令的连发定时器
void Home::updateBackwardTimer()
{
    if (shouldSendBackward()) {  // 后退按钮或键盘触发时保持定时器运行
        if (!backwardRepeatTimer->isActive()) {  // 仅在停止状态下重新启动
            backwardRepeatTimer->start();  // 定时推送负向速度
        }
    } else {  // 无输入时停止后退命令
        backwardRepeatTimer->stop();  // 避免继续发送后退速度
    }
}
// 控制左转角速度的重复发送逻辑
void Home::updateTurnLeftTimer()
{
    if (shouldSendTurnLeft()) {  // 左转按键或键盘激活时保持定时器
        if (!turnLeftRepeatTimer->isActive()) {  // 避免重复 start()
            turnLeftRepeatTimer->start();  // 周期性发送左转指令
        }
    } else {  // 无需左转时关闭定时器
        turnLeftRepeatTimer->stop();  // 停止推送左转角速度
    }
}
// 控制右转角速度的重复发送逻辑
void Home::updateTurnRightTimer()
{
    if (shouldSendTurnRight()) {  // 右转输入存在时保持定时器运行
        if (!turnRightRepeatTimer->isActive()) {  // 仅在未运行状态下启动
            turnRightRepeatTimer->start();  // 周期性发送右转指令
        }
    } else {  // 无右转需求时停止
        turnRightRepeatTimer->stop();  // 结束右转角速度输出
    }
}
// 判断是否需要继续推送前进速度
bool Home::shouldSendForward() const
{
    return (forwardButtonHeld && isManualControlEnabledForButtons()) ||  // UI 按钮被按住且权限开启
           (forwardKeyHeld && isManualControlEnabledForKeys());  // 或者键盘 W 键被按住且键盘权限已开
}
// 判断是否要发送后退速度
bool Home::shouldSendBackward() const
{
    return (backwardButtonHeld && isManualControlEnabledForButtons()) ||  // UI 后退按钮按住
           (backwardKeyHeld && isManualControlEnabledForKeys());  // 或键盘 S 键按住
}
// 判断是否要发送左转角速度
bool Home::shouldSendTurnLeft() const
{
    return (turnLeftButtonHeld && isManualControlEnabledForButtons()) ||  // 左转按钮被按住
           (turnLeftKeyHeld && isManualControlEnabledForKeys());  // 或键盘 A 键按住
}
// 判断是否要发送右转角速度
bool Home::shouldSendTurnRight() const
{
    return (turnRightButtonHeld && isManualControlEnabledForButtons()) ||  // 右转按钮按住
           (turnRightKeyHeld && isManualControlEnabledForKeys());  // 或键盘 D 键按住
}
// 通过 WebSocket 向底盘发送速度指令 JSON
void Home::sendVelocityCommand(double xVel, double thetaVel)
{
    if (!m_chassisClient) {
        return;
    }
    m_chassisClient->sendVelocityCommand(xVel, thetaVel);
}
// 发送前进线速度，来源于 UI 数值框
void Home::sendForwardCommand()
{
    if (!shouldSendForward()) {  // 再次确认输入源仍需要前进
        return;  // 没有持续需求立即退出
    }
    const double speed = ui->doubleSpinBox->value();  // 使用界面上的速度调节值
    sendVelocityCommand(speed, 0.0);  // x 轴正向速度，角速度为 0
}
// 发送后退速度，复用前进速度的绝对值
void Home::sendBackwardCommand()
{
    if (!shouldSendBackward()) {  // 确认仍需后退
        return;  // 当前无法执行重启操作
    }
    const double speed = ui->doubleSpinBox->value();  // 与前进共用一套速度设置
    sendVelocityCommand(-speed, 0.0);  // 线速度取相反数表示后退
}
// 发送左转角速度
void Home::sendTurnLeftCommand()
{
    if (!shouldSendTurnLeft()) {  // 确保输入仍保持
        return;  // 无需继续刷新
    }
    const double angular = ui->doubleSpinBox_2->value();  // 角速度取自 UI 旋转速度设置
    sendVelocityCommand(0.0, angular);  // 线速度为0，仅发送正向角速度
}
// 发送右转角速度
void Home::sendTurnRightCommand()
{
    if (!shouldSendTurnRight()) {  // 输入已释放则直接返回
        return;  // 保持轮询等待
    }
    const double angular = ui->doubleSpinBox_2->value();  // 复用角速度调节值
    sendVelocityCommand(0.0, -angular);  // 角速度取负表示右转
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
    // 每次切换都要重新评估所有方向的连发状态
    updateForwardTimer();  // 启动或维持前进定时器
    updateBackwardTimer();  // 启动后退连发
    updateTurnLeftTimer();  // 启动左转连发
    updateTurnRightTimer();  // 启动右转连发
    // 如果有按键保持按下，立即补发一次指令
    if (shouldSendForward()) {
        sendForwardCommand();  // 补发前进速度避免停顿
    }
    // 后退方向同理
    if (shouldSendBackward()) {
        sendBackwardCommand();  // 补发后退速度
    }
    // 左转方向同理
    if (shouldSendTurnLeft()) {
        sendTurnLeftCommand();  // 补发左转角速度
    }
    // 右转方向同理
    if (shouldSendTurnRight()) {
        sendTurnRightCommand();  // 补发右转角速度
    }
}
// 前进按钮按下：立即发送一次指令并启动连发
void Home::handleForwardButtonPressed()
{
    forwardButtonHeld = true;  // 标记按钮处于按下状态
    sendForwardCommand();  // 立即发送一次前进命令，响应用户操作
    updateForwardTimer();  // 依据当前状态停止前进定时器
    logMessage(tr("前进开始（按钮）"));
}
// 前进按钮抬起：停止连发
void Home::handleForwardButtonReleased()
{
    forwardButtonHeld = false;  // 清除按钮按下标记
    // 更新所有定时器，确保立即停止输出
    updateForwardTimer();
    logMessage(tr("前进停止（按钮）"));
}
// 后退按钮按下：立即发送一次后退命令
void Home::handleBackwardButtonPressed()
{
    backwardButtonHeld = true;  // 记录按钮按下
    sendBackwardCommand();  // 立刻推送后退速度
    updateBackwardTimer();  // 停止后退定时器
    logMessage(tr("后退开始（按钮）"));
}
// 后退按钮抬起：停止后退连发
void Home::handleBackwardButtonReleased()
{
    backwardButtonHeld = false;  // 清除按钮状态
    updateBackwardTimer();
    logMessage(tr("后退停止（按钮）"));
}
// 左转按钮按下：立即推送左转角速度
void Home::handleTurnLeftButtonPressed()
{
    turnLeftButtonHeld = true;  // 记录按钮按下
    sendTurnLeftCommand();  // 立即发送左转角速度
    updateTurnLeftTimer();  // 停止左转定时器
    logMessage(tr("左转开始（按钮）"));
}
// 左转按钮抬起：停止左转连发
void Home::handleTurnLeftButtonReleased()
{
    turnLeftButtonHeld = false;  // 清除按钮状态
    updateTurnLeftTimer();
    logMessage(tr("左转停止（按钮）"));
}
// 右转按钮按下：立即推送右转角速度
void Home::handleTurnRightButtonPressed()
{
    turnRightButtonHeld = true;  // 记录按钮按下
    sendTurnRightCommand();  // 立即发送右转角速度
    updateTurnRightTimer();  // 停止右转定时器
    logMessage(tr("右转开始（按钮）"));
}
// 右转按钮抬起：停止右转连发
void Home::handleTurnRightButtonReleased()
{
    turnRightButtonHeld = false;  // 清除按钮状态
    updateTurnRightTimer();
    logMessage(tr("右转停止（按钮）"));
}
// 急停按钮：清除所有输入并发送零速度
void Home::handleStopButtonClicked()
{
    forwardButtonHeld = backwardButtonHeld = false;  // 取消界面方向按钮的按压状态
    turnLeftButtonHeld = turnRightButtonHeld = false;  // 停止左右转按钮连发
    forwardKeyHeld = backwardKeyHeld = false;  // 清除键盘 W/S 状态
    turnLeftKeyHeld = turnRightKeyHeld = false;  // 清除键盘 A/D 状态
    updateForwardTimer();
    updateBackwardTimer();
    updateTurnLeftTimer();
    updateTurnRightTimer();
    if (isManualControlEnabledForButtons() || isManualControlEnabledForKeys()) {  // 允许手动控制时发送停驶指令
        sendVelocityCommand(0.0, 0.0);  // 确保底盘停下来
    }
    logMessage(tr("急停"));
}
// 处理 W/A/S/D 键按下，触发对应的运动指令
bool Home::handleKeyPress(int key, bool isAutoRepeat)
{
    if (isAutoRepeat) {  // 忽略长按产生的重复事件
        return false;
    }
    // 未开启键盘控制时不处理也不记录
    if (!isManualControlEnabledForKeys()) {
        return false;
    }
    switch (key) {  // 根据物理按键决定方向
    case Qt::Key_W:  // W 键 -> 前进
        forwardKeyHeld = true;  // 记录键盘输入状态
        sendForwardCommand();  // 即刻触发前进命令
        updateForwardTimer();  // 启动前进连发
        logMessage(tr("前进开始（键盘）"));
        return true;
    case Qt::Key_S:  // S 键 -> 后退
        backwardKeyHeld = true;  // 记录按键状态
        sendBackwardCommand();  // 即刻触发后退
        updateBackwardTimer();  // 启动后退连发
        logMessage(tr("后退开始（键盘）"));
        return true;
    case Qt::Key_A:  // A 键 -> 左转
        turnLeftKeyHeld = true;  // 标记左转按下
        sendTurnLeftCommand();  // 立即发送左转角速度
        updateTurnLeftTimer();  // 启动左转连发
        logMessage(tr("左转开始（键盘）"));
        return true;
    case Qt::Key_D:  // D 键 -> 右转
        turnRightKeyHeld = true;  // 标记右转按下
        sendTurnRightCommand();  // 立即发送右转角速度
        updateTurnRightTimer();  // 启动右转连发
        logMessage(tr("右转开始（键盘）"));
        return true;
    default:  // 其它按键交由基类处理
        return false;
    }
}
// 处理 W/A/S/D 键释放，关闭对应定时器
bool Home::handleKeyRelease(int key, bool isAutoRepeat)
{
    if (isAutoRepeat) {  // 忽略长按重复事件
        return false;
    }
    // 未开启键盘控制时不处理也不记录
    if (!isManualControlEnabledForKeys()) {
        return false;
    }
    switch (key) {  // 根据释放的按键更新状态
    case Qt::Key_W:  // W 键抬起
        forwardKeyHeld = false;  // 清除标记
        updateForwardTimer();  // 若没有其它输入则停止前进
        logMessage(tr("前进停止（键盘）"));
        return true;
    case Qt::Key_S:  // S 键抬起
        backwardKeyHeld = false;  // 清除后退状态
        updateBackwardTimer();  // 更新后退定时器
        logMessage(tr("后退停止（键盘）"));
        return true;
    case Qt::Key_A:  // A 键抬起
        turnLeftKeyHeld = false;  // 清除左转状态
        updateTurnLeftTimer();  // 停止左转连发
        logMessage(tr("左转停止（键盘）"));
        return true;
    case Qt::Key_D:  // D 键抬起
        turnRightKeyHeld = false;  // 清除右转状态
        updateTurnRightTimer();  // 停止右转连发
        logMessage(tr("右转停止（键盘）"));
        return true;
    default:  // 其它按键交由基类处理
        return false;
    }
}
// 录像按钮占位实现：后续可接入实际录像逻辑
void Home::record()
{
    if (!m_videoSource && !usesRemoteCameraControl()) {
        return;
    }

    if (usesRemoteCameraControl()) {
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
    if (!m_videoSource && !usesRemoteCameraControl()) {
        return;
    }

    if (usesRemoteCameraControl()) {
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
            const QByteArray response = reply->readAll();
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
    if (m_routeFollower) {
        m_routeFollower->enqueueSegment(polyline, startTheta, endTheta);
    }
}
void Home::handleRouteQueueCompleted()
{
    cancelRouteExecution();
}
void Home::cancelRouteExecution()
{
    if (m_routeFollower) {
        m_routeFollower->cancel();
    } else {
        sendVelocityCommand(0.0, 0.0);
    }
}

#include "home.h"
#include "homenetworkworker.h"
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
#include <QMap>              // 将模式值映射为可读文本
#include <QMessageBox>       // 输入校验提示与结果反馈
#include <QLineEdit>         // 对经纬度输入框执行焦点与选中操作
#include <QComboBox>       // 模式选择控件
#include <QRegularExpression>// 校验经纬度格式
#include <QNetworkRequest>   // 配置 HTTP 请求头与目标地址
#include <QNetworkAccessManager> // HTTP 管理器
#include <QPlainTextEdit>
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <QPushButton>       // 访问 UI 中的按钮控件
#include <QString>           // UI 文本与网络地址处理
#include <QStringList>       // 拼接位置坐标展示字符串
#include <QTimer>            // 周期性任务与长按控制的定时器
#include <QUrl>              // 解析 WebSocket 与 HTTP 地址
#include <QWebSocket>        // 与底盘通信的 WebSocket 客户端
#include <QWebSocketProtocol>// 选择 WebSocket 协议版本
#include <QAbstractSocket>   // 检测 socket 状态与错误
#include <QUrlQuery>
#include <QKeyEvent>         // 处理 W/A/S/D 键盘事件
#include <QDebug>            // 调试输出
#include <QProgressDialog>   // 展示控制器重启的倒计时
#include <QNetworkReply>      // 读取网络响应并判断错误状态
#include <QThread>            // 网络工作线程
#include <QMetaObject>
#include <QVector>
#include <QtMath>
#include <algorithm>
#include <cmath>
namespace {  // 限定在本文件内使用的常量
constexpr const char *kWebSocketUrl = "ws://192.168.31.7:1202";  // 底盘控制端 WebSocket 地址
}
// 构造函数：缓存 UI 指针并准备网络与定时资源
Home::Home(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
    , restartCheckTimer(new QTimer(this))
    , rebootCountdownTimer(new QTimer(this))
    , webSocket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , forwardRepeatTimer(new QTimer(this))
    , backwardRepeatTimer(new QTimer(this))
    , turnLeftRepeatTimer(new QTimer(this))
    , turnRightRepeatTimer(new QTimer(this))
    , rebootProgressDialog(nullptr)
    , networkWorker(nullptr)
    , networkThread(nullptr)
    , lastConnectionStatus(false)
    , connectionRestored(false)
    , forwardButtonHeld(false)
    , forwardKeyHeld(false)
    , backwardButtonHeld(false)
    , backwardKeyHeld(false)
    , turnLeftButtonHeld(false)
    , turnLeftKeyHeld(false)
    , turnRightButtonHeld(false)
    , turnRightKeyHeld(false)
    , startupMessagesSent(false)
    , rebootRemainingSeconds(0)
{
    initialize();
    const ConfigManager &config = ConfigManager::instance();
    const auto &control = config.control();
    m_maxLinearSpeed = control.maxLinearSpeed;
    m_maxAngularSpeed = control.maxAngularSpeed;
    m_arrivalDistanceThreshold = control.arrivalDistanceThreshold;
    m_arrivalAngleThresholdRad = qDegreesToRadians(control.arrivalAngleThresholdDeg);
    m_linearGain = control.linearGain;
    m_angularGain = control.angularGain;
    m_headingStopThresholdRad = qDegreesToRadians(control.headingStopThresholdDeg);
    m_headingSlowdownThresholdRad = qDegreesToRadians(control.headingSlowdownThresholdDeg);
    m_headingSlowdownFactor = control.headingSlowdownFactor;
    m_nearTargetDistanceMultiplier = control.nearTargetDistanceMultiplier;
    m_nearTargetSpeedMultiplier = control.nearTargetSpeedMultiplier;
    m_linearAccelerationLimit = control.linearAccelerationLimit;
    m_linearDecelerationLimit = control.linearDecelerationLimit;
    m_angularAccelerationLimit = control.angularAccelerationLimit;
    m_angularDecelerationLimit = control.angularDecelerationLimit;
    m_finalAdjustLinearSpeed = control.finalAdjustLinearSpeed;
    m_finalAdjustAngularSpeed = control.finalAdjustAngularSpeed;
    m_vehicleWheelBase = config.vehicle().wheelBaseMeters;
    m_vehicleWheelDiameter = config.vehicle().wheelDiameterMeters;
    m_vehicleGearReduction = config.vehicle().gearReduction;
    m_routeFollowerTimer = new QTimer(this);
    m_routeFollowerTimer->setTimerType(Qt::PreciseTimer);
    m_routeFollowerTimer->setInterval(100);
    connect(m_routeFollowerTimer, &QTimer::timeout, this, &Home::processRouteFollowerTick);
    m_routeFollowerDt = static_cast<double>(m_routeFollowerTimer->interval()) / 1000.0;
    initializeMotionTimers();
    initializeNetworkWorker();
    connect(webSocket, &QWebSocket::connected, this, &Home::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &Home::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::errorOccurred, this, &Home::onWebSocketError);
    connectWebSocket();
    rebootCountdownTimer->setInterval(1000);
    rebootCountdownTimer->setSingleShot(false);
    connect(rebootCountdownTimer, &QTimer::timeout, this, &Home::updateRebootProgress);
}
Home::~Home()
{
    cancelRouteExecution();
    stopVideoStream();
    if (m_videoReconnectTimer) {
        m_videoReconnectTimer->stop();
    }
    if (networkWorker) {
        QMetaObject::invokeMethod(networkWorker, "stop", Qt::QueuedConnection);
    }
    if (networkThread) {
        networkThread->quit();
        networkThread->wait();
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
    connect(ui->pushButton_9, &QPushButton::clicked, this, &Home::orignsubmmit);
    connect(ui->pushButton, &QPushButton::clicked, this, &Home::modesubmmit);
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
// 确保 WebSocket 只在需要时发起连接
void Home::initializeNetworkWorker()
{
    QJsonArray requests = {
        QJsonObject{{"address", "3f"}, {"type", "uint8"}, {"len", 1}},
        QJsonObject{{"address", "38"}, {"type", "float"}, {"len", 4}},
        QJsonObject{{"address", "3c"}, {"type", "uint8"}, {"len", 1}},
        QJsonObject{{"address", "100"}, {"type", "float"}, {"len", 12}},
        QJsonObject{{"address", "320"}, {"type", "string"}, {"len", 32}},
        QJsonObject{{"address", "20"}, {"type", "float"}, {"len", 4}},
        QJsonObject{{"address", "13"}, {"type", "uint8"}, {"len", 1}},
        QJsonObject{{"address", "14"}, {"type", "uint8"}, {"len", 1}},
        QJsonObject{{"address", "15"}, {"type", "uint8"}, {"len", 3}}
    };
    networkWorker = new HomeNetworkWorker;
    networkWorker->configure(QUrl(QStringLiteral("http://192.168.31.7:9999/table/reads")), requests, 100);
    networkThread = new QThread(this);
    networkWorker->moveToThread(networkThread);
    connect(networkThread, &QThread::finished, networkWorker, &QObject::deleteLater);
    connect(networkWorker, &HomeNetworkWorker::statusReceived, this, &Home::handleStatusPacket);
    connect(networkWorker, &HomeNetworkWorker::requestFailed, this, &Home::handleNetworkFailure);
    networkThread->start();
    QMetaObject::invokeMethod(networkWorker, "start", Qt::QueuedConnection);
}

void Home::initializeVideoDisplay()
{
    if (!isVideoDisplayReady()) {
        return;
    }
    QLabel *videoLabel = ui->videoDisplay;
    videoLabel->setAlignment(Qt::AlignCenter);
    const auto &videoCfg = ConfigManager::instance().video();
    m_videoStreamTemplate = videoCfg.streamUrl.trimmed();
    m_videoUrl.clear();
    m_activeVideoTopic.clear();
    m_videoReconnectIntervalMs = qMax(200, videoCfg.reconnectIntervalMs);
    m_videoAutoStart = videoCfg.autoStart;
    m_videoScaleContents = videoCfg.scaleContents;
    videoLabel->setScaledContents(m_videoScaleContents);
    QComboBox *topicCombo = ui->video_topic_name;
    if (topicCombo) {
        topicCombo->setCurrentIndex(0);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(topicCombo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &Home::handleVideoTopicChanged);
#else
        connect(topicCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &Home::handleVideoTopicChanged);
#endif
        updateVideoPlaceholder(m_videoStreamTemplate.isEmpty()
                                   ? tr("未配置视频流 URL")
                                   : tr("请选择视频话题以开启视频流"));
    } else {
        m_videoUrl = m_videoStreamTemplate;
        updateVideoPlaceholder(m_videoUrl.isEmpty() ? tr("未配置视频流 URL")
                                                    : tr("等待视频流..."));
        if (m_videoAutoStart && !m_videoUrl.isEmpty()) {
            startVideoStream();
        }
    }
}

bool Home::isVideoDisplayReady() const
{
    return ui && ui->videoDisplay;
}

bool Home::isVideoStreamActive() const
{
    return m_videoReply && m_videoReply->isRunning();
}

void Home::startVideoStream()
{
    if (m_videoUrl.isEmpty()) {
        updateVideoPlaceholder(tr("未配置视频流 URL"));
        return;
    }
    if (!m_videoManager) {
        m_videoManager = new QNetworkAccessManager(this);
    }
    if (m_videoReply) {
        return;
    }
    m_videoBuffer.clear();
    QNetworkRequest req{QUrl(m_videoUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("TencoVideoClient/1.0"));
    req.setRawHeader("Accept", "multipart/x-mixed-replace");
    req.setRawHeader("Connection", "keep-alive");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#endif
    m_videoReply = m_videoManager->get(req);
    connect(m_videoReply, &QNetworkReply::readyRead, this, &Home::handleVideoReadyRead);
    connect(m_videoReply, &QNetworkReply::finished, this, &Home::handleVideoStreamFinished);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_videoReply, &QNetworkReply::errorOccurred, this, &Home::handleVideoError);
#else
    connect(m_videoReply, QOverload<QNetworkReply::NetworkError>::of(&QNetworkReply::error),
            this, &Home::handleVideoError);
#endif
    updateVideoPlaceholder(tr("正在连接视频流..."));
    logMessage(tr("正在连接视频流…"));
}

void Home::stopVideoStream()
{
    if (m_videoReconnectTimer && m_videoReconnectTimer->isActive()) {
        m_videoReconnectTimer->stop();
    }
    if (m_videoReply) {
        disconnect(m_videoReply, nullptr, this, nullptr);
        if (m_videoReply->isRunning()) {
            m_videoReply->abort();
        }
        m_videoReply->deleteLater();
        m_videoReply = nullptr;
    }
    m_videoBuffer.clear();
}

void Home::handleVideoReadyRead()
{
    if (!m_videoReply) {
        return;
    }
    m_videoBuffer.append(m_videoReply->readAll());
    if (m_videoBuffer.size() > kMaxVideoBufferSize) {
        m_videoBuffer = m_videoBuffer.right(kMaxVideoBufferSize / 2);
    }
    static const QByteArray kJpegStart("\xFF\xD8", 2);
    static const QByteArray kJpegEnd("\xFF\xD9", 2);
    while (true) {
        int startIndex = m_videoBuffer.indexOf(kJpegStart);
        if (startIndex < 0) {
            m_videoBuffer = m_videoBuffer.right(kMaxVideoBufferSize / 2);
            return;
        }
        if (startIndex > 0) {
            m_videoBuffer.remove(0, startIndex);
            startIndex = 0;
        }
        const int endIndex = m_videoBuffer.indexOf(kJpegEnd, startIndex + kJpegStart.size());
        if (endIndex < 0) {
            if (m_videoBuffer.size() > kMaxVideoBufferSize) {
                m_videoBuffer = m_videoBuffer.right(kMaxVideoBufferSize / 2);
            }
            return;
        }
        const int frameSize = endIndex - startIndex + kJpegEnd.size();
        QByteArray frameData = m_videoBuffer.mid(startIndex, frameSize);
        m_videoBuffer.remove(0, startIndex + frameSize);
        QImage image;
        if (image.loadFromData(frameData, "JPG")) {
            displayVideoFrame(image);
            if (m_isRecording) {
                if (m_recordFile && m_recordFile->isOpen()) {
                    m_recordFile->write(frameData);
                } else {
                    m_recordBuffer.append(frameData);
                }
            }
        }
    }
}

void Home::handleVideoStreamFinished()
{
    stopVideoStream();
    updateVideoPlaceholder(tr("视频流已结束，等待重连..."));
    scheduleVideoReconnect();
    logMessage(tr("视频流结束，等待重连"));
}

void Home::handleVideoError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error);
    const QString errorText = m_videoReply ? m_videoReply->errorString() : QStringLiteral("unknown");
    qWarning() << "Video stream error:" << errorText;
    stopVideoStream();
    updateVideoPlaceholder(tr("视频流异常: %1").arg(errorText));
    scheduleVideoReconnect();
    logMessage(tr("视频流异常：%1").arg(errorText));
}

void Home::scheduleVideoReconnect()
{
    if (!m_videoAutoStart || m_videoUrl.isEmpty()) {
        return;
    }
    if (!m_videoReconnectTimer) {
        m_videoReconnectTimer = new QTimer(this);
        m_videoReconnectTimer->setSingleShot(true);
        connect(m_videoReconnectTimer, &QTimer::timeout, this, &Home::restartVideoStream);
    }
    if (!m_videoReconnectTimer->isActive()) {
        m_videoReconnectTimer->start(m_videoReconnectIntervalMs);
    }
}

void Home::restartVideoStream()
{
    if (!m_videoAutoStart || m_videoUrl.isEmpty()) {
        return;
    }
    if (m_videoReply) {
        return;
    }
    startVideoStream();
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

void Home::logMessage(const QString &text)
{
    if (!ui || !ui->plainTextEdit) {
        return;
    }
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString("hh:mm:ss"))
                             .arg(text);
    ui->plainTextEdit->appendPlainText(line);
}

void Home::handleVideoTopicChanged(int index)
{
    if (!ui || !ui->video_topic_name) {
        return;
    }

    if (index <= 0) {
        m_activeVideoTopic.clear();
        m_videoUrl.clear();
        stopVideoStream();
        updateVideoPlaceholder(tr("视频流已关闭"));
        return;
    }

    const QString topic = ui->video_topic_name->itemText(index).trimmed();
    if (topic.isEmpty()) {
        return;
    }

    const QString nextUrl = buildVideoUrlForTopic(topic);
    if (nextUrl.isEmpty()) {
        stopVideoStream();
        updateVideoPlaceholder(tr("视频流 URL 配置无效"));
        return;
    }

    m_activeVideoTopic = topic;
    if (m_videoUrl == nextUrl && m_videoReply) {
        return;
    }

    m_videoUrl = nextUrl;
    stopVideoStream();
    startVideoStream();
}

QString Home::buildVideoUrlForTopic(const QString &topic) const
{
    if (m_videoStreamTemplate.isEmpty()) {
        return QString();
    }

    QUrl url(m_videoStreamTemplate);
    if (!url.isValid()) {
        return QString();
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    QUrlQuery query(url);
    if (query.hasQueryItem(QStringLiteral("topic"))) {
        query.removeAllQueryItems(QStringLiteral("topic"));
    }
    if (!topic.isEmpty()) {
        query.addQueryItem(QStringLiteral("topic"), topic);
    }
    url.setQuery(query);
#else
    Q_UNUSED(topic);
#endif
    return url.toString(QUrl::FullyEncoded);
}
void Home::connectWebSocket()
{
    if (!webSocket) {  // 指针应始终存在，但依旧防御性检查
        return;  // 缺少客户端时直接退出
    }
    if (webSocket->state() == QAbstractSocket::ConnectedState ||  // 已连接或正在连接时无需重复 open
        webSocket->state() == QAbstractSocket::ConnectingState) {  // 保持当前连接流程
        return;  // 避免重复调用 open()
    }
    webSocket->open(QUrl(QString::fromUtf8(kWebSocketUrl)));  // 按既定地址发起 WebSocket 连接
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
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {  // 仅在连接成功后才发送数据
        if (webSocket && webSocket->state() == QAbstractSocket::UnconnectedState) {  // 如果断开则尝试重新连接
            connectWebSocket();  // 触发重连流程
        }
        return;  // 当前无法发送任何指令
    }
    QJsonObject packetObj{{"cmd", "region"}, {"region", "cmd_vel"}, {"index", 1}};  // packet：标识控制区域及命令类型
    QJsonObject msgObj{{"xvel", xVel}, {"yvel", 0.0}, {"thetavel", thetaVel}, {"isRemote", true}};  // msg：实际速度向量与远程控制标记
    QJsonDocument doc(QJsonObject{{"packet", packetObj}, {"msg", msgObj}});  // 封装成单个 JSON 文档
    webSocket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));  // 以紧凑格式发送文本消息
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
    m_isRecording = true;
    m_recordBuffer.clear();
    m_recordFilePath.clear();

    if (m_saveDirectory.isEmpty()) {
        // 未选择保存目录时先缓存到内存，停止时再提示保存
        QMessageBox::information(parentWidget, tr("提示"), tr("尚未选择保存路径，录像数据将暂存，停止后请选定保存位置。"));
    } else {
        QDir dir(m_saveDirectory);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
        const QString fileName = QStringLiteral("video_%1.mjpeg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
        m_recordFilePath = dir.filePath(fileName);
        m_recordFile = new QFile(m_recordFilePath, this);
        if (!m_recordFile->open(QIODevice::WriteOnly)) {
            QMessageBox::warning(parentWidget, tr("提示"), tr("无法打开文件写入，录像将暂存内存。"));
            m_recordFile->deleteLater();
            m_recordFile = nullptr;
            m_recordFilePath.clear();
        }
    }

    if (ui && ui->recordButton) {
        ui->recordButton->setText(tr("停止录像"));
    }
    logMessage(tr("开始录像"));
}

// 停止录像并保存文件
void Home::stopRecordingAndSave()
{
    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    m_isRecording = false;

    // 关闭已打开的文件
    if (m_recordFile) {
        m_recordFile->close();
        m_recordFile->deleteLater();
        m_recordFile = nullptr;
    }

    // 如未选择路径，在停止时弹出选择
    if (m_saveDirectory.isEmpty()) {
        if (!ensureSaveDirectorySelected(parentWidget)) {
            QMessageBox::warning(parentWidget, tr("提示"), tr("未保存录像，因未选择保存路径。"));
            m_recordBuffer.clear();
            m_recordFilePath.clear();
            if (ui && ui->recordButton) {
                ui->recordButton->setText(tr("开始录像"));
            }
            return;
        }
    }

    // 若此前未直接写盘，则将缓冲帧写入文件
    if (m_recordBuffer.size() > 0 || m_recordFilePath.isEmpty()) {
        QDir dir(m_saveDirectory);
        if (!dir.exists()) {
            dir.mkpath(".");
        }
        if (m_recordFilePath.isEmpty()) {
            const QString fileName = QStringLiteral("video_%1.mjpeg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
            m_recordFilePath = dir.filePath(fileName);
        }
        QFile file(m_recordFilePath);
        if (file.open(QIODevice::WriteOnly)) {
            if (!m_recordBuffer.isEmpty()) {
                file.write(m_recordBuffer);
            }
            file.close();
        } else {
            QMessageBox::warning(parentWidget, tr("提示"), tr("保存录像失败：无法写入文件"));
            m_recordBuffer.clear();
            m_recordFilePath.clear();
            if (ui && ui->recordButton) {
                ui->recordButton->setText(tr("开始录像"));
            }
            return;
        }
    }

    m_recordBuffer.clear();
    if (ui && ui->recordButton) {
        ui->recordButton->setText(tr("开始录像"));
    }
    if (!m_recordFilePath.isEmpty()) {
        QMessageBox::information(parentWidget, tr("成功"), tr("录像已保存到:\n%1").arg(m_recordFilePath));
        logMessage(tr("录像已保存到 %1").arg(m_recordFilePath));
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
// 发送控制器重启命令
void Home::sendRebootCommand()
{
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {  // 只有在 WebSocket 已连接时才允许下发
        if (webSocket && webSocket->state() == QAbstractSocket::UnconnectedState) {  // 若已断开则先尝试重连
            connectWebSocket();  // 触发重连机制
        }
        return;
    }
    QJsonObject packetObj{{"cmd", "reboot"}};  // 构造重启指令的数据包
    QJsonDocument doc(QJsonObject{{"packet", packetObj}, {"msg", QJsonObject{}}});  // 消息体为空，仅下发命令
    webSocket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));  // 推送重启指令到控制端
}
// 打印触发该信号的开关，便于调试
void Home::onWebSocketConnected()
{
    // 记录一次急停事件，方便排查
    // 当前仅输出一条调试信息
    // 预留给实际截图流程
    // 仅输出日志，等待后续补齐业务
    // 预留刷新逻辑
    // 记录一次人工重启操作，便于日志分析
    // 打印恢复日志，帮助分析断线时长
    qDebug() << "WebSocket connected";
    if (!startupMessagesSent) {
        sendStartupWebSocketMessages();
    }
}
void Home::onWebSocketDisconnected()
{
    qDebug() << "WebSocket disconnected";
    QTimer::singleShot(1000, this, &Home::connectWebSocket);
}
void Home::onWebSocketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    qDebug() << "WebSocket error:" << webSocket->errorString();
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
//发送退出定位指令
void Home::sendStopLocation()
{
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {
        if (webSocket && webSocket->state() == QAbstractSocket::UnconnectedState) {
            connectWebSocket();
        }
        return;
    }
    const QJsonObject packetObj{{"cmd", "region"}, {"region", "slam"}, {"index", 1}};
    const QJsonObject msgObj{{"talk", "stopLocation"}};
    const QJsonDocument doc(QJsonObject{{"packet", packetObj}, {"msg", msgObj}});
    webSocket->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));  // 以紧凑格式发送文本消息
}
void Home::resetRouteCommandState(){
    m_lastRouteLinearCommand = 0.0;
    m_lastRouteAngularCommand = 0.0;
}
double Home::applySlewRate(double target, double lastValue, double accelLimit, double decelLimit, double dt) const{
    if (dt <= 0.0) {
        return target;
    }
    const double delta = target - lastValue;
    if (qFuzzyIsNull(delta)) {
        return target;
    }
    const double limit = (delta >= 0.0 ? accelLimit : decelLimit) * dt;
    if (limit <= 0.0) {
        return target;
    }
    if (std::abs(delta) <= limit) {
        return target;
    }
    return lastValue + std::copysign(limit, delta);
}
void Home::sendRouteVelocity(double linear, double angular){
    const double limitedLinear = applySlewRate(linear, m_lastRouteLinearCommand, m_linearAccelerationLimit, m_linearDecelerationLimit, m_routeFollowerDt);
    const double limitedAngular = applySlewRate(angular, m_lastRouteAngularCommand, m_angularAccelerationLimit, m_angularDecelerationLimit, m_routeFollowerDt);
    m_lastRouteLinearCommand = limitedLinear;
    m_lastRouteAngularCommand = limitedAngular;
    sendVelocityCommand(limitedLinear, limitedAngular);
}
// 向 HTTP 接口请求最新的运行数据
void Home::handleStatusPacket(const QJsonObject &resp)
{
    const bool wasConnected = lastConnectionStatus;
    lastConnectionStatus = true;
    if (!wasConnected) {
        connectionRestored = true;
        checkConnectionRestored();
        logMessage(tr("状态通信已恢复"));
    }
    ui->pushButton_10->setText(tr("通信正常"));
    ui->pushButton_10->setStyleSheet("background-color: #55ff00; min-width: 80px; min-height: 50px; border: 1px solid white; border-radius: 10px; font-size: 18px; font-family: 微软雅黑;");
    const QJsonArray dataArray = resp.value("data").toArray();
    for (const QJsonValue &item : dataArray) {
        const QJsonObject data = item.toObject();
        const QString addr = data.value("address").toString();
        const QJsonArray values = data.value("value").toArray();
        const QJsonValue val = values.isEmpty() ? QJsonValue() : values.first();
        if (addr == "3f") {
            const int battery = val.toInt();
            ui->battery->setValue(battery);
            ui->lable_battray->setText(QString::number(battery) + "%");
        } else if (addr == "38") {
            ui->lineEdit_BattryVol->setText(QString::number(val.toDouble(), 'f', 2) + "V");
        } else if (addr == "3c") {
            static const QMap<int, QString> modeMap{{0, QStringLiteral("维护模式")},
                                                    {1, QStringLiteral("手动模式")},
                                                    {2, QStringLiteral("自动模式")}};
            ui->lineEdit_mode->setText(QStringLiteral("当前模式:   ") + modeMap.value(val.toInt(), QStringLiteral("未知模式")));
        } else if (addr == "100") {
            if (values.size() >= 3) {
                m_currentPoseX = values.at(0).toDouble();
                m_currentPoseY = values.at(1).toDouble();
                m_currentPoseTheta = values.at(2).toDouble();
                m_poseValid = true;
                emit vehiclePoseUpdated(m_currentPoseX, m_currentPoseY, m_currentPoseTheta);
                QStringList parts;
                for (const QJsonValue &v : values) {
                    parts << QString::number(v.toDouble(), 'f', 5);
                }
                ui->lineEdit_Position->setText(parts.join(", "));

            } else {
                m_poseValid = false;
            }
        } else if (addr == "320") {
            ui->lineEdit_mapname->setText(QStringLiteral("当前地图:   ") + val.toString());
        } else if (addr == "20") {
            if (!values.isEmpty()) {
                const double velocity = values.first().toDouble();
                if (velocity >= -2.0 && velocity <= 2.0) {
                    ui->lcdNumber->setDigitCount(5);
                    ui->lcdNumber->display(QString::number(qAbs(velocity), 'f', 2));
                }
            }
        } else if (addr == "13") {
            const int temp = val.toInt();
            ui->lineEdit_BattryTemp->setText(QString::number(temp) + "℃");
        } else if (addr == "14") {
            QString bettray_state = QStringLiteral("电已充满");
            if (val.toInt() == 1)
                bettray_state = QStringLiteral("充电中");
            if (val.toInt() == 2)
                bettray_state = QStringLiteral("放电中");
            ui->lineEdit_BattryState->setText(bettray_state);
        } else if (addr == "15") {
            const int hour = values.size() > 0 ? values.at(0).toInt() : 0;
            const int minute = values.size() > 1 ? values.at(1).toInt() : 0;
            const int second = values.size() > 2 ? values.at(2).toInt() : 0;
            ui->lineEdit_RunTime->setText(QStringLiteral("%1小时%2分钟%3秒").arg(hour).arg(minute).arg(second));
        }
    }
}
void Home::handleNetworkFailure(int httpStatus, const QString &errorString, const QByteArray &responseBody)
{
    Q_UNUSED(httpStatus);
    Q_UNUSED(errorString);
    Q_UNUSED(responseBody);
    const bool wasConnected = lastConnectionStatus;
    ui->pushButton_10->setText(tr("通信故障"));
    ui->pushButton_10->setStyleSheet("background-color: #ff0000; min-width: 80px; min-height: 50px; border: 1px solid white; border-radius: 10px; font-size: 18px; font-family: 微软雅黑;");
    lastConnectionStatus = false;
    connectionRestored = false;
    logMessage(tr("通信故障，正在尝试重连"));
    if (wasConnected) {
        restartCheckTimer->start();
    }
}
void Home::modesubmmit()
{
    if (!ui) {
        return;
    }
    QWidget *targetWidget = ui->comboBox_Mode;
    QString address = "3c", type = "int8";
    int len = 1;
    sendStopLocation();
    QJsonArray writeReq;
    QJsonObject dataObj;
    dataObj["address"] = address;
    dataObj["type"] = type;
    dataObj["len"] = len;
    auto combo = qobject_cast<QComboBox *>(targetWidget);
    dataObj["data"] = QJsonArray{combo->currentIndex()};
    writeReq.append(dataObj);
    QNetworkRequest req(QUrl("http://192.168.31.7:9999/table/writeIns"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
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
    // 确保视频流已打开，否则提示后尝试自动开启
    if (!isVideoStreamActive()) {
        startVideoStream();
        if (!isVideoStreamActive()) {
            QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先开启视频流后再录像。"));
            return;
        }
    }

    if (!m_isRecording) {
        startRecording();
    } else {
        stopRecordingAndSave();
    }
}
// 截图按钮占位实现
void Home::photo()
{
    if (!isVideoStreamActive() && m_lastVideoFrame.isNull()) {
        QMessageBox::warning(ui ? ui->centralwidget : nullptr, tr("提示"), tr("请先开启视频流后再拍照。"));
        return;
    }

    QWidget *parentWidget = ui ? ui->centralwidget : nullptr;
    if (m_saveDirectory.isEmpty()) {
        if (!ensureSaveDirectorySelected(parentWidget)) {
            return;
        }
    }

    QDir dir(m_saveDirectory);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    const QString fileName = QStringLiteral("photo_%1.jpg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString fullPath = dir.filePath(fileName);
    if (m_lastVideoFrame.save(fullPath, "JPG", 90)) {
        QMessageBox::information(parentWidget, tr("成功"), tr("已保存到:\n%1").arg(fullPath));
        logMessage(tr("已拍照并保存到 %1").arg(fullPath));
    } else {
        QMessageBox::warning(parentWidget, tr("失败"), tr("保存图片失败"));
        logMessage(tr("拍照保存失败"));
    }
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
    qDebug() << "重启控制命令";
    sendRebootCommand();
    connectionRestored = false;
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
void Home::orignsubmmit()
{
    if (!ui) {
        return;
    }
    QWidget *messageParent = ui->centralwidget;
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
    request.setUrl(QUrl(QStringLiteral("http://192.168.31.7:9999/saveFile")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
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
    if (!connectionRestored) {  // 若尚未恢复则无需处理
        return;
    }
    qDebug() << "连接已恢复";
    restartCheckTimer->stop();  // 停止重连轮询
    connectionRestored = false;
}
void Home::followRouteSegment(int fromPointId, int toPointId, const QList<QPointF> &polyline, double startTheta, double endTheta)
{
    Q_UNUSED(fromPointId);
    Q_UNUSED(toPointId);
    RouteSegmentExecution segment;
    segment.polyline = polyline;
    if (segment.polyline.isEmpty()) {
        segment.polyline.append(QPointF(m_currentPoseX, m_currentPoseY));
    }
    segment.startTheta = startTheta;
    segment.endTheta = endTheta;
    segment.targetIndex = 0;
    m_pendingRouteSegments.enqueue(segment);
    if (!m_routeFollowerActive) {
        startNextSegment();
    }
}
void Home::handleRouteQueueCompleted()
{
    cancelRouteExecution();
}
void Home::cancelRouteExecution()
{
    m_pendingRouteSegments.clear();
    m_routeFollowerActive = false;
    if (m_routeFollowerTimer && m_routeFollowerTimer->isActive()) {
        m_routeFollowerTimer->stop();
    }
    resetRouteCommandState();
    sendVelocityCommand(0.0, 0.0);
}
void Home::processRouteFollowerTick()
{
    if (!m_routeFollowerActive) {
        if (!m_pendingRouteSegments.isEmpty()) {
            startNextSegment();
        }
        return;
    }
    if (!m_poseValid) {
        sendRouteVelocity(0.0, 0.0);
        return;
    }
    RouteSegmentExecution &segment = m_activeRouteSegment;
    if (segment.polyline.isEmpty()) {
        finalizeCurrentSegment(true);
        return;
    }
    if (segment.targetIndex >= segment.polyline.size()) {
        finalizeCurrentSegment(true);
        return;
    }
    if (segment.targetIndex == 0) {
        const double startHeadingError = normalizeAngle(segment.startTheta - m_currentPoseTheta);
        if (std::abs(startHeadingError) > m_arrivalAngleThresholdRad) {
            const double angularTarget = std::clamp(m_angularGain * startHeadingError, -m_maxAngularSpeed, m_maxAngularSpeed);
            const double angularCommand = std::clamp(angularTarget, -m_finalAdjustAngularSpeed, m_finalAdjustAngularSpeed);
            sendRouteVelocity(0.0, angularCommand);
            return;
        }
        if (segment.polyline.size() > 1) {
            segment.targetIndex = 1;
        }
    }
    const QPointF current(m_currentPoseX, m_currentPoseY);
    const QPointF target = segment.polyline.at(segment.targetIndex);
    const double distance = QLineF(current, target).length();
    if (segment.targetIndex == segment.polyline.size() - 1) {
        if (distance <= m_arrivalDistanceThreshold) {
            const double headingError = normalizeAngle(segment.endTheta - m_currentPoseTheta);
            if (std::abs(headingError) <= m_arrivalAngleThresholdRad) {
                finalizeCurrentSegment(true);
                return;
            }
            const double angularTarget = std::clamp(m_angularGain * headingError, -m_maxAngularSpeed, m_maxAngularSpeed);
            const double angularCommand = std::clamp(angularTarget, -m_finalAdjustAngularSpeed, m_finalAdjustAngularSpeed);
            sendRouteVelocity(0.0, angularCommand);
            return;
        }
    } else {
        if (distance <= m_arrivalDistanceThreshold) {
            ++segment.targetIndex;
            return;
        }
    }
    const double angleToTarget = std::atan2(target.y() - current.y(), target.x() - current.x());
    const double headingError = normalizeAngle(angleToTarget - m_currentPoseTheta);
    double linear = m_linearGain * distance;
    linear = std::min(linear, m_maxLinearSpeed);
    if (std::abs(headingError) > m_headingStopThresholdRad) {
        linear = 0.0;
    } else if (std::abs(headingError) > m_headingSlowdownThresholdRad) {
        linear *= m_headingSlowdownFactor;
    }
    if (distance < m_arrivalDistanceThreshold * m_nearTargetDistanceMultiplier) {
        linear = std::min(linear, m_maxLinearSpeed * m_nearTargetSpeedMultiplier);
        linear = std::min(linear, m_finalAdjustLinearSpeed);
    }
    const double angular = std::clamp(m_angularGain * headingError, -m_maxAngularSpeed, m_maxAngularSpeed);
    sendRouteVelocity(linear, angular);
}
void Home::startNextSegment()
{
    if (m_pendingRouteSegments.isEmpty()) {
        m_routeFollowerActive = false;
        if (m_routeFollowerTimer && m_routeFollowerTimer->isActive()) {
            m_routeFollowerTimer->stop();
        }
        resetRouteCommandState();
        sendRouteVelocity(0.0, 0.0);
        return;
    }
    m_activeRouteSegment = m_pendingRouteSegments.dequeue();
    if (m_activeRouteSegment.polyline.isEmpty()) {
        m_activeRouteSegment.polyline.append(QPointF(m_currentPoseX, m_currentPoseY));
    }
    m_activeRouteSegment.targetIndex = 0;
    resetRouteCommandState();
    m_routeFollowerActive = true;
    if (m_routeFollowerTimer && !m_routeFollowerTimer->isActive()) {
        m_routeFollowerTimer->start();
    }
    processRouteFollowerTick();
}
void Home::finalizeCurrentSegment(bool success)
{
    if (m_routeFollowerTimer && m_routeFollowerTimer->isActive()) {
        m_routeFollowerTimer->stop();
    }
    m_routeFollowerActive = false;
    m_pendingRouteSegments.clear();
    resetRouteCommandState();
    sendVelocityCommand(0.0, 0.0);
    emit routeSegmentCompleted(success);
}
double Home::normalizeAngle(double angle) const
{
    while (angle > M_PI) {
        angle -= 2.0 * M_PI;
    }
    while (angle < -M_PI) {
        angle += 2.0 * M_PI;
    }
    return angle;
}

void Home::sendStartupWebSocketMessages()
{
    if (startupMessagesSent) {
        return;
    }
    if (!webSocket || webSocket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    const QJsonObject stopPacket{{"cmd", "region"}, {"region", "slam"}, {"index", 1}};
    const QJsonObject stopMsg{{"talk", "stopLocation"}};
    const QString stopPayload =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"packet", stopPacket}, {"msg", stopMsg}})
                              .toJson(QJsonDocument::Compact));

    const QJsonObject scriptPacket{{"cmd", "region"}, {"region", "ScriptDeal"}, {"index", 1}};
    const QJsonObject scriptMsg{{"talk", "printScript"},
                                {"name", QStringLiteral(u"script/motor/步科电机-差速轮/recmotor.lua")}};
    const QString scriptPayload =
        QString::fromUtf8(QJsonDocument(QJsonObject{{"packet", scriptPacket}, {"msg", scriptMsg}})
                              .toJson(QJsonDocument::Compact));

    for (int i = 0; i < 3; ++i) {
        webSocket->sendTextMessage(stopPayload);
    }
    for (int i = 0; i < 3; ++i) {
        webSocket->sendTextMessage(scriptPayload);
    }

    startupMessagesSent = true;
    qDebug() << "Startup WebSocket messages dispatched";
}

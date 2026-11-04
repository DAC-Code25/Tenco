#include "maintenance.h"
#include "camerasettingspanel.h"
#include "navigationdiagnosticspanel.h"
#include "ipcmanagementpanel.h"
#include "supportpagestyle.h"
#include "parameterwheelguard.h"

#include "configmanager.h"
#include "loggingmanager.h"
#include "ui_mainwindow.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

#include <limits>

namespace {
constexpr int kFieldMinWidth = 240;

bool hasRetiredLocalSettings(const QJsonObject &document)
{
    return document.contains("geo") || document.contains("taskDefaults") ||
        document.contains("control") || document.contains("rowWork") || document.contains("vehicle");
}
const char *kMaintenanceStyle = R"(
QFrame#maintenanceHero { background:transparent; border:none; }
QFrame#maintenanceCard, QFrame#maintenanceNavigation { background:white; border:1px solid #dfe7e3; border-radius:12px; }
QLabel#maintenanceTitle { color:#183a2c; font-size:24px; font-weight:700; }
QLabel#maintenanceSubtitle, QLabel#maintenanceHint, QLabel#maintenancePageDesc { color:#718278; font-size:12px; }
QLabel#maintenancePageTitle { color:#183a2c; font-size:22px; font-weight:700; }
QLabel#maintenanceSectionTitle { color:#24775c; font-size:13px; font-weight:600; padding:12px 0 6px; border-bottom:1px solid #edf1ee; }
QLabel#maintenanceFieldLabel { color:#3f564a; font-size:13px; }
QLabel#maintenanceFieldHint { color:#7a8b80; font-size:12px; }
QLabel#maintenanceReadonlyValue { color:#294a38; font-size:13px; padding:8px 0; }
QLabel#maintenanceStatusLabel { color:#647a6c; font-size:12px; }
QTabWidget#maintenanceTabWidget::pane { border:none; background:transparent; }
QWidget#maintenanceUnitField { background:white; border:1px solid #d3ded7; border-radius:7px; }
QSpinBox#maintenanceUnitSpinBox, QDoubleSpinBox#maintenanceUnitSpinBox { background:transparent; border:none; padding:7px 8px; }
QLabel#maintenanceUnitLabel { color:#7b8c81; font-size:12px; padding-right:10px; }
QSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width:0; border:none; }
QPushButton#maintenancePrimaryButton { background:#24775c; color:white; border-color:#24775c; }
QPushButton#maintenancePrimaryButton:hover { background:#1e664e; }
QPushButton#maintenancePrimaryButton:disabled { background:#dce9e1; color:#8da798; border-color:#dce9e1; }
QWidget#cameraSettingsPanel { background:white; border:1px solid #dfe7e3; border-radius:12px; }
QWidget#cameraSettingsPanel > QLabel { border:none; background:transparent; }
QPushButton#cameraSettingsApply { background:#24775c; color:white; border-color:#24775c; }
QPushButton#cameraSettingsApply:disabled { background:#dce9e1; color:#8da798; border-color:#dce9e1; }
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

void applyInputFocus(QWidget *widget)
{
    if (!widget) {
        return;
    }

    widget->setFocusPolicy(Qt::StrongFocus);

    if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
        if (auto *lineEdit = spin->findChild<QLineEdit *>()) {
            lineEdit->setFocusPolicy(Qt::StrongFocus);
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

QString parameterDescription(const QString &key)
{
    static const QHash<QString, QString> descriptions = {
        {QStringLiteral("network.websocketUrl"), QStringLiteral("底盘 WebSocket 控制地址，用于首页向小车发送手动速度及本通道停止指令。控制器 IP 或端口变化时修改。")},
        {QStringLiteral("network.statusReadUrl"), QStringLiteral("状态读取 HTTP 地址，用于获取电量、电压和设备模式等运行状态。状态页无数据或换控制器时检查。")},
        {QStringLiteral("network.authToken"), QStringLiteral("上位机访问工控机接口的认证令牌，需要与目标服务保持一致；此处只保存本机凭据，不修改服务端密钥。")},
        {QStringLiteral("network.statusPollIntervalMs"), QStringLiteral("状态轮询周期。数值越小状态越实时，但网络和控制器负载越高。")},
        {QStringLiteral("network.statusRequestTimeoutMs"), QStringLiteral("单次状态请求超时时间。太小容易误判离线，太大会让故障恢复变慢。")},
        {QStringLiteral("network.statusMaxBackoffMs"), QStringLiteral("状态请求连续失败后的最大退避间隔。网络不稳定时适当增大可减少日志和请求压力。")},
        {QStringLiteral("network.chassisAutoReconnect"), QStringLiteral("底盘 WebSocket 断开后是否自动重连。生产运行建议开启。")},
        {QStringLiteral("network.chassisReconnectIntervalMs"), QStringLiteral("底盘断线后的初始重连间隔。数值越小恢复更快，但断网时重连更频繁。")},
        {QStringLiteral("network.chassisReconnectMaxIntervalMs"), QStringLiteral("底盘连续重连失败后的最大重连间隔，不能小于基础重连间隔。")},

        {QStringLiteral("video.backend"), QStringLiteral("视频源类型。mjpeg_http 表示使用工控机 HTTP MJPEG 流，oak_depthai 表示本机直连 OAK 相机。")},
        {QStringLiteral("video.streamUrl"), QStringLiteral("仅在未配置工控机相机服务时使用的通用 MJPEG 地址；不会覆盖已配置服务的彩色和深度流。")},
        {QStringLiteral("video.controlBaseUrl"), QStringLiteral("工控机相机服务唯一地址，统一用于彩色预览、深度预览、拍照、录像和状态查询。")},
        {QStringLiteral("video.deviceId"), QStringLiteral("OAK 相机设备 ID。单台本机 OAK 可留空，多台相机时建议指定，避免打开错误设备。")},
        {QStringLiteral("video.previewWidth"), QStringLiteral("请求的视频预览宽度。分辨率越高画面越清晰，但 CPU、网络和解码压力越大。")},
        {QStringLiteral("video.previewHeight"), QStringLiteral("本机 OAK 后端请求的预览高度，需与本机相机支持能力匹配。")},
        {QStringLiteral("video.previewFps"), QStringLiteral("请求的视频预览帧率。帧率越高越流畅，但负载越高。")},
        {QStringLiteral("video.recordMode"), QStringLiteral("本机 OAK 后端录像模式。当前仅支持 host_opencv，由本机 OpenCV 处理录像；不修改工控机相机服务。")},
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




        {QStringLiteral("logging.level"), QStringLiteral("运行日志最低输出等级。debug 最详细，info 适合调试，warn/error 适合现场稳定运行。")},
        {QStringLiteral("logging.consoleEnabled"), QStringLiteral("是否输出到 Qt Creator 应用程序输出或终端。现场发布版可关闭以减少控制台噪声。")},
        {QStringLiteral("logging.fileEnabled"), QStringLiteral("是否写入运行日志文件。生产维护建议开启。")},
        {QStringLiteral("logging.includeSourceLocation"), QStringLiteral("是否记录源码文件和行号。定位开发问题有用，但会增加日志长度。")},
        {QStringLiteral("logging.includeThreadId"), QStringLiteral("是否记录线程 ID。排查网络、视频、云台等多线程问题时建议开启。")},
        {QStringLiteral("logging.includeCategory"), QStringLiteral("是否记录模块分类，例如 tenco.net.status 或 tenco.gimbal.control。")},
        {QStringLiteral("logging.maxFileBytes"), QStringLiteral("本次运行日志容量上限。达到上限后在同一文件中保留最新记录，不创建分流或轮转文件。")},
        {QStringLiteral("logging.redactSensitiveData"), QStringLiteral("是否对 token、Authorization、password、secret 等敏感信息脱敏。生产环境必须开启。")},
        {QStringLiteral("logging.categoryRules"), QStringLiteral("Qt 分类日志过滤规则，每行一条，例如 tenco.net.status.debug=false。高级维护人员使用。")},
        {QStringLiteral("logging.currentLevel"), QStringLiteral("当前运行中实际生效的日志等级。")},
        {QStringLiteral("logging.currentFile"), QStringLiteral("当前运行日志文件路径。")},
        {QStringLiteral("logging.actions"), QStringLiteral("日志维护操作：刷新日志状态、打开日志目录和导出现场诊断包。")},

        {QStringLiteral("production.configPath"), QStringLiteral("当前程序实际读取和写入的 config.json 路径，用于确认是否改到了正确配置文件。")},
        {QStringLiteral("production.loadedFromFile"), QStringLiteral("显示当前配置是否来自磁盘文件；否表示程序使用了内置默认配置。")},
        {QStringLiteral("production.runtimeDir"), QStringLiteral("当前程序可执行文件所在目录，用于排查运行目录和配置文件路径问题。")},
        {QStringLiteral("production.backupDir"), QStringLiteral("启动自动备份和手动恢复使用的配置备份目录。")},
        {QStringLiteral("production.backupCount"), QStringLiteral("本机配置快照数量，包含可读取的旧目录备份；不自动删除现场备份。")},
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

    field->setProperty("maintenanceConfigKey", key);
    const QString description = parameterDescription(key);
    auto *label = new QLabel(labelText);
    label->setObjectName(QStringLiteral("maintenanceFieldLabel"));
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setMinimumHeight(36);
    label->setFixedWidth(154);
    label->setWordWrap(true);

    auto *fieldBox = new QWidget();
    auto *fieldLayout = new QVBoxLayout(fieldBox);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(5);
    fieldLayout->addWidget(field);
    fieldBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    if (!description.isEmpty()) {
        auto *hint = new QLabel(description, fieldBox);
        hint->setObjectName(QStringLiteral("maintenanceFieldHint"));
        hint->setWordWrap(true);
        hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        fieldLayout->addWidget(hint);
        hint->hide();
        label->setToolTip(description);
        field->setToolTip(description);
        field->setAccessibleDescription(description);
        fieldBox->setToolTip(description);
    }

    layout->addRow(label, fieldBox);
}

void addSection(QFormLayout *layout, const QString &title)
{
    auto *label = new QLabel(title);
    label->setObjectName("maintenanceSectionTitle");
    layout->addRow(label);
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
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(12);
    ui->maintenancePage->setObjectName(QStringLiteral("maintenanceRoot"));
    SupportPageStyle::apply(ui->maintenancePage, QString::fromUtf8(kMaintenanceStyle));

    auto *body = new QHBoxLayout();
    body->setSpacing(18);
    auto *navigation = new QFrame(ui->maintenancePage);
    navigation->setObjectName("maintenanceNavigation");
    navigation->setFixedWidth(196);
    auto *navLayout = new QVBoxLayout(navigation);
    navLayout->setContentsMargins(12, 20, 12, 14);
    navLayout->setSpacing(12);
    auto *navTitle = new QLabel(tr("维护中心"), navigation);
    navTitle->setObjectName("maintenancePageTitle");
    navLayout->addWidget(navTitle);
    auto *navHint = new QLabel(tr("设备状态与运行设置"), navigation);
    navHint->setObjectName("maintenanceHint");
    navLayout->addWidget(navHint);
    auto *navList = new QListWidget(navigation);
    navList->setObjectName("maintenanceNavList");
    navList->setProperty("supportNavigation", true);
    navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navLayout->addWidget(navList, 1);
    body->addWidget(navigation);
    auto *workspace = new QVBoxLayout();
    workspace->setSpacing(12);
    body->addLayout(workspace, 1);
    rootLayout->addLayout(body, 1);

    auto *hero = new QFrame(ui->maintenancePage);
    hero->setObjectName(QStringLiteral("maintenanceHero"));
    hero->setProperty("maintenanceConfigRegion", true);
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(4, 0, 4, 0);
    heroLayout->setSpacing(12);
    auto *titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(4);
    auto *desc = new QLabel(tr("上位机本机设置"), hero);
    desc->setToolTip(tr("配置文件：%1\n保存本机仅落盘；保存并应用更新上位机连接与偏好，不修改工控机参数。")
        .arg(ConfigManager::instance().configFilePath()));
    desc->setObjectName(QStringLiteral("maintenanceSubtitle"));
    desc->setWordWrap(true);
    titleBlock->addWidget(desc);
    heroLayout->addLayout(titleBlock, 1);
    m_dirtyLabel = new QLabel(hero);
    m_dirtyLabel->setObjectName("maintenanceDirtyState");
    heroLayout->addWidget(m_dirtyLabel);
    workspace->addWidget(hero);

    auto *tabs = new QTabWidget(ui->maintenancePage);
    tabs->setObjectName(QStringLiteral("maintenanceTabWidget"));
    tabs->tabBar()->hide();
    auto wrapScrollable = [](QWidget *content) {
        auto *scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setWidget(content);
        return scroll;
    };
    auto *diagnostics = new NavigationDiagnosticsPanel();
    m_navigationDiagnostics = diagnostics;
    const auto configureDiagnostics = [diagnostics] {
        const auto &config = ConfigManager::instance();
        QUrl managementUrl(config.tracking().baseUrl);
        managementUrl.setPort(18133);
        managementUrl.setPath("/api/v1");
        managementUrl.setQuery(QString());
        managementUrl.setFragment(QString());
        diagnostics->configure({QUrl(config.poseSource().baseUrl), QUrl(config.tracking().baseUrl),
            config.network().authToken, config.poseSource().requestTimeoutMs,
            config.tracking().requestTimeoutMs, config.poseSource().enabled, config.tracking().enabled, managementUrl});
    };
    configureDiagnostics();
    connect(&ConfigManager::instance(), &ConfigManager::configChanged, diagnostics, configureDiagnostics);
    tabs->addTab(wrapScrollable(diagnostics), tr("状态总览"));
    m_ipcManagement = new IpcManagementPanel;
    connect(m_ipcManagement, &IpcManagementPanel::releaseTaskSessionRequested, this, &Maintenance::releaseTaskSessionRequested);
    connect(m_ipcManagement, &IpcManagementPanel::originSnapshotChanged, this, &Maintenance::originSnapshotChanged);
    m_ipcManagement->setConnectionProvider([] {
        const auto &config = ConfigManager::instance();
        QUrl address(config.tracking().baseUrl);
        address.setPort(18133);
        address.setPath("/api/v1");
        address.setQuery(QString());
        address.setFragment(QString());
        return IpcManagementConnection{address, config.network().authToken, config.tracking().requestTimeoutMs};
    });
    auto *remoteCamera = new CameraSettingsPanel;
    remoteCamera->setConnectionProvider([] {
        const auto &config = ConfigManager::instance();
        return CameraSettingsConnection{QUrl(config.video().controlBaseUrl), config.network().authToken, config.video().cameraRequestTimeoutMs};
    });
    m_ipcManagement->addMaintenancePage(tr("相机采集"), remoteCamera);
    auto *vehicle = createVehiclePage();
    m_ipcManagement->addMaintenancePage(tr("车辆与原点 · 只读"), vehicle);
    auto *managementPage = m_ipcManagement;
    managementPage->setProperty("maintenanceRemotePage", true);
    tabs->addTab(managementPage, tr("工控机 · 服务与参数"));
    auto *connectionTabs = new QTabWidget;
    connectionTabs->setObjectName("maintenanceLocalConnectionTabs");
    connectionTabs->addTab(wrapScrollable(createNetworkPage()), tr("底盘连接与认证"));
    connectionTabs->addTab(wrapScrollable(createRowWorkPage()), tr("位姿与任务连接"));
    connectionTabs->addTab(wrapScrollable(createControlPage()), tr("手动控制"));
    tabs->addTab(connectionTabs, tr("本机 · 连接与手动"));
    auto *deviceTabs = new QTabWidget;
    deviceTabs->setObjectName("maintenanceLocalDeviceTabs");
    deviceTabs->addTab(wrapScrollable(createVideoPage()), tr("相机连接与预览"));
    deviceTabs->addTab(wrapScrollable(createGimbalPage()), tr("云台连接"));
    tabs->addTab(deviceTabs, tr("本机 · 相机与云台"));
    auto *localFilesTabs = new QTabWidget;
    localFilesTabs->setObjectName("maintenanceLocalFilesTabs");
    localFilesTabs->addTab(wrapScrollable(createProductionPage()), tr("配置与备份"));
    localFilesTabs->addTab(wrapScrollable(createLoggingPage()), tr("上位机日志"));
    tabs->addTab(localFilesTabs, tr("本机 · 备份与日志"));
    tabs->setCurrentIndex(0);
    workspace->addWidget(tabs, 1);
    for (int i = 0; i < tabs->count(); ++i)
        navList->addItem(tabs->tabText(i));
    connect(navList, &QListWidget::currentRowChanged, tabs, &QTabWidget::setCurrentIndex);
    connect(tabs, &QTabWidget::currentChanged, navList, qOverload<int>(&QListWidget::setCurrentRow));
    navList->setCurrentRow(0);

    auto *footer = new QFrame(ui->maintenancePage);
    footer->setObjectName(QStringLiteral("maintenanceCard"));
    footer->setProperty("maintenanceConfigRegion", true);
    auto *footerBox = new QVBoxLayout(footer);
    footerBox->setContentsMargins(14, 10, 14, 12);
    footerBox->setSpacing(8);
    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    m_statusLabel = new QLabel(ui->maintenancePage);
    m_statusLabel->setObjectName(QStringLiteral("maintenanceStatusLabel"));
    m_statusLabel->setWordWrap(true);
    footerBox->addWidget(m_statusLabel);
    footerBox->addLayout(buttonRow);

    m_loadConfigButton = new QPushButton(tr("导入配置…"), ui->maintenancePage);
    m_loadConfigButton->setToolTip(tr("导入上位机本机配置供预览，显式保存前不会替换运行配置。"));
    m_loadConfigButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *reloadButton = new QPushButton(tr("还原编辑"), ui->maintenancePage);
    reloadButton->setToolTip(tr("丢弃本页尚未保存的编辑，恢复为当前上位机运行配置。"));
    reloadButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_restoreBackupButton = new QPushButton(tr("恢复备份…"), ui->maintenancePage);
    m_restoreBackupButton->setToolTip(tr("选择本机配置备份恢复；不修改工控机配置。"));
    m_restoreBackupButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_saveButton = new QPushButton(tr("保存本机"), ui->maintenancePage);
    m_saveButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    m_applyButton = new QPushButton(tr("保存并应用"), ui->maintenancePage);
    m_applyButton->setToolTip(tr("保存本机配置，并更新上位机连接、独立手动控制与显示偏好。"));
    m_applyButton->setObjectName(QStringLiteral("maintenancePrimaryButton"));
    m_applyButton->setDefault(true);
    buttonRow->addWidget(m_loadConfigButton);
    buttonRow->addWidget(reloadButton);
    buttonRow->addWidget(m_restoreBackupButton);
    buttonRow->addStretch();
    buttonRow->addWidget(m_saveButton);
    buttonRow->addWidget(m_applyButton);
    workspace->addWidget(footer);

    const auto updateConfigVisibility = [tabs, hero, footer] {
        const bool showConfig = tabs->currentIndex() != 0 && !tabs->currentWidget()->property("maintenanceRemotePage").toBool();
        hero->setVisible(showConfig);
        footer->setVisible(showConfig);
    };
    connect(tabs, &QTabWidget::currentChanged, this, updateConfigVisibility);
    updateConfigVisibility();

    connect(m_loadConfigButton, &QPushButton::clicked, this, &Maintenance::loadConfigFile);
    connect(reloadButton, &QPushButton::clicked, this, &Maintenance::loadFromConfig);
    connect(m_restoreBackupButton, &QPushButton::clicked, this, &Maintenance::restoreLatestBackup);
    connect(m_saveButton, &QPushButton::clicked, this, [this]() { saveConfig(false); });
    connect(m_applyButton, &QPushButton::clicked, this, [this]() { saveConfig(true); });
    ParameterWheelGuard::install(ui->maintenancePage);
}

void Maintenance::showIpcSessionReleaseResult(bool released, const QString &message)
{
    if (m_ipcManagement) m_ipcManagement->showSessionReleaseResult(released, message);
}

void Maintenance::openIpcMaintenancePage()
{
    if (!ui || !ui->maintenancePage) return;
    auto *tabs = ui->maintenancePage->findChild<QTabWidget *>("maintenanceTabWidget");
    if (!tabs) return;
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->widget(i)->property("maintenanceRemotePage").toBool()) {
            tabs->setCurrentIndex(i);
            return;
        }
    }
}

void Maintenance::refreshOriginSnapshot()
{
    if (m_ipcManagement && !m_ipcManagement->isVisible()) m_ipcManagement->refreshOriginSnapshot();
}

void Maintenance::setTrackingClient(TrackingClient *client)
{
    if (m_navigationDiagnostics) m_navigationDiagnostics->setTrackingClient(client);
}

QWidget *Maintenance::createFormPage(const QString &titleText, const QString &description, QFormLayout **outLayout) const
{
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 8, 0);
    pageLayout->setSpacing(14);

    auto *header = new QWidget(page);
    auto *headerBox = new QHBoxLayout(header);
    headerBox->setContentsMargins(4, 4, 4, 0);
    headerBox->setSpacing(12);
    auto *titles = new QVBoxLayout();
    titles->setSpacing(5);
    auto *pageTitle = new QLabel(titleText, header);
    pageTitle->setObjectName(QStringLiteral("maintenancePageTitle"));
    auto *pageDesc = new QLabel(description, header);
    pageDesc->setObjectName(QStringLiteral("maintenancePageDesc"));
    pageDesc->setWordWrap(true);
    pageDesc->hide();
    pageTitle->setToolTip(description);
    titles->addWidget(pageTitle);
    titles->addWidget(pageDesc);
    headerBox->addLayout(titles, 1);
    auto *descriptions = new QCheckBox(tr("参数说明"), header);
    descriptions->setObjectName("maintenanceDescriptionsToggle");
    descriptions->setToolTip(tr("展开各项参数的详细说明；鼠标悬停在参数上也可查看。"));
    headerBox->addWidget(descriptions, 0, Qt::AlignTop);
    connect(descriptions, &QCheckBox::toggled, page, [page, pageDesc](bool visible) {
        pageDesc->setVisible(visible);
        for (auto *hint : page->findChildren<QLabel *>("maintenanceFieldHint"))
            hint->setVisible(visible);
    });
    pageLayout->addWidget(header);

    auto *card = new QFrame(page);
    card->setObjectName(QStringLiteral("maintenanceCard"));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 8, 20, 20);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(16);
    cardLayout->addLayout(form);
    pageLayout->addWidget(card);
    pageLayout->addStretch(1);

    if (outLayout) {
        *outLayout = form;
    }
    return page;
}

QWidget *Maintenance::createNetworkPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("通信参数"),
                                tr("包含底盘手动直连和设备状态轮询地址，以及认证 token 和轮询间隔。"),
                                &layout);
    addSection(layout, tr("连接与认证"));
    addLineEdit(layout, QStringLiteral("network.websocketUrl"), tr("底盘 WebSocket"));
    addLineEdit(layout, QStringLiteral("network.statusReadUrl"), tr("状态读取 HTTP"));
    addLineEdit(layout, QStringLiteral("network.authToken"), tr("认证 Token"));
    addSection(layout, tr("设备状态轮询"));
    addSpinBox(layout, QStringLiteral("network.statusPollIntervalMs"), tr("状态轮询间隔"), 50, 5000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.statusRequestTimeoutMs"), tr("状态请求超时"), 500, 30000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.statusMaxBackoffMs"), tr("状态失败退避上限"), 500, 60000, QStringLiteral(" ms"));
    addSection(layout, tr("底盘断线重连"));
    addCheckBox(layout, QStringLiteral("network.chassisAutoReconnect"), tr("底盘自动重连"));
    addSpinBox(layout, QStringLiteral("network.chassisReconnectIntervalMs"), tr("底盘重连基础间隔"), 200, 60000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("network.chassisReconnectMaxIntervalMs"), tr("底盘重连最大间隔"), 200, 120000, QStringLiteral(" ms"));
    return page;
}

QWidget *Maintenance::createVideoPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("视频与相机"),
                                tr("此处只维护上位机的视频连接与显示。使用工控机 MJPEG 时，采集尺寸和帧率统一在“工控机 · 服务与参数”的相机设置修改；本机 OAK 设置仅在本机直连后端显示。"),
                                &layout);
    addSection(layout, tr("上位机视频连接"));
    addComboBox(layout, QStringLiteral("video.backend"), tr("视频后端"), {QStringLiteral("mjpeg_http"), QStringLiteral("oak_depthai")});
    const int serviceRow = layout->rowCount();
    auto *serviceAddress = addLineEdit(layout, QStringLiteral("video.controlBaseUrl"), tr("工控机相机服务地址"));
    const int genericStreamRow = layout->rowCount();
    addLineEdit(layout, QStringLiteral("video.streamUrl"), tr("通用 MJPEG 地址"));
    const int localStartRow = layout->rowCount();
    addSection(layout, tr("本地 OAK 后端采集"));
    addLineEdit(layout, QStringLiteral("video.deviceId"), tr("OAK 设备 ID"));
    addSpinBox(layout, QStringLiteral("video.previewWidth"), tr("本地 OAK 后端预览宽度"), 320, 4096);
    addSpinBox(layout, QStringLiteral("video.previewHeight"), tr("本地 OAK 后端预览高度"), 240, 3040);
    addSpinBox(layout, QStringLiteral("video.previewFps"), tr("本地 OAK 后端预览帧率"), 1, 120);
    addComboBox(layout, QStringLiteral("video.recordMode"), tr("本地 OAK 后端录像模式"), {QStringLiteral("host_opencv")});
    addLineEdit(layout, QStringLiteral("video.recordCodec"), tr("本地 OAK 后端录像编码"));
    const int localEndRow = layout->rowCount();
    auto *backend = m_comboBoxes.value(QStringLiteral("video.backend"));
    const auto updateBackendFields = [layout, backend, serviceAddress, serviceRow, genericStreamRow, localStartRow, localEndRow] {
        const bool local = backend->currentText() == QStringLiteral("oak_depthai");
        layout->setRowVisible(serviceRow, !local);
        layout->setRowVisible(genericStreamRow, !local && serviceAddress->text().trimmed().isEmpty());
        for (int row = localStartRow; row < localEndRow; ++row)
            layout->setRowVisible(row, local);
    };
    connect(backend, &QComboBox::currentTextChanged, page, updateBackendFields);
    connect(serviceAddress, &QLineEdit::textChanged, page, updateBackendFields);
    updateBackendFields();
    addSection(layout, tr("重连与显示"));
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
                                tr("配置 PLC 连接与控制寄存器；软限位不能替代机械限位，保存并应用后生效。"),
                                &layout);
    addSection(layout, tr("PLC 连接"));
    addCheckBox(layout, QStringLiteral("gimbal.enabled"), tr("启用云台控制"));
    addLineEdit(layout, QStringLiteral("gimbal.plcHost"), tr("PLC 地址"));
    addSpinBox(layout, QStringLiteral("gimbal.plcPort"), tr("PLC 端口"), 1, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.unitId"), tr("Modbus Unit ID"), 1, 255);
    addSpinBox(layout, QStringLiteral("gimbal.requestTimeoutMs"), tr("请求超时"), 200, 10000, QStringLiteral(" ms"));
    addSpinBox(layout, QStringLiteral("gimbal.statusPollIntervalMs"), tr("状态轮询间隔"), 100, 10000, QStringLiteral(" ms"));
    addSection(layout, tr("寄存器映射"));
    addSpinBox(layout, QStringLiteral("gimbal.heightControlAddress"), tr("升降控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.pitchControlAddress"), tr("俯仰控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.yawControlAddress"), tr("水平控制寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.statusStartAddress"), tr("状态起始寄存器"), 0, 65535);
    addSpinBox(layout, QStringLiteral("gimbal.statusRegisterCount"), tr("状态寄存器数量"), 7, 64);
    addSection(layout, tr("软限位与安全超时"));
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
    QFormLayout* layout = nullptr;
    auto* page = createFormPage(tr("工控机连接"), tr("位姿和任务使用独立服务；手动控制继续直连底盘。"), &layout);
    addSection(layout, tr("位姿服务"));
    addCheckBox(layout, "poseSource.enabled", tr("位姿服务启用"));
    addLineEdit(layout, "poseSource.baseUrl", tr("位姿服务地址"));
    addSpinBox(layout, "poseSource.requestTimeoutMs", tr("位姿服务请求超时"), 500, 10000, " ms");
    addSection(layout, tr("任务服务"));
    addCheckBox(layout, "tracking.enabled", tr("任务服务启用"));
    addLineEdit(layout, "tracking.baseUrl", tr("任务服务地址"));
    addSpinBox(layout, "tracking.requestTimeoutMs", tr("任务服务请求超时"), 500, 10000, " ms");
    return page;
}

QWidget *Maintenance::createControlPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("手动控制"),
                                tr("此处只调整上位机独立手动操作。自动行驶的速度、停车与转向参数统一在“工控机 · 服务与参数 → 导航参数”修改，任务自动使用工控机回读值。"),
                                &layout);
    addSection(layout, tr("独立手动控制"));
    addDoubleSpinBox(layout, "manualControl.maxLinearSpeed", tr("手动最大线速度 m/s"), 0.0, VehicleParameters::maxOperatingSpeedMps, 3);
    addDoubleSpinBox(layout, "manualControl.maxAngularSpeed", tr("手动最大角速度 rad/s"), 0.0, 5.0, 3);
    addSpinBox(layout, "manualControl.manualMotionRepeatIntervalMs", tr("手动心跳间隔"), 20, 1000, " ms");
    return page;
}

QWidget *Maintenance::createVehiclePage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("工控机标定与原点"),
        tr("车辆标定、原点和自动控制参数由工控机统一管理。此处显示当前回读值。"), &layout);
    m_ipcConfiguration = new QLabel(tr("导航服务未启动或未连接，尚未回读配置"), page);
    m_ipcConfiguration->setWordWrap(true);
    m_ipcConfiguration->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addRow(m_ipcConfiguration);
    return page;
}

void Maintenance::setIpcConfiguration(const QJsonObject& config)
{
    m_ipcConfigurationSnapshot = config;
    if (!m_ipcConfiguration) return;
    if (config.isEmpty()) { m_ipcConfiguration->setText(tr("导航服务未启动或未连接，尚未回读配置；工控机管理与相机状态请查看上方独立服务。")); return; }
    const auto cal = config["calibration"].toObject();
    const auto origin = config["origin"].toObject()["active"].toObject();
    QString detail = tr("标定版本：%1（%2）\n轮距：%3 m\n左/右轮径：%4 / %5 m\n轮端 ticks/圈：%6")
        .arg(cal["calibrationId"].toString(), cal["verified"].toBool() ? tr("已核实") : tr("未核实"))
        .arg(cal["wheelBaseMeters"].toDouble()).arg(cal["leftWheelDiameterMeters"].toDouble())
        .arg(cal["rightWheelDiameterMeters"].toDouble()).arg(cal["ticksPerWheelRevolution"].toDouble());
    detail += origin.isEmpty() ? tr("\n当前运行原点尚未确认，等待融合位姿；已保存原点见上方服务维护区。") :
        tr("\n运行原点版本：%1\n纬度/经度：%2 / %3\n高度：%4 m")
            .arg(origin["originRevision"].toString()).arg(origin["latitude"].toDouble(), 0, 'f', 8)
            .arg(origin["longitude"].toDouble(), 0, 'f', 8).arg(origin["altitude"].toDouble());
    m_ipcConfiguration->setText((m_ipcMonitoringCurrent ? QString() :
        tr("无线任务监视状态延迟，以下为上次配置；当前运行状态待更新。\n")) +
        detail + tr("\n控制配置：%1").arg(config["profile_revision"].toString()));
}

void Maintenance::setIpcMonitoringCurrent(bool current)
{
    m_ipcMonitoringCurrent = current;
    setIpcConfiguration(m_ipcConfigurationSnapshot);
}

QWidget *Maintenance::createLoggingPage()
{
    QFormLayout *layout = nullptr;
    auto *page = createFormPage(tr("日志系统"),
                                tr("每次启动使用一个按时间命名的日志文件，统一记录关键操作和状态变化。达到容量上限后保留最新记录，不记录高频运动命令。"),
                                &layout);
    addSection(layout, tr("日志输出"));
    addComboBox(layout,
                QStringLiteral("logging.level"),
                tr("最低日志等级"),
                {QStringLiteral("debug"), QStringLiteral("info"), QStringLiteral("warn"), QStringLiteral("error"), QStringLiteral("fatal")});
    addCheckBox(layout, QStringLiteral("logging.consoleEnabled"), tr("输出到控制台"));
    addCheckBox(layout, QStringLiteral("logging.fileEnabled"), tr("写入运行日志文件"));
    addCheckBox(layout, QStringLiteral("logging.includeSourceLocation"), tr("记录源码位置"));
    addCheckBox(layout, QStringLiteral("logging.includeThreadId"), tr("记录线程 ID"));
    addCheckBox(layout, QStringLiteral("logging.includeCategory"), tr("记录分类名称"));
    addSection(layout, tr("运行日志存储"));
    auto *runtimeSize = addSpinBox(layout, QStringLiteral("logging.maxFileBytes"), tr("本次运行日志容量"), 256 * 1024, 16 * 1024 * 1024, QStringLiteral(" bytes"));
    if (runtimeSize) {
        runtimeSize->setSingleStep(1024 * 1024);
    }
    addSection(layout, tr("隐私"));
    addCheckBox(layout, QStringLiteral("logging.redactSensitiveData"), tr("敏感信息脱敏"));
    addSection(layout, tr("分类过滤"));
    addPlainTextEdit(layout, QStringLiteral("logging.categoryRules"), tr("分类过滤规则"), tr("每行一条，例如：tenco.net.status.debug=false"));
    addSection(layout, tr("当前运行状态"));

    m_loggingCurrentLevelValue = new QLabel(page);
    m_loggingCurrentFileValue = new QLabel(page);
    const QList<QLabel *> labels{m_loggingCurrentLevelValue, m_loggingCurrentFileValue};
    for (QLabel *label : labels) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        label->setObjectName(QStringLiteral("maintenanceReadonlyValue"));
    }
    addDescribedRow(layout, QStringLiteral("logging.currentLevel"), tr("当前运行等级"), m_loggingCurrentLevelValue);
    addDescribedRow(layout, QStringLiteral("logging.currentFile"), tr("当前运行日志文件"), m_loggingCurrentFileValue);

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
    auto *page = createFormPage(tr("本机配置与备份"),
                                tr("只管理上位机程序目录中的 config.json。备份包含连接凭据，保存到 backups/config/local；启动、保存和恢复前保留原文件及 SHA-256 清单。旧 config_backups 只读兼容，不自动迁移或删除。"),
                                &layout);
    addSection(layout, tr("程序与配置来源"));
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
    auto *versionsCard = new QFrame(page);
    versionsCard->setObjectName(QStringLiteral("maintenanceConfigurationVersionCard"));
    versionsCard->setStyleSheet(QStringLiteral(
        "QFrame#maintenanceConfigurationVersionCard { background:#f7faf8; border:1px solid #e0e9e3; border-radius:9px; }"
        "QLabel { background:transparent; border:none; }"));
    auto *versionsLayout = new QGridLayout(versionsCard);
    versionsLayout->setContentsMargins(14, 12, 14, 12);
    versionsLayout->setHorizontalSpacing(16);
    versionsLayout->setVerticalSpacing(8);
    versionsLayout->setColumnStretch(1, 1);
    m_configDiskRevisionValue = new QLabel(versionsCard);
    m_configDiskRevisionValue->setObjectName(QStringLiteral("maintenanceDiskRevisionValue"));
    m_configRuntimeRevisionValue = new QLabel(versionsCard);
    m_configRuntimeRevisionValue->setObjectName(QStringLiteral("maintenanceRuntimeRevisionValue"));
    const QList<QLabel *> revisionValues{m_configDiskRevisionValue, m_configRuntimeRevisionValue};
    const QStringList revisionTitles{tr("磁盘文件"), tr("当前加载")};
    for (int row = 0; row < revisionValues.size(); ++row) {
        auto *title = new QLabel(revisionTitles[row], versionsCard);
        title->setProperty("supportCaption", true);
        title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        auto *value = revisionValues[row];
        value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setMinimumHeight(24);
        value->setStyleSheet(QStringLiteral("font-family:Consolas,monospace; font-size:13px; color:#294a38;"));
        versionsLayout->addWidget(title, row, 0);
        versionsLayout->addWidget(value, row, 1);
    }
    m_configurationVersions = new QLabel(versionsCard);
    m_configurationVersions->setObjectName(QStringLiteral("maintenanceVersionState"));
    m_configurationVersions->setAlignment(Qt::AlignCenter);
    m_configurationVersions->setWordWrap(true);
    m_configurationVersions->setMinimumHeight(28);
    versionsLayout->addWidget(m_configurationVersions, 2, 0, 1, 2);
    addDescribedRow(layout, QStringLiteral("production.configurationVersions"), tr("配置版本"), versionsCard);
    addSection(layout, tr("配置备份"));
    addDescribedRow(layout, QStringLiteral("production.backupDir"), tr("备份目录"), m_backupDirValue);
    addDescribedRow(layout, QStringLiteral("production.backupCount"), tr("备份数量"), m_backupCountValue);
    addDescribedRow(layout, QStringLiteral("production.latestBackup"), tr("最近备份"), m_latestBackupValue);

    auto *buttonRow = new QGridLayout();
    buttonRow->setSpacing(10);
    auto *refreshButton = new QPushButton(tr("刷新状态"), page);
    refreshButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *openConfigDirButton = new QPushButton(tr("打开配置目录"), page);
    openConfigDirButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *openBackupDirButton = new QPushButton(tr("打开备份目录"), page);
    openBackupDirButton->setObjectName(QStringLiteral("maintenanceSecondaryButton"));
    auto *selfCheckButton = new QPushButton(tr("配置自检"), page);
    selfCheckButton->setObjectName(QStringLiteral("maintenancePrimaryButton"));
    buttonRow->addWidget(refreshButton, 0, 0);
    buttonRow->addWidget(openConfigDirButton, 0, 1);
    buttonRow->addWidget(openBackupDirButton, 0, 2);
    buttonRow->addWidget(selfCheckButton, 1, 0);
    auto *backupButton = new QPushButton(tr("立即备份本机配置"), page);
    backupButton->setObjectName("maintenanceCreateLocalBackup");
    buttonRow->addWidget(backupButton, 1, 1);
    auto *restoreButton = new QPushButton(tr("选择备份恢复"), page);
    restoreButton->setObjectName("maintenanceRestoreLocalBackup");
    buttonRow->addWidget(restoreButton, 1, 2);
    auto *buttonContainer = new QWidget(page);
    buttonContainer->setLayout(buttonRow);
    addDescribedRow(layout, QStringLiteral("production.actions"), tr("维护操作"), buttonContainer);

    connect(refreshButton, &QPushButton::clicked, this, [this]() {
        refreshProductionInfo();
        updateActionState();
    });
    connect(selfCheckButton, &QPushButton::clicked, this, &Maintenance::runConfigSelfCheck);
    connect(backupButton, &QPushButton::clicked, this, &Maintenance::createManualBackup);
    connect(restoreButton, &QPushButton::clicked, this, &Maintenance::restoreLatestBackup);
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
    if (snap.video.backend == QStringLiteral("oak_depthai") && snap.video.deviceId.trimmed().isEmpty()) {
        warnings << tr("OAK 后端未指定设备 ID：单设备可为空，多设备现场建议指定。");
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
        m_loadedFromFileValue->setText(config.loadedFromFile() ? tr("已加载本机文件") : tr("文件未加载；请核对配置警告。损坏或未知版本不会被默认模板覆盖。"));
    }
    if (m_runtimeDirValue) {
        m_runtimeDirValue->setText(QCoreApplication::applicationDirPath());
    }
    if (m_backupDirValue) {
        m_backupDirValue->setText(config.backupDirectoryPath());
    }
    if (m_backupCountValue) {
        m_backupCountValue->setText(tr("%1 份（包含可读取的旧备份）").arg(backups.size()));
    }
    if (m_latestBackupValue) {
        m_latestBackupValue->setText(latest.isEmpty() ? tr("暂无备份") : latest);
    }
    if (m_configurationVersions) {
        const QString disk = config.diskRevision();
        const QString runtime = config.runtimeRevision();
        const bool known = !disk.isEmpty() && !runtime.isEmpty();
        const bool matching = known && disk == runtime;
        m_configDiskRevisionValue->setText(disk.isEmpty() ? tr("未知") : disk.left(16));
        m_configRuntimeRevisionValue->setText(runtime.isEmpty() ? tr("未知") : runtime.left(16));
        m_configDiskRevisionValue->setToolTip(tr("磁盘文件完整版本：%1").arg(disk.isEmpty() ? tr("未知") : disk));
        m_configRuntimeRevisionValue->setToolTip(tr("当前加载完整版本：%1").arg(runtime.isEmpty() ? tr("未知") : runtime));
        m_configurationVersions->setText(!known ? tr("版本待读取")
            : matching ? tr("磁盘与已加载配置一致") : tr("磁盘与已加载配置不同"));
        m_configurationVersions->setToolTip(!known ? tr("尚未获得完整配置版本，请刷新状态并检查配置是否已加载。")
            : matching ? tr("磁盘与本机已加载版本一致。") : tr("磁盘与运行版本不同，保存不等于应用；核对参数后再明确应用。"));
        const QString stateStyle = QStringLiteral(
            "background:%1; color:%2; border:none; border-radius:6px; padding:4px 10px; font-size:12px; font-weight:500;")
            .arg(!known ? QStringLiteral("#edf1ee") : matching ? QStringLiteral("#e7f2eb") : QStringLiteral("#fff3df"),
                 !known ? QStringLiteral("#718278") : matching ? QStringLiteral("#24775c") : QStringLiteral("#946200"));
        if (m_configurationVersions->styleSheet() != stateStyle) m_configurationVersions->setStyleSheet(stateStyle);
    }
}

void Maintenance::createManualBackup()
{
    QString error;
    if (!ConfigManager::instance().createBackup(&error)) {
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("本机备份失败"), error);
        return;
    }
    refreshProductionInfo();
    updateActionState();
    if (m_statusLabel) m_statusLabel->setText(tr("本机原文件及摘要已备份或复用同内容快照；运行配置未更改。"));
}

QLineEdit *Maintenance::addLineEdit(QFormLayout *layout, const QString &key, const QString &label, const QString &placeholder)
{
    auto *edit = new QLineEdit();
    if (key == "network.authToken") edit->setEchoMode(QLineEdit::PasswordEchoOnEdit);
    edit->setPlaceholderText(placeholder);
    applyInputFocus(edit);
    applyFieldSize(edit);
    connect(edit, &QLineEdit::textChanged, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, edit);
    m_lineEdits.insert(key, edit);
    return edit;
}

QSpinBox *Maintenance::addSpinBox(QFormLayout *layout, const QString &key, const QString &label, int min, int max, const QString &suffix)
{
    auto *spin = new QSpinBox();
    spin->setProperty("maintenanceConfigKey", key);
    spin->setRange(min, max);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    applyInputFocus(spin);
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
    applyInputFocus(spin);
    applyFieldSize(spin);
    connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, createUnitField(spin, suffix));
    m_doubleSpinBoxes.insert(key, spin);
    return spin;
}

QCheckBox *Maintenance::addCheckBox(QFormLayout *layout, const QString &key, const QString &label)
{
    auto *check = new QCheckBox();
    applyInputFocus(check);
    connect(check, &QCheckBox::toggled, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, check);
    m_checkBoxes.insert(key, check);
    return check;
}

QComboBox *Maintenance::addComboBox(QFormLayout *layout, const QString &key, const QString &label, const QStringList &items)
{
    auto *combo = new QComboBox();
    combo->addItems(items);
    applyInputFocus(combo);
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
    applyInputFocus(edit);
    edit->setMinimumHeight(110);
    edit->setMinimumWidth(kFieldMinWidth);
    edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    connect(edit, &QPlainTextEdit::textChanged, this, &Maintenance::markDirty);
    addDescribedRow(layout, key, label, edit);
    m_plainTextEdits.insert(key, edit);
    return edit;
}

void Maintenance::loadFromConfig()
{
    const auto snap = ConfigManager::instance().snapshot();
    loadSnapshotToForm(snap);
    if (m_statusLabel) {
        m_statusLabel->setText(hasRetiredLocalSettings(snap.sourceDocument) ?
            tr("已加载本机配置 · 旧自动导航参数已停用") : tr("已加载本机配置"));
        m_statusLabel->setToolTip(tr("配置文件：%1\n旧本机原点和导航配置不生效，自动参数以工控机为准。显式保存时先备份原文，再清除旧字段。")
            .arg(ConfigManager::instance().configFilePath()));
    }
}

void Maintenance::loadSnapshotToForm(const ConfigManager::ConfigSnapshot &snap)
{
    m_savedFormRevision.clear();
    m_formSnapshotBase = snap;
    m_formSourceDocument = snap.sourceDocument;
    m_loading = true;
    setLineValue(QStringLiteral("network.websocketUrl"), snap.network.websocketUrl);
    setLineValue(QStringLiteral("network.statusReadUrl"), snap.network.statusReadUrl);
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

    setDoubleValue("manualControl.maxLinearSpeed", snap.manualControl.maxLinearSpeed);
    setDoubleValue("manualControl.maxAngularSpeed", snap.manualControl.maxAngularSpeed);
    setIntValue("manualControl.manualMotionRepeatIntervalMs", snap.manualControl.manualMotionRepeatIntervalMs);
    setBoolValue("poseSource.enabled", snap.poseSource.enabled);
    setLineValue("poseSource.baseUrl", snap.poseSource.baseUrl);
    setIntValue("poseSource.requestTimeoutMs", snap.poseSource.requestTimeoutMs);
    setBoolValue("tracking.enabled", snap.tracking.enabled);
    setLineValue("tracking.baseUrl", snap.tracking.baseUrl);
    setIntValue("tracking.requestTimeoutMs", snap.tracking.requestTimeoutMs);


    m_cleanFingerprint = snapshotFingerprint(snap);
    m_loading = false;
    updateActionState();
    refreshProductionInfo();
}

ConfigManager::ConfigSnapshot Maintenance::collectSnapshot() const
{
    auto snap = m_formSnapshotBase;
    snap.sourceDocument = m_formSourceDocument;
    snap.network.websocketUrl = lineValue(QStringLiteral("network.websocketUrl"));
    snap.network.statusReadUrl = lineValue(QStringLiteral("network.statusReadUrl"));
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


    snap.logging.level = comboValue(QStringLiteral("logging.level"));
    snap.logging.consoleEnabled = boolValue(QStringLiteral("logging.consoleEnabled"));
    snap.logging.fileEnabled = boolValue(QStringLiteral("logging.fileEnabled"));
    snap.logging.includeSourceLocation = boolValue(QStringLiteral("logging.includeSourceLocation"));
    snap.logging.includeThreadId = boolValue(QStringLiteral("logging.includeThreadId"));
    snap.logging.includeCategory = boolValue(QStringLiteral("logging.includeCategory"));
    snap.logging.maxFileBytes = intValue(QStringLiteral("logging.maxFileBytes"));
    // Deprecated split-log settings remain readable for old configurations;
    // runtime logging now always uses a single file for this process run.
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

    snap.manualControl.maxLinearSpeed = doubleValue("manualControl.maxLinearSpeed");
    snap.manualControl.maxAngularSpeed = doubleValue("manualControl.maxAngularSpeed");
    snap.manualControl.manualMotionRepeatIntervalMs = intValue("manualControl.manualMotionRepeatIntervalMs");
    snap.poseSource.enabled = boolValue("poseSource.enabled");
    snap.poseSource.baseUrl = lineValue("poseSource.baseUrl");
    snap.poseSource.requestTimeoutMs = intValue("poseSource.requestTimeoutMs");
    snap.tracking.enabled = boolValue("tracking.enabled");
    snap.tracking.baseUrl = lineValue("tracking.baseUrl");
    snap.tracking.requestTimeoutMs = intValue("tracking.requestTimeoutMs");
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
    if (!requireUrl(snapshot.video.controlBaseUrl, tr("工控机相机服务地址"), true)) {
        return false;
    }
    if (snapshot.video.controlBaseUrl.trimmed().isEmpty() &&
        !requireUrl(snapshot.video.streamUrl, tr("通用 MJPEG 地址"), true)) return false;
    if ((snapshot.tracking.enabled && !requireUrl(snapshot.tracking.baseUrl, tr("任务服务地址"))) ||
        (snapshot.poseSource.enabled && !requireUrl(snapshot.poseSource.baseUrl, tr("位姿服务地址")))) {
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

    return true;
}

bool Maintenance::saveConfig(bool applyAfterSave)
{
    if (ConfigManager::instance().diskRevision() != ConfigManager::instance().expectedDiskRevision()) {
        updateActionState();
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("磁盘配置已变化"),
            tr("配置已在上位机之外修改，不能用旧表单覆盖或应用。请核对文件后重启上位机，再进行维护。"));
        return false;
    }
    const auto snap = collectSnapshot();
    if (!ConfigManager::instance().network().authToken.isEmpty() && snap.network.authToken.trimmed().isEmpty() &&
        QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("确认清空本机认证凭据"),
            tr("本次保存会清空上位机访问工控机与相机的认证令牌，可能导致连接返回 401。确定清空并继续保存？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
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

    const QString message = applyAfterSave ? tr("本机配置已保存并应用，上位机连接按新配置重连") : tr("本机配置已保存待应用；运行模块继续使用原值，可直接点击“应用已保存配置到本机”生效");
    m_cleanFingerprint = snapshotFingerprint(snap);
    m_savedFormRevision = ConfigManager::instance().diskRevision();
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

    if (QMessageBox::warning(ui ? ui->maintenancePage : nullptr, tr("导入本机配置"),
            tr("所选文件可能覆盖本机连接地址和认证凭据，也可能清空令牌。下一步只载入表单预览；保存前请核对。旧文件中的本机原点与自动导航参数已停用，保存时先备份原文再清除，不会覆盖工控机参数；其它未展示字段保留。是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;

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
        m_statusLabel->setText(tr("已加载配置到表单，尚未保存或应用：%1").arg(QFileInfo(filePath).fileName()) +
            (hasRetiredLocalSettings(snap.sourceDocument) ?
                tr("；旧本机原点/自动参数不会使用或发送到工控机，保存时保留原文备份后清除。") : QString()));
    }
}

void Maintenance::restoreLatestBackup()
{
    QString error;
    const QString latestPath = QFileDialog::getOpenFileName(ui ? ui->maintenancePage : nullptr,
        tr("选择本机配置备份"), ConfigManager::instance().backupDirectoryPath(), tr("配置备份 (config_*.json)"));
    if (latestPath.isEmpty()) {
        updateActionState();
        return;
    }

    const auto answer = QMessageBox::question(ui ? ui->maintenancePage : nullptr,
                                              tr("恢复本机配置备份"),
                                              tr("将校验所选备份的版本和摘要，先备份当前原文件，再恢复并应用到上位机。此操作会覆盖连接地址与认证凭据，令牌也可能变为空。旧 config_backups 无摘要，真实性需自行核对。\n\n备份文件：%1\n\n是否继续？")
                                                  .arg(QFileInfo(latestPath).fileName()),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    if (!ConfigManager::instance().restoreBackup(latestPath, &error, true)) {
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
        m_statusLabel->setText(tr("已恢复并应用所选本机配置：%1").arg(QFileInfo(latestPath).fileName()));
    }
}

bool Maintenance::hasUnsavedChanges() const
{
    return snapshotFingerprint(collectSnapshot()) != m_cleanFingerprint || (m_ipcManagement && m_ipcManagement->hasUnsavedChanges());
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
    const auto &config = ConfigManager::instance();
    const bool dirty = !m_loading && snapshotFingerprint(collectSnapshot()) != m_cleanFingerprint;
    const QString diskRevision = config.diskRevision();
    const bool diskChangedExternally = diskRevision != config.expectedDiskRevision();
    const bool savedPendingApply = !dirty && !m_savedFormRevision.isEmpty() &&
        diskRevision == m_savedFormRevision && diskRevision != config.runtimeRevision();
    const bool retirementPending = hasRetiredLocalSettings(config.snapshot().sourceDocument);
    if (m_dirtyLabel) {
        m_dirtyLabel->setText(diskChangedExternally ? tr("磁盘配置已在外部变化，请核对后重启上位机") :
            (dirty ? tr("有未保存修改") : (savedPendingApply ? tr("已保存待应用") : tr("无未保存修改"))));
        m_dirtyLabel->setStyleSheet((dirty || savedPendingApply || diskChangedExternally) ?
            "color:#936323; background:#fcf2df; border-radius:7px; padding:6px 10px;" :
            "color:#647a6c; background:#eaf0ec; border-radius:7px; padding:6px 10px;");
    }
    const bool hasBackup = !ConfigManager::instance().latestBackupFilePath().isEmpty();
    const bool canRestoreBackup = hasBackup && (dirty || !ConfigManager::instance().currentConfigMatchesLatestBackup());
    if (m_saveButton) {
        m_saveButton->setEnabled((dirty || retirementPending) && !diskChangedExternally);
    }
    if (m_applyButton) {
        m_applyButton->setEnabled((dirty || savedPendingApply) && !diskChangedExternally);
        m_applyButton->setText(savedPendingApply ? tr("应用已保存配置到本机") : tr("保存并应用"));
    }
    if (m_restoreBackupButton) {
        m_restoreBackupButton->setEnabled(canRestoreBackup);
    }
}

bool Maintenance::confirmLeaveIfDirty(QWidget *parentWidget)
{
    if (m_ipcManagement && m_ipcManagement->hasUnsavedChanges() &&
        QMessageBox::question(parentWidget ? parentWidget : ui->maintenancePage,
            tr("工控机参数尚未保存"), tr("工控机参数编辑尚未保存。离开页面后本次程序内仍保留编辑，退出程序会丢失。是否继续离开？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
    if (snapshotFingerprint(collectSnapshot()) == m_cleanFingerprint) {
        return true;
    }

    QMessageBox box(parentWidget ? parentWidget : (ui ? ui->maintenancePage : nullptr));
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("是否保存本次修改"));
    box.setText(tr("是否保存本次修改？"));
    QPushButton *discardButton = box.addButton(tr("放弃修改"), QMessageBox::DestructiveRole);
    QPushButton *saveButton = box.addButton(tr("保存本机"), QMessageBox::AcceptRole);
    QPushButton *applyButton = box.addButton(tr("保存并应用到本机"), QMessageBox::AcceptRole);
    QPushButton *cancelButton = box.addButton(tr("取消"), QMessageBox::RejectRole);
    box.setDefaultButton(applyButton);
    box.setEscapeButton(cancelButton);
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
    return QJsonObject{
        {QStringLiteral("preservedDocument"), snapshot.sourceDocument},
        {QStringLiteral("network"),
         QJsonObject{{QStringLiteral("websocketUrl"), snapshot.network.websocketUrl.trimmed()},
                     {QStringLiteral("statusReadUrl"), snapshot.network.statusReadUrl.trimmed()},
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
        {"manualControl", QJsonObject{{"maxLinearSpeed", snapshot.manualControl.maxLinearSpeed},{"maxAngularSpeed", snapshot.manualControl.maxAngularSpeed},{"manualMotionRepeatIntervalMs", snapshot.manualControl.manualMotionRepeatIntervalMs}}},
        {"poseSource", QJsonObject{{"enabled", snapshot.poseSource.enabled},{"baseUrl", snapshot.poseSource.baseUrl},{"requestTimeoutMs", snapshot.poseSource.requestTimeoutMs}}},
        {"tracking", QJsonObject{{"enabled", snapshot.tracking.enabled},{"baseUrl", snapshot.tracking.baseUrl},{"requestTimeoutMs", snapshot.tracking.requestTimeoutMs}}},
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

#include "ipcmanagementpanel.h"
#include "loggingmanager.h"
#include "parameterwheelguard.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <limits>

namespace {
class CompactDoubleSpinBox final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;
protected:
    QString textFromValue(double value) const override {
        QString valueText = QDoubleSpinBox::textFromValue(value);
        const auto point = locale().decimalPoint();
        if (valueText.contains(point)) {
            while (valueText.endsWith(QLatin1Char('0'))) valueText.chop(1);
            if (valueText.endsWith(point)) valueText.chop(point.size());
        }
        return valueText;
    }
};
QString revision(const QJsonValue &value) {
    return value.isString() ? value.toString() : value.isDouble() ? QString::number(value.toDouble(), 'g', 16) : QString();
}
QString responseMessage(const JsonHttpResult &result) {
    const QString message = result.object.value("message").toString();
    if (!message.isEmpty()) return message;
    if (result.status == 401) return QObject::tr("认证失败，请核对工控机与上位机的访问令牌。");
    if (result.status == 404) return QObject::tr("管理接口尚未安装，请先更新工控机管理服务。");
    if (result.error == "request_timeout") return QObject::tr("请求超时");
    if (result.error == "connection_failed") return QObject::tr("无法连接工控机管理服务");
    return result.error;
}
QLabel *text(const QString &value, QWidget *parent, bool caption = false) {
    auto *label = new QLabel(value, parent);
    label->setWordWrap(true);
    if (caption) label->setProperty("supportCaption", true);
    return label;
}
QString parameterGroup(const QJsonObject &field) {
    const auto group = field.value("group").toString().trimmed();
    if (!group.isEmpty()) return group;
    const auto key = field.value("key").toString();
    if (key.startsWith("pure_pursuit.")) return QObject::tr("路径跟踪与前视距离");
    if (key.startsWith("control.")) return QObject::tr("速度与加减速");
    if (key.startsWith("arrival.") || key.startsWith("turning.")) return QObject::tr("到点与转向完成条件");
    if (key.startsWith("server.")) return QObject::tr("通信与响应");
    if (key.startsWith("rtk.")) return QObject::tr("RTK 输入与安装几何");
    if (key.startsWith("imu.") || key.startsWith("fusion.")) return QObject::tr("IMU 与融合时序");
    return QObject::tr("其他运行参数");
}
QString parameterValueText(const QJsonValue &value, const QJsonObject &field) {
    const auto type = field.value("type").toString();
    QString result;
    if (type == "string" && value.isString()) result = value.toString().isEmpty() ? QObject::tr("（空）") : value.toString();
    else if ((type == "number" || type == "integer") && value.isDouble() && std::isfinite(value.toDouble()) &&
             (type != "integer" || value.toDouble() == std::floor(value.toDouble())))
        result = QString::number(value.toDouble(), 'g', 16);
    else return {};
    const auto unit = field.value("unit").toString();
    return unit.isEmpty() ? result : result + " " + unit;
}
bool usableOrigin(const QJsonObject &origin) {
    const auto saved = origin.value("saved").toObject();
    const auto latitude = saved.value("latitude"), longitude = saved.value("longitude");
    return origin.value("revision").isString() && !origin.value("revision").toString().isEmpty() &&
        latitude.isDouble() && longitude.isDouble() && std::isfinite(latitude.toDouble()) && std::isfinite(longitude.toDouble()) &&
        latitude.toDouble() >= -90 && latitude.toDouble() <= 90 && longitude.toDouble() >= -180 && longitude.toDouble() <= 180;
}
}

IpcManagementPanel::IpcManagementPanel(QWidget *parent) : QWidget(parent), m_http(this), m_snapshotHttp(this), m_timer(this) {
    setObjectName("ipcManagementPanel");
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(10);
    auto *heading = text(tr("工控机 · 服务与参数"), this);
    heading->setProperty("supportSectionTitle", true);
    root->addWidget(heading);
    heading->setToolTip(tr("所有操作均在工控机执行，修改前自动备份；保存与运行生效分别回读确认。"));
    m_connectionState = text(tr("尚未连接管理服务"), this, true);
    m_connectionState->setObjectName("ipcManagementConnection");
    root->addWidget(m_connectionState);
    m_pages = new QTabWidget(this);
    m_pages->setObjectName("ipcMaintenanceTabs");
    root->addWidget(m_pages, 1);
    auto *services = new QFrame(this);
    services->setProperty("supportCard", true);
    auto *serviceLayout = new QVBoxLayout(services);
    serviceLayout->setContentsMargins(18, 16, 18, 16);
    serviceLayout->setSpacing(16);
    auto *serviceHeading = text(tr("服务与进程"), services);
    serviceHeading->setProperty("supportSectionTitle", true);
    serviceLayout->addWidget(serviceHeading);
    auto *serviceSummary = new QFrame(services);
    serviceSummary->setObjectName("ipcServiceSummary");
    serviceSummary->setStyleSheet("QFrame#ipcServiceSummary { background:#f5f8f6; border:1px solid #e3ebe6; border-radius:10px; }");
    auto *summaryLayout = new QVBoxLayout(serviceSummary);
    summaryLayout->setContentsMargins(16, 12, 16, 12);
    summaryLayout->setSpacing(12);
    auto *serviceGuide = text(tr("融合 → 导航　·　相机独立启停"), serviceSummary);
    serviceGuide->setObjectName("ipcServiceGuide");
    serviceGuide->setAlignment(Qt::AlignCenter);
    serviceGuide->setContentsMargins(0, 4, 0, 4);
    serviceGuide->setToolTip(tr("启动融合前会确认本次基准站坐标；停止融合前先停导航。服务运行后仍需等待定位和任务就绪，启动服务不会执行任务。"));
    summaryLayout->addWidget(serviceGuide);
    auto *divider = new QFrame(serviceSummary);
    divider->setFrameShape(QFrame::HLine);
    divider->setFixedHeight(1);
    divider->setStyleSheet("background:#e0e9e3; border:none;");
    summaryLayout->addWidget(divider);
    m_originState = text(tr("基准站坐标 · 等待读取工控机"), serviceSummary);
    m_originState->setObjectName("ipcOriginState");
    m_originState->setAlignment(Qt::AlignCenter);
    m_originState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_originState->setContentsMargins(0, 4, 0, 4);
    summaryLayout->addWidget(m_originState);
    serviceLayout->addWidget(serviceSummary);
    auto *releaseRow = new QHBoxLayout;
    auto *release = new QPushButton(tr("释放任务操作权"), services);
    release->setObjectName("ipcReleaseTaskSession");
    release->setToolTip(tr("仅当提示操作权被占用时使用。任务结束后释放，再等待操作权到期。"));
    releaseRow->addStretch();
    releaseRow->addWidget(release);
    connect(release, &QPushButton::clicked, this, &IpcManagementPanel::releaseTaskSessionRequested);
    for (const auto &entry : {qMakePair(QString("fusion"), tr("融合服务")),
                              qMakePair(QString("tracking"), tr("导航服务")),
                              qMakePair(QString("camera"), tr("相机服务"))}) {
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        auto *name = text(entry.second, services);
        name->setMinimumWidth(90);
        row->addWidget(name);
        auto *state = text(tr("● 未知"), services);
        state->setAlignment(Qt::AlignCenter);
        state->setObjectName("ipcServiceState_" + entry.first);
        m_serviceStates.insert(entry.first, state);
        row->addWidget(state, 1);
        for (const auto &action : {qMakePair(QString("start"), tr("启动")), qMakePair(QString("restart"), tr("重启")), qMakePair(QString("stop"), tr("停止"))}) {
            auto *button = new QPushButton(action.second, services);
            button->setObjectName("ipcService_" + entry.first + "_" + action.first);
            button->setProperty("serviceId", entry.first);
            button->setProperty("serviceAction", action.first);
            button->setMinimumWidth(72);
            button->setCursor(Qt::PointingHandCursor);
            m_serviceButtons.append(button);
            row->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, id=entry.first, verb=action.first] { operateService(id, verb); });
        }
        serviceLayout->addLayout(row);
    }
    m_serviceHint = text(tr("先连接工控机以读取服务状态。"), services, true);
    m_serviceHint->setObjectName("ipcServiceCondition");
    m_serviceHint->setContentsMargins(0, 4, 0, 4);
    serviceLayout->addWidget(m_serviceHint);
    serviceLayout->addLayout(releaseRow);
    addMaintenancePage(tr("服务管理"), services);
    auto *parameters = new QFrame(this);
    parameters->setProperty("supportCard", true);
    auto *parameterLayout = new QVBoxLayout(parameters);
    parameterLayout->setContentsMargins(18, 16, 18, 16);
    parameterLayout->setSpacing(12);
    auto *parameterHeading = text(tr("运行参数"), parameters);
    parameterHeading->setProperty("supportSectionTitle", true);
    parameterLayout->addWidget(parameterHeading);
    parameterHeading->setToolTip(tr("先读取再修改。仅保存不会改变运行值；保存并应用会在空闲时重启服务，并读回核验。"));
    auto *readRow = new QHBoxLayout;
    m_component = new QComboBox(parameters);
    m_component->setObjectName("ipcParameterComponent");
    m_component->setMaximumWidth(300);
    m_component->addItem(tr("融合参数"), "fusion");
    m_component->addItem(tr("导航参数"), "tracking");
    readRow->addWidget(m_component, 1);
    m_read = new QPushButton(tr("读取工控机参数"), parameters);
    m_read->setObjectName("ipcParametersRead");
    readRow->addWidget(m_read);
    parameterLayout->addLayout(readRow);
    m_parameterState = text(tr("尚未读取参数"), parameters, true);
    m_parameterState->setObjectName("ipcParameterState");
    parameterLayout->addWidget(m_parameterState);
    m_parameterTarget = text(tr("配置位置 · 等待读取工控机"), parameters, true);
    m_parameterTarget->setObjectName("ipcParameterTarget");
    m_parameterTarget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    parameterLayout->addWidget(m_parameterTarget);
    auto *parameterGuide = text(tr("编辑框用于修改已读取的保存值；下方分别显示工控机已保存值和已核验运行值。仅保存后，运行值可能仍是上一版本。"), parameters, true);
    parameterGuide->setObjectName("ipcParameterGuide");
    parameterLayout->addWidget(parameterGuide);
    m_form = new QFormLayout;
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_form->setVerticalSpacing(12);
    m_form->setHorizontalSpacing(28);
    m_form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    parameterLayout->addLayout(m_form);
    addMaintenancePage(tr("融合与导航参数"), parameters);
    auto *parameterPage = m_pages->widget(m_pages->count() - 1);
    auto *parameterActions = new QFrame(this);
    parameterActions->setObjectName("ipcParameterActions");
    parameterActions->setProperty("supportCard", true);
    auto *actionBox = new QVBoxLayout(parameterActions);
    actionBox->setContentsMargins(14, 10, 14, 12);
    actionBox->setSpacing(8);
    m_parameterEditState = text({}, parameterActions, true);
    m_parameterEditState->setObjectName("ipcParameterEditState");
    actionBox->addWidget(m_parameterEditState);
    auto *actions = new QHBoxLayout;
    m_save = new QPushButton(tr("仅保存"), parameterActions);
    m_save->setToolTip(tr("只保存工控机配置，运行值保持原状。"));
    m_save->setObjectName("ipcParametersSave");
    m_saveApply = new QPushButton(tr("保存并应用"), parameterActions);
    m_saveApply->setObjectName("ipcParametersSaveApply");
    m_saveApply->setProperty("supportPrimary", true);
    m_saveApply->setToolTip(tr("保存配置并重启对应服务，运行值读回核验后才显示已生效。"));
    m_applySaved = new QPushButton(tr("应用已保存配置"), parameterActions);
    m_applySaved->setObjectName("ipcParametersApplySaved");
    m_applySaved->setToolTip(tr("应用工控机磁盘中已保存的配置，不提交当前未保存编辑。"));
    actions->addWidget(m_save);
    actions->addWidget(m_saveApply);
    actions->addWidget(m_applySaved);
    actions->addStretch();
    actionBox->addLayout(actions);
    root->addWidget(parameterActions);
    const auto updateParameterActionsVisibility = [this, parameterPage, parameterActions] {
        parameterActions->setVisible(m_pages->currentWidget() == parameterPage);
    };
    connect(m_pages, &QTabWidget::currentChanged, this, updateParameterActionsVisibility);
    updateParameterActionsVisibility();
    auto *backups = new QFrame(this);
    backups->setProperty("supportCard", true);
    auto *backupLayout = new QVBoxLayout(backups);
    backupLayout->setContentsMargins(18, 16, 18, 16);
    auto *backupHeading = text(tr("工控机配置备份"), backups);
    backupHeading->setProperty("supportSectionTitle", true);
    backupLayout->addWidget(backupHeading);
    auto *backupGuide = text(tr("修改前自动备份至工控机 backups/config/。"), backups, true);
    backupGuide->setToolTip(tr("备份完整配置并保留认证信息，仅授权用户可读取。此操作不启停服务，本机配置备份位于本机维护页。"));
    backupLayout->addWidget(backupGuide);
    m_backupState = text(tr("尚未读取备份记录"), backups, true);
    m_backupState->setObjectName("ipcBackupState");
    m_backupState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    backupLayout->addWidget(m_backupState);
    auto *backupActions = new QHBoxLayout;
    m_backupCreate = new QPushButton(tr("立即备份工控机配置"), backups);
    m_backupCreate->setObjectName("ipcBackupCreate");
    m_backupRefresh = new QPushButton(tr("刷新备份记录"), backups);
    m_backupRefresh->setObjectName("ipcBackupRefresh");
    backupActions->addWidget(m_backupCreate);
    backupActions->addWidget(m_backupRefresh);
    backupActions->addStretch();
    backupLayout->addLayout(backupActions);
    addMaintenancePage(tr("配置备份"), backups);
    m_result = text(tr("等待操作"), this);
    m_result->setObjectName("ipcManagementResult");
    root->addWidget(m_result);
    ParameterWheelGuard::install(this);
    connect(m_read, &QPushButton::clicked, this, &IpcManagementPanel::readParameters);
    connect(m_backupCreate, &QPushButton::clicked, this, [this] {
        if (m_online && m_operationId.isEmpty()) beginOperation("/management/backups", {{"operation", "create"}});
    });
    connect(m_backupRefresh, &QPushButton::clicked, this, &IpcManagementPanel::refreshBackups);
    connect(m_save, &QPushButton::clicked, this, [this] { writeParameters(false); });
    connect(m_saveApply, &QPushButton::clicked, this, [this] { writeParameters(true); });
    connect(m_applySaved, &QPushButton::clicked, this, [this] { writeParameters(true, true); });
    connect(m_component, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loadedComponent.isEmpty() && m_component->currentData() != m_loadedComponent)
            m_parameterState->setText(tr("点击读取所选组件。当前编辑暂时保留，未写入工控机。"));
        updateButtons();
    });
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &IpcManagementPanel::refresh);
    updateButtons();
}
void IpcManagementPanel::addMaintenancePage(const QString &title, QWidget *content) {
    auto *scroll = new QScrollArea(m_pages);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *page = new QWidget(scroll);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->addWidget(content);
    layout->addStretch();
    scroll->setWidget(page);
    m_pages->addTab(scroll, title);
}
IpcManagementPanel::~IpcManagementPanel() { m_timer.stop(); m_http.cancelAll(); m_snapshotHttp.cancelAll(); }
void IpcManagementPanel::setConnectionProvider(std::function<IpcManagementConnection()> provider) { m_provider = std::move(provider); }
void IpcManagementPanel::refreshOriginSnapshot() {
    if (isVisible() || !m_operationId.isEmpty() || m_operationRefreshPending || m_readAfterRefresh) refresh();
    else refreshOriginOnly();
}
void IpcManagementPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    m_snapshotHttp.cancelAll();
    refresh();
}
void IpcManagementPanel::hideEvent(QHideEvent *event) {
    if (m_originDialog) m_originDialog->reject();
    m_timer.stop();
    m_http.cancelAll();
    m_snapshotHttp.cancelAll();
    ++m_backupGeneration;
    m_backupsLoaded = false;
    m_readAfterRefresh = false;
    updateButtons();
    QWidget::hideEvent(event);
}
bool IpcManagementPanel::configureConnection() {
    if (!m_provider) return false;
    const auto connection = m_provider();
    if (connection.baseUrl != m_connection.baseUrl || connection.token != m_connection.token || connection.timeoutMs != m_connection.timeoutMs) {
        if (!m_operationId.isEmpty()) {
            setResult(tr("管理连接已更改，之前的操作结果需在原工控机核实；未重发操作。"), true);
            m_operationId.clear();
        }
        m_connection = connection;
        m_http.configure(connection.baseUrl, connection.token, connection.timeoutMs);
        m_snapshotHttp.configure(connection.baseUrl, connection.token, connection.timeoutMs);
        m_online = false;
        updateParameterReadbacks({});
        m_operationRefreshPending = false;
        m_stale = true;
        m_bootId.clear();
        m_legacyOriginEndpoint = false;
        m_backupsLoaded = false;
        ++m_backupGeneration;
        m_backupState->setText(tr("工控机连接已更改，备份记录待重新读取。"));
        emit originSnapshotChanged({});
    }
    return m_http.configured();
}
void IpcManagementPanel::refreshOriginOnly() {
    if (!configureConnection()) { emit originSnapshotChanged({}); return; }
    if (m_originDialog || m_http.busy("management") || !m_operationId.isEmpty() || m_snapshotHttp.busy("snapshot")) return;
    const bool legacy = m_legacyOriginEndpoint;
    m_snapshotHttp.request("snapshot", "GET", legacy ? "/management" : "/management/origin", {}, {}, {},
        [this, legacy](const JsonHttpResult &result) {
            if (!legacy && result.status == 404) {
                m_legacyOriginEndpoint = true;
                if (!isVisible()) refreshOriginOnly();
                return;
            }
            const bool valid = result.ok() && result.object.value("apiVersion").toInt() == 1 &&
                !result.object.value("bootId").toString().isEmpty();
            // This display-only read never enables maintenance writes or reuses
            // a previous full snapshot for write admission.
            emit originSnapshotChanged(valid ? result.object.value("origin").toObject() : QJsonObject{});
        }, legacy ? 128 * 1024 : 16 * 1024);
}
void IpcManagementPanel::refresh() {
    if (!configureConnection()) {
        m_online = false;
        updateParameterReadbacks({});
        m_readAfterRefresh = false;
        m_connectionState->setText(tr("管理服务地址无效，请先维护工控机连接。"));
        m_originState->setText(tr("本次基准站坐标 · 未连接工控机"));
        emit originSnapshotChanged({});
        updateButtons();
        return;
    }
    if (m_originDialog) { if (isVisible()) m_timer.start(2000); return; }
    if (m_http.busy("management")) return;
    if (!m_operationId.isEmpty()) { pollOperation(); return; }
    if (m_snapshotHttp.busy("snapshot")) return;
    // Background reads must not disable focused controls: Qt would move focus
    // and the enclosing scroll area would follow it. Actual operations retain
    // their separate busy state and invalidate any older snapshot before writing.
    m_snapshotHttp.request("snapshot", "GET", "/management", {}, {}, {}, [this](const JsonHttpResult &result) {
        m_online = result.ok() && result.object.value("apiVersion").toInt() == 1;
        if (m_online) {
            m_backoffMs = 2000;
            renderSnapshot(result.object);
            m_operationRefreshPending = false;
            if (m_readAfterRefresh) { loadFields(componentSnapshot()); m_readAfterRefresh = false; }
            if (!m_backupsLoaded) refreshBackups();
        } else {
            updateParameterReadbacks({});
            m_connectionState->setText(tr("● 离线 · %1").arg(responseMessage(result)));
            m_connectionState->setStyleSheet("color:#b02d2d");
            m_originState->setText(tr("本次基准站坐标 · 管理服务离线，待重新读取"));
            m_backupsLoaded = false;
            m_backupState->setText(tr("管理服务离线，备份记录待重新读取。"));
            emit originSnapshotChanged({});
            for (auto *label : m_serviceStates) { label->setText(tr("● 未知 · 管理服务离线")); label->setStyleSheet("color:#738278"); }
            m_backoffMs = qMin(16000, m_backoffMs * 2);
            if (m_readAfterRefresh) { setResult(responseMessage(result), true); m_readAfterRefresh = false; }
        }
        updateButtons();
        if (isVisible()) m_timer.start(m_backoffMs);
    }, 128 * 1024);
    updateButtons();
}
QJsonObject IpcManagementPanel::componentSnapshot() const {
    const QString id = m_component->currentData().toString();
    for (const auto &value : m_snapshot.value("parameters").toArray()) {
        const auto component = value.toObject();
        if (component.value("component") == id) return component;
    }
    return {};
}
void IpcManagementPanel::renderSnapshot(const QJsonObject &snapshot) {
    const QString boot = snapshot.value("bootId").toString();
    if (!m_bootId.isEmpty() && boot != m_bootId && !m_loadedRevision.isEmpty()) m_stale = true;
    m_bootId = boot;
    m_snapshot = snapshot;
    const auto origin = snapshot.value("origin").toObject();
    emit originSnapshotChanged(origin);
    if (usableOrigin(origin)) {
        const auto saved = origin.value("saved").toObject();
        const auto originMessage = origin.value("message").toString(tr("启动融合前确认本次坐标，融合与导航共用此原点。"));
        m_originState->setText(tr("<table width='100%' cellspacing='0' cellpadding='3'><tr>"
            "<td width='50%' align='center'><span style='color:#718278'>纬度</span><br/>"
            "<span style='font-size:14px;font-weight:600'>%1°</span></td>"
            "<td width='50%' align='center'><span style='color:#718278'>经度</span><br/>"
            "<span style='font-size:14px;font-weight:600'>%2°</span></td>"
            "</tr></table>").arg(
            QString::number(saved.value("latitude").toDouble(), 'f', 9),
            QString::number(saved.value("longitude").toDouble(), 'f', 9)) +
            (origin.value("consistent").toBool() ? QString() :
                "<p align='center' style='margin-top:10px'>" + originMessage.toHtmlEscaped() + "</p>"));
        m_originState->setToolTip(originMessage);
        m_originState->setStyleSheet(origin.value("consistent").toBool() ? "color:#385346" : "color:#95610f");
    } else {
        m_originState->setText(tr("统一基准站坐标不可用，请更新工控机管理服务后读取；不使用本地旧坐标替代。"));
        m_originState->setStyleSheet("color:#b02d2d");
    }
    m_connectionState->setText(tr("● 已连接 · %1").arg(m_connection.baseUrl.host()));
    m_connectionState->setToolTip(snapshot.value("projectRoot").toString());
    m_connectionState->setStyleSheet("color:#216b46");
    for (const auto &value : snapshot.value("services").toArray()) {
        const auto service = value.toObject();
        auto *label = m_serviceStates.value(service.value("id").toString());
        if (!label) continue;
        const QString state = service.value("activeState").toString();
        const QString display = state == "active" ? tr("运行中") : state == "inactive" ? tr("已停止") : state == "failed" ? tr("异常") : state == "activating" ? tr("启动中") : state == "deactivating" ? tr("停止中") : tr("未知");
        label->setText(tr("● %1").arg(display));
        label->setToolTip(service.value("message").toString());
        label->setStyleSheet(state == "active" ? "color:#216b46" : state == "failed" ? "color:#b02d2d" : "color:#738278");
    }
    const auto component = componentSnapshot();
    const auto configPath = component.value("configPath").toString();
    const auto projectRoot = snapshot.value("projectRoot").toString();
    m_parameterTarget->setText(tr("修改对象 · 工控机 / %1\n配置文件 · %2").arg(
        snapshot.value("profile").toString(),
        configPath.isEmpty() ? tr("当前服务未提供路径，请更新管理服务") :
        configPath.startsWith('/') ? configPath : projectRoot + "/" + configPath));
    if (snapshot.value("configuredProfile").toString() != snapshot.value("profile").toString())
        m_parameterTarget->setText(m_parameterTarget->text() + tr("\n保存的部署模式与管理进程不同，请先核对部署并重启管理服务。"));
    if (m_loadedComponent == m_component->currentData() && !m_loadedRevision.isEmpty()) {
        if (revision(component.value("revision")) != m_loadedRevision) m_stale = true;
        if (m_stale) m_parameterState->setText(tr("工控机参数或管理进程已变化。编辑内容已保留，请重新读取后再保存。"));
        else {
            const auto state = component.value("state").toString();
            const QString status = state == "applied" ? tr("已核验生效") : state == "pending" ? tr("已保存，待应用") : state == "failed" ? tr("应用失败") : tr("运行值尚未核验");
            m_parameterState->setText(tr("%1%2 · 配置版本 %3").arg(status,
                hasUnsavedChanges() ? tr(" · 有未保存修改") : QString(), m_loadedRevision.left(12)) +
                (state == "failed" ? "\n" + component.value("message").toString() : QString()));
            m_parameterState->setToolTip(tr("配置版本：%1\n%2").arg(m_loadedRevision, component.value("message").toString()));
        }
        updateParameterReadbacks(component);
    }
}
void IpcManagementPanel::updateParameterReadbacks(const QJsonObject &component) {
    const auto appliedRevision = revision(component.value("appliedRevision"));
    const auto state = component.value("state").toString();
    const bool verified = m_online && !appliedRevision.isEmpty() &&
        (state == "applied" || state == "pending" || state == "failed");
    QHash<QString, QJsonObject> fields;
    for (const auto &value : component.value("fields").toArray()) {
        const auto field = value.toObject();
        fields.insert(field.value("key").toString(), field);
    }
    for (auto it = m_parameterReadbacks.cbegin(); it != m_parameterReadbacks.cend(); ++it) {
        const auto field = fields.value(it.key());
        const auto saved = m_online ? parameterValueText(field.value("value"), field) :
            it.value()->property("lastSavedValueText").toString();
        if (m_online) it.value()->setProperty("lastSavedValueText", saved);
        const auto applied = verified ? parameterValueText(field.value("appliedValue"), field) : QString();
        it.value()->setText(tr("已保存：%1  ·  运行值：%2").arg(
            saved.isEmpty() ? tr("不可用") : saved, !m_online ? tr("尚未核验（管理服务离线）") :
            applied.isEmpty() ? tr("尚未核验") : applied));
        it.value()->setToolTip(!m_online ? tr("管理服务离线，无法确认当前服务进程的运行值；未保存编辑已保留。") :
            applied.isEmpty() ? tr("尚无当前服务进程的运行值读回核验，不能用保存值代替。") :
            tr("运行值已由工控机读回核验，应用版本：%1").arg(appliedRevision));
    }
}
void IpcManagementPanel::readParameters() {
    if (m_readAfterRefresh || !m_operationId.isEmpty() || m_originDialog) return;
    if (hasUnsavedChanges() && QMessageBox::question(this, tr("重新读取参数"), tr("重新读取会替换当前未保存的工控机参数编辑，是否继续？")) != QMessageBox::Yes) return;
    m_readAfterRefresh = true;
    updateButtons();
    refresh();
}
void IpcManagementPanel::loadFields(const QJsonObject &component) {
    const auto fields = component.value("fields").toArray();
    if (component.isEmpty() || revision(component.value("revision")).isEmpty() || fields.isEmpty() || fields.size() > 32) {
        setResult(tr("工控机未提供可维护的参数白名单，无法编辑。"), true); return;
    }
    for (const auto &value : fields) {
        const auto field = value.toObject();
        const auto type = field.value("type").toString();
        if (field.value("key").toString().isEmpty() || (type != "number" && type != "integer" && type != "string") ||
            (type != "string" && (!field.value("value").isDouble() || !std::isfinite(field.value("value").toDouble()) ||
              !field.value("minimum").isDouble() || !field.value("maximum").isDouble() ||
              !std::isfinite(field.value("minimum").toDouble()) || !std::isfinite(field.value("maximum").toDouble()) ||
              field.value("minimum").toDouble() > field.value("maximum").toDouble() ||
              field.value("value").toDouble() < field.value("minimum").toDouble() ||
              field.value("value").toDouble() > field.value("maximum").toDouble())) ||
            (type == "integer" && (field.value("value").toDouble() != std::floor(field.value("value").toDouble()) ||
              field.value("minimum").toDouble() < std::numeric_limits<int>::min() ||
              field.value("maximum").toDouble() > std::numeric_limits<int>::max())) ||
            (type == "string" && (!field.value("value").isString() || field.value("value").toString().size() > 256))) {
            setResult(tr("工控机参数定义无效，拒绝展示不完整配置。"), true); return;
        }
    }
    m_loading = true;
    while (m_form->rowCount()) m_form->removeRow(0);
    m_fields.clear(); m_parameterReadbacks.clear(); m_originalValues = {};
    QStringList groups;
    QHash<QString, QJsonArray> groupedFields;
    for (const auto &value : fields) {
        const auto field = value.toObject();
        const auto group = parameterGroup(field);
        if (!groupedFields.contains(group)) groups.append(group);
        groupedFields[group].append(field);
    }
    for (const auto &group : groups) {
        auto *heading = text(group, this);
        heading->setTextFormat(Qt::PlainText);
        heading->setObjectName("ipcParameterGroup_" + QString::number(m_form->rowCount()));
        heading->setProperty("supportSectionTitle", true);
        heading->setContentsMargins(0, m_form->rowCount() ? 12 : 0, 0, 2);
        m_form->addRow(heading);
        for (const auto &value : groupedFields.value(group)) {
            const auto field = value.toObject();
            const QString key = field.value("key").toString();
            const QString type = field.value("type").toString();
            QWidget *editor = nullptr;
            if (type == "string") {
                auto *line = new QLineEdit(field.value("value").toString(), this);
                line->setMaxLength(256);
                connect(line, &QLineEdit::textEdited, this, [this] { updateButtons(); });
                editor = line;
            } else if (type == "integer") {
                auto *spin = new QSpinBox(this);
                spin->setRange(qMax(double(std::numeric_limits<int>::min()), field.value("minimum").toDouble()), qMin(double(std::numeric_limits<int>::max()), field.value("maximum").toDouble()));
                spin->setValue(field.value("value").toInt());
                spin->setSuffix(field.value("unit").toString().isEmpty() ? QString() : " " + field.value("unit").toString());
                connect(spin, &QSpinBox::valueChanged, this, [this] { updateButtons(); });
                editor = spin;
            } else {
                auto *spin = new CompactDoubleSpinBox(this);
                spin->setDecimals(9);
                spin->setRange(field.value("minimum").toDouble(), field.value("maximum").toDouble());
                const double span = spin->maximum() - spin->minimum();
                spin->setSingleStep(span <= 10 ? 0.01 : span <= 100 ? 0.1 : 1.0);
                spin->setValue(field.value("value").toDouble());
                spin->setSuffix(field.value("unit").toString().isEmpty() ? QString() : " " + field.value("unit").toString());
                connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { updateButtons(); });
                editor = spin;
            }
            editor->setObjectName("ipcParameter_" + key);
            editor->setToolTip(field.value("description").toString(key));
            m_fields.insert(key, editor);
            m_originalValues.insert(key, field.value("value"));
            editor->setProperty("originalDisplayed", type == "number" ? QJsonValue(qobject_cast<QDoubleSpinBox *>(editor)->value()).toVariant() : QVariant());
            auto *label = text(field.value("label").toString(key), this);
            label->setMinimumWidth(148);
            label->setContentsMargins(0, 8, 0, 0);
            label->setToolTip(editor->toolTip());
            label->setBuddy(editor);
            auto *details = new QWidget(this);
            auto *detailLayout = new QVBoxLayout(details);
            detailLayout->setContentsMargins(0, 0, 0, 0);
            detailLayout->setSpacing(5);
            detailLayout->addWidget(editor);
            auto *readback = text({}, details, true);
            readback->setTextFormat(Qt::PlainText);
            readback->setObjectName("ipcParameterReadback_" + key);
            readback->setTextInteractionFlags(Qt::TextSelectableByMouse);
            detailLayout->addWidget(readback);
            m_parameterReadbacks.insert(key, readback);
            const auto description = field.value("description").toString().trimmed();
            if (!description.isEmpty()) {
                auto *explanation = text(description, details, true);
                explanation->setTextFormat(Qt::PlainText);
                explanation->setObjectName("ipcParameterDescription_" + key);
                detailLayout->addWidget(explanation);
            }
            m_form->addRow(label, details);
        }
    }
    m_loadedComponent = component.value("component").toString();
    m_loadedRevision = revision(component.value("revision"));
    m_stale = false; m_loading = false;
    renderSnapshot(m_snapshot);
    setResult(tr("已读取工控机参数。修改后主动保存或应用。"));
    updateButtons();
}
QJsonObject IpcManagementPanel::currentValues() const {
    QJsonObject values;
    for (auto it=m_fields.cbegin(); it!=m_fields.cend(); ++it) {
        if (auto *line = qobject_cast<QLineEdit *>(it.value())) values.insert(it.key(), line->text());
        else if (auto *spin = qobject_cast<QSpinBox *>(it.value())) values.insert(it.key(), spin->value());
        else if (auto *spin = qobject_cast<QDoubleSpinBox *>(it.value())) {
            // Do not round unchanged remote values to the display precision.
            values.insert(it.key(), spin->value() == spin->property("originalDisplayed").toDouble() ? m_originalValues.value(it.key()) : QJsonValue(spin->value()));
        }
    }
    return values;
}
bool IpcManagementPanel::hasUnsavedChanges() const { return !m_loading && !m_originalValues.isEmpty() && currentValues() != m_originalValues; }
void IpcManagementPanel::writeParameters(bool apply, bool savedOnly) {
    if (!m_online || m_stale || m_loadedRevision.isEmpty() || m_loadedComponent != m_component->currentData() || !m_operationId.isEmpty()) return;
    QJsonObject body{{"expectedRevision", m_loadedRevision}};
    QString path = "/management/parameters/" + m_loadedComponent;
    if (savedOnly) path += "/apply";
    else { body.insert("values", currentValues()); body.insert("apply", apply); }
    if (apply && m_loadedComponent == "fusion") {
        const auto condition = serviceCondition("fusion", "restart");
        if (!condition.isEmpty()) { setResult(condition, true); return; }
        confirmFusionOrigin([this, path, body](QJsonObject originBody) mutable {
            for (auto it = originBody.begin(); it != originBody.end(); ++it) body.insert(it.key(), it.value());
            beginOperation(path, body);
        });
    } else beginOperation(path, body);
}
void IpcManagementPanel::operateService(const QString &id, const QString &action) {
    if (!m_online || !m_operationId.isEmpty() || m_operationRefreshPending) return;
    const auto condition = serviceCondition(id, action);
    if (!condition.isEmpty()) { setResult(condition, true); return; }
    const QString path = "/management/services/" + id;
    if (id == "fusion" && action != "stop") {
        confirmFusionOrigin([this, path, action](QJsonObject body) {
            body.insert("action", action);
            beginOperation(path, body);
        });
    } else beginOperation(path, {{"action", action}});
}
QJsonObject IpcManagementPanel::serviceSnapshot(const QString &id) const {
    for (const auto &value : m_snapshot.value("services").toArray())
        if (value.toObject().value("id") == id) return value.toObject();
    return {};
}
QString IpcManagementPanel::serviceCondition(const QString &id, const QString &action) const {
    const auto tracking = serviceSnapshot("tracking");
    const auto fusion = serviceSnapshot("fusion");
    const auto trackingState = tracking.value("activeState").toString();
    if (id == "fusion" && action != "start" && trackingState != "inactive" && trackingState != "failed")
        return tr("请先停止导航服务，再停止、重启或应用融合配置。相机服务可独立运行。");
    if (id == "fusion" && action != "stop" && !usableOrigin(m_snapshot.value("origin").toObject()))
        return tr("尚未读到工控机统一基准站坐标，请更新管理服务并重新读取后启动融合。");
    if (id == "tracking" && action != "stop") {
        if (fusion.value("activeState") != "active") return tr("请先确认本次基准站坐标并启动融合服务，再启动导航。无需等待导航服务来启动融合。");
        if (!usableOrigin(m_snapshot.value("origin").toObject()) || !m_snapshot.value("origin").toObject().value("consistent").toBool())
            return tr("融合与导航的基准站坐标尚未确认一致，请先停止导航，再通过融合启动确认统一坐标。");
        const auto expected = m_snapshot.value("configuredProfile").toString(m_snapshot.value("profile").toString());
        const auto actual = fusion.value("actualProfile").toString();
        if (expected.isEmpty() || actual != expected) return tr("融合服务的实际运行模式尚未确认或与保存配置不一致，请先停止导航并重启融合，再启动导航。");
    }
    return {};
}
void IpcManagementPanel::confirmFusionOrigin(std::function<void(QJsonObject)> confirmed) {
    if (m_originDialog || !m_online || m_readAfterRefresh || m_http.busy("management") || !m_operationId.isEmpty()) return;
    const auto origin = m_snapshot.value("origin").toObject();
    if (!usableOrigin(origin)) { setResult(tr("工控机未提供统一基准站坐标，请先更新管理服务。"), true); return; }
    const auto saved = origin.value("saved").toObject();
    const auto originRevision = origin.value("revision").toString();
    const auto bootId = m_bootId;
    const auto connection = m_connection;
    m_snapshotHttp.cancelAll();
    m_timer.stop();
    auto *dialog = new QDialog(this);
    dialog->setObjectName("ipcFusionOriginDialog");
    dialog->setProperty("supportPage", true);
    dialog->setWindowTitle(tr("确认本次基准站坐标"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setMinimumWidth(460);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    auto *heading = text(tr("本次作业的统一坐标原点"), dialog);
    heading->setProperty("supportSectionTitle", true);
    layout->addWidget(heading);
    layout->addWidget(text(tr("确认后再启动融合。保留原值即可沿用；修改后融合与导航将共同使用新坐标。"), dialog, true));
    auto *form = new QFormLayout;
    form->setSpacing(12);
    auto createCoordinate = [dialog](const char *name, double value, double low, double high) {
        auto *editor = new QDoubleSpinBox(dialog);
        editor->setObjectName(name);
        editor->setDecimals(9);
        editor->setRange(low, high);
        editor->setSingleStep(0.0000001);
        editor->setSuffix(QString::fromUtf8("°"));
        editor->setValue(value);
        editor->setMinimumWidth(240);
        return editor;
    };
    auto *latitude = createCoordinate("ipcOriginLatitude", saved.value("latitude").toDouble(), -90, 90);
    auto *longitude = createCoordinate("ipcOriginLongitude", saved.value("longitude").toDouble(), -180, 180);
    form->addRow(tr("纬度（北纬为正）"), latitude);
    form->addRow(tr("经度（东经为正）"), longitude);
    layout->addLayout(form);
    layout->addWidget(text(tr("高度固定为 0 m，与当前平面导航配置一致。取消不会保存坐标或启动服务。"), dialog, true));
    auto *changeNotice = text(QString(), dialog, true);
    changeNotice->setObjectName("ipcOriginChangeNotice");
    layout->addWidget(changeNotice);
    const double displayedLatitude = latitude->value(), displayedLongitude = longitude->value();
    auto updateNotice = [=] {
        const bool changed = latitude->value() != displayedLatitude || longitude->value() != displayedLongitude || saved.value("altitude").toDouble() != 0;
        changeNotice->setText(changed ? tr("坐标已修改：旧地图和旧任务将失效，需重新绑定或重建后再作业。") :
            origin.value("consistent").toBool() ? tr("沿用工控机保存坐标；服务启动后仍需等待有效定位和实测反馈就绪。") :
            tr("现有配置不一致：确认后将统一融合与导航原点；旧地图和任务需核对坐标版本。"));
        changeNotice->setStyleSheet(changed || !origin.value("consistent").toBool() ? "color:#95610f" : "color:#385346");
    };
    connect(latitude, &QDoubleSpinBox::valueChanged, dialog, updateNotice);
    connect(longitude, &QDoubleSpinBox::valueChanged, dialog, updateNotice);
    updateNotice();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, dialog);
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    auto *confirm = buttons->button(QDialogButtonBox::Ok);
    confirm->setText(tr("确认坐标并继续"));
    confirm->setObjectName("ipcOriginConfirm");
    confirm->setProperty("supportPrimary", true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, dialog, latitude, longitude, displayedLatitude, displayedLongitude,
        saved, originRevision, bootId, connection, confirmed=std::move(confirmed)](int result) {
        if (m_originDialog == dialog) m_originDialog.clear();
        updateButtons();
        if (isVisible()) m_timer.start(2000);
        if (result != QDialog::Accepted) { setResult(tr("已取消，基准站坐标和服务状态未更改。")); return; }
        if (!m_online || m_bootId != bootId || m_snapshot.value("origin").toObject().value("revision") != originRevision ||
            !m_provider || m_provider().baseUrl != connection.baseUrl || m_provider().token != connection.token) {
            setResult(tr("工控机连接或原点配置已变化，本次确认已取消。请重新读取后确认坐标，未发送启动请求。"), true);
            return;
        }
        // Preserve all original coordinate digits when the displayed value was
        // left untouched; display precision must not create an origin change.
        const auto latitudeValue = latitude->value() == displayedLatitude ? saved.value("latitude") : QJsonValue(latitude->value());
        const auto longitudeValue = longitude->value() == displayedLongitude ? saved.value("longitude") : QJsonValue(longitude->value());
        confirmed({{"origin", QJsonObject{{"latitude", latitudeValue}, {"longitude", longitudeValue}, {"altitude", 0}}},
                   {"expectedOriginRevision", originRevision}});
    });
    m_originDialog = dialog;
    updateButtons();
    dialog->open();
}
void IpcManagementPanel::beginOperation(const QString &path, QJsonObject body) {
    if (m_http.busy("management") || !m_operationId.isEmpty() || m_originDialog || m_readAfterRefresh || m_operationRefreshPending) return;
    m_snapshotHttp.cancelAll();
    ++m_backupGeneration;
    m_backupsLoaded = false;
    m_operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_parameterOperation = path.startsWith("/management/parameters/");
    body.insert("requestId", m_operationId);
    m_operationAge.start();
    m_timer.stop();
    const QString operationId = m_operationId;
    const bool submitted = m_http.request("management", "POST", path, body, {}, {}, [this, operationId](const JsonHttpResult &result) {
        if (m_operationId != operationId) return;
        if (!result.ok() && result.status >= 400) {
            setResult(tr("操作被拒绝：%1").arg(responseMessage(result)), true);
            LoggingManager::audit("ipc.operation", "rejected", {{"reason", responseMessage(result)}});
            m_operationId.clear(); updateButtons(); if (isVisible()) m_timer.start(2000); return;
        }
        setResult(result.ok() ? tr("工控机已受理，正在等待执行与核验结果…") : tr("响应中断，正在按操作编号查询结果，不会重复发送操作…"));
        if (isVisible()) m_timer.start(500);
    }, 128 * 1024);
    if (!submitted) { m_operationId.clear(); setResult(tr("已有维护请求在处理，请稍后再试。"), true); }
    else { setResult(tr("正在提交维护操作…")); LoggingManager::audit("ipc.operation", "submitted", {{"path", path}, {"requestId", operationId}}); }
    updateButtons();
}
void IpcManagementPanel::pollOperation() {
    if (m_http.busy("management")) return;
    const QString id = m_operationId;
    m_http.request("management", "GET", "/management/operations/" + id, {}, {}, {}, [this, id](const JsonHttpResult &result) {
        if (id != m_operationId) return;
        const QString state = result.object.value("state").toString();
        if (result.ok() && (state == "applied" || state == "failed")) {
            const bool applied = state == "applied";
            setResult(result.object.value("message").toString(applied ? tr("操作已完成，正在读取实际状态。") : tr("工控机操作失败。")), !applied);
            LoggingManager::audit("ipc.operation", applied ? "success" : "failed", {{"requestId", id}, {"message", result.object.value("message").toString()}});
            m_operationId.clear();
            m_operationRefreshPending = true;
            if (applied && m_parameterOperation) m_readAfterRefresh = true;
            if (applied) { m_backupsLoaded = false; refreshBackups(); }
            updateButtons();
            if (isVisible()) m_timer.start(100);
        } else if (result.status == 404 || m_operationAge.elapsed() > 120000 || result.status == 401) {
            setResult(result.status == 404 ?
                tr("工控机已无法查询操作记录：%1。结果尚未确认，正在重新读取实际服务状态；未重复发送操作。").arg(id) :
                tr("尚未确认操作结果：%1。正在重新读取工控机实际状态；上位机没有重发操作。").arg(id), true);
            m_operationId.clear(); m_stale = true;
            m_operationRefreshPending = true;
            updateButtons(); if (isVisible()) m_timer.start(100);
        } else {
            setResult(result.ok() ? result.object.value("message").toString(tr("工控机正在执行，请稍候…")) : tr("连接暂不可用，继续查询已提交操作，未重复发送。"), !result.ok());
            if (isVisible()) m_timer.start(result.ok() ? 1000 : 4000);
        }
    }, 128 * 1024);
    updateButtons();
}
void IpcManagementPanel::refreshBackups() {
    if (!m_online || !m_operationId.isEmpty() || m_originDialog || m_http.busy("backups")) return;
    const auto connection = m_connection;
    const auto generation = m_backupGeneration;
    m_http.request("backups", "GET", "/management/backups", {}, {}, {}, [this, connection, generation](const JsonHttpResult &result) {
        if (connection.baseUrl != m_connection.baseUrl || connection.token != m_connection.token) return;
        if (generation != m_backupGeneration) { updateButtons(); return; }
        m_backupsLoaded = result.ok() && result.object.value("apiVersion").toInt() == 1 && result.object.value("backups").isArray();
        if (!m_backupsLoaded) {
            m_backupState->setText(tr("备份记录读取失败 · %1").arg(result.ok() ? tr("工控机返回的备份摘要格式无效") : responseMessage(result)));
        } else {
            const auto entries = result.object.value("backups").toArray();
            const auto latest = entries.isEmpty() ? QJsonObject{} : entries.first().toObject();
            const QString location = m_snapshot.value("projectRoot").toString() + "/" + result.object.value("backupRoot").toString("backups/config");
            const auto time = QDateTime::fromString(latest.value("createdAtUtc").toString(), Qt::ISODate);
            m_backupState->setText(entries.isEmpty() ? tr("位置 · %1\n尚无备份，可点击立即备份。").arg(location) :
                tr("位置 · %1\n最近备份 · %2 · %3 个文件\n备份编号 · %4").arg(location,
                    time.isValid() ? time.toLocalTime().toString("yyyy-MM-dd HH:mm:ss") : latest.value("createdAtUtc").toString(),
                    QString::number(latest.value("fileCount").toInt()), latest.value("backupId").toString()));
            if (result.object.value("invalidEntries").toInt() > 0)
                m_backupState->setText(m_backupState->text() + tr("\n有 %1 条备份摘要损坏或不可读，请核查工控机备份目录。").arg(result.object.value("invalidEntries").toInt()));
        }
        updateButtons();
    }, 128 * 1024);
    updateButtons();
}
void IpcManagementPanel::setResult(const QString &value, bool error) {
    m_result->setText(value);
    m_result->setStyleSheet(error ? "color:#b02d2d" : "color:#385346");
}
void IpcManagementPanel::showSessionReleaseResult(bool released, const QString &message) {
    setResult(message, !released);
}
void IpcManagementPanel::updateButtons() {
    if (m_loadedComponent == m_component->currentData() && !m_loadedRevision.isEmpty() &&
        revision(componentSnapshot().value("revision")) != m_loadedRevision) m_stale = true;
    const bool busy = m_http.busy("management") || !m_operationId.isEmpty() || m_originDialog || m_readAfterRefresh || m_operationRefreshPending;
    const QString busyReason = m_originDialog ? tr("请先完成或取消本次基准站坐标确认。") :
        !m_operationId.isEmpty() ? tr("维护操作正在执行或查询结果，完成后自动重新读取服务状态；请勿重复操作。") :
        m_operationRefreshPending ? tr("正在重新读取操作后的实际服务状态，请稍候。") :
        m_readAfterRefresh ? tr("正在读取工控机参数，请稍候。") :
        m_http.busy("management") ? tr("维护请求正在处理，请稍候。") : QString();
    m_backupCreate->setEnabled(m_online && !busy && !m_http.busy("backups"));
    m_backupRefresh->setEnabled(m_online && !busy && !m_http.busy("backups"));
    m_read->setEnabled(!busy);
    m_component->setEnabled(!busy);
    const bool editable = m_online && !m_stale && !m_loadedRevision.isEmpty() && m_loadedComponent == m_component->currentData() && !busy;
    const QString parameterCondition = !m_online ? tr("管理服务离线，请重新连接并读取实际状态。") :
        !busyReason.isEmpty() ? busyReason : m_loadedRevision.isEmpty() ? tr("请先读取所选组件的工控机参数。") :
        m_loadedComponent != m_component->currentData() ? tr("当前仍保留另一组件的编辑，请切回该组件，或读取所选组件。") :
        m_stale ? tr("工控机配置已变化，请重新读取后再编辑。") : QString();
    const QString editedComponent = m_loadedComponent == "fusion" ? tr("融合参数") : tr("导航参数");
    m_parameterEditState->setText(m_loadedRevision.isEmpty() ? tr("先读取参数，再编辑并保存。") :
        tr("%1：%2 · %3").arg(m_loadedComponent == m_component->currentData() ? tr("正在编辑") : tr("保留的编辑"),
            editedComponent, hasUnsavedChanges() ? tr("有未保存修改") : tr("保存值未改动")) +
            (parameterCondition.isEmpty() ? QString() : "\n" + parameterCondition));
    m_save->setEnabled(editable);
    m_save->setToolTip(parameterCondition.isEmpty() ? tr("只保存当前编辑到工控机，运行值保持原状。") : parameterCondition);
    const QString applyCondition = m_loadedComponent == "fusion" ? serviceCondition("fusion", "restart") : serviceCondition("tracking", "restart");
    m_saveApply->setEnabled(editable && applyCondition.isEmpty());
    m_saveApply->setToolTip(!parameterCondition.isEmpty() ? parameterCondition : applyCondition.isEmpty() ? tr("保存并重启对应服务，读回核验运行值后显示已生效。") : applyCondition);
    m_applySaved->setEnabled(editable && applyCondition.isEmpty() && !hasUnsavedChanges() && componentSnapshot().value("state") != "applied");
    m_applySaved->setToolTip(!parameterCondition.isEmpty() ? parameterCondition :
        hasUnsavedChanges() ? tr("当前存在未保存修改，可选择“保存并应用”；此按钮只应用工控机已保存配置。") :
        !applyCondition.isEmpty() ? applyCondition : componentSnapshot().value("state") == "applied" ?
        tr("工控机已保存配置已核验生效。") : tr("应用工控机已保存的配置，不提交当前未保存编辑。"));
    const bool canEdit = m_online && !m_stale && !m_loadedRevision.isEmpty() &&
        m_loadedComponent == m_component->currentData() && m_operationId.isEmpty() && !m_originDialog && !m_readAfterRefresh && !m_operationRefreshPending;
    for (auto *field : m_fields) field->setEnabled(canEdit);
    for (auto *button : m_serviceButtons) {
        bool allowed = false;
        for (const auto &value : m_snapshot.value("services").toArray()) {
            const auto service = value.toObject();
            if (service.value("id").toString() == button->property("serviceId").toString()) {
                const auto action = button->property("serviceAction").toString();
                const auto state = service.value("activeState").toString();
                allowed = service.value("canOperate").toBool() && (action == "start" ? state == "inactive" || state == "failed" : state == "active" || state == "failed");
            }
        }
        const auto id = button->property("serviceId").toString();
        const auto action = button->property("serviceAction").toString();
        const auto condition = serviceCondition(id, action);
        const auto service = serviceSnapshot(id);
        const auto state = service.value("activeState").toString();
        const QString reason = !m_online ? tr("请先连接工控机管理服务并读取实际状态。") :
            !busyReason.isEmpty() ? busyReason : !condition.isEmpty() ? condition :
            !service.value("canOperate").toBool() ? tr("工控机尚未提供可操作的服务状态，请重新读取。") :
            state == "activating" || state == "deactivating" || state == "reloading" ? tr("服务正在切换，完成后自动恢复可用操作。") :
            action == "start" && state == "active" ? tr("服务已经运行，可按条件选择重启或停止。") :
            action != "start" && state == "inactive" ? tr("服务已经停止。") :
            !allowed ? tr("服务状态尚未确认，请重新读取。") : QString();
        button->setToolTip(reason);
        button->setEnabled(m_online && !busy && allowed && condition.isEmpty());
    }
    const auto nextCondition = serviceCondition("tracking", "start");
    m_serviceHint->setText(!m_online ? tr("管理服务离线，服务状态与坐标需要重新读取。") :
        !busyReason.isEmpty() ? busyReason :
        !nextCondition.isEmpty() ? nextCondition : serviceSnapshot("tracking").value("activeState") == "active" ?
        tr("导航进程已运行；确认有效位姿和实测反馈后，在地图页点击“下发并执行”。需要重启融合时请先停止导航。") :
        tr("融合进程已运行，可以启动导航；定位初始化可继续进行，收到有效位姿和实测反馈后任务才会就绪。"));
}

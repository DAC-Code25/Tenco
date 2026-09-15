#include "help.h"

#include "configmanager.h"
#include "loggingmanager.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const char *kHelpStyle = R"(
QWidget#helpRoot {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #f5f7f2, stop:0.55 #eef3eb, stop:1 #e5ece3);
}
QFrame#helpHero, QFrame#helpCard, QFrame#helpNavCard {
    background: rgba(255, 255, 255, 0.97);
    border: 1px solid rgba(124, 143, 126, 0.22);
    border-radius: 16px;
}
QLabel#helpTitle {
    color: #132019;
    font-size: 28px;
    font-weight: 800;
}
QLabel#helpSubtitle {
    color: #4d5d54;
    font-size: 14px;
    font-weight: 500;
}
QLabel#helpPageTitle {
    color: #17211b;
    font-size: 24px;
    font-weight: 800;
}
QLabel#helpPageDesc {
    color: #526259;
    font-size: 14px;
    font-weight: 500;
}
QLabel#helpCardTitle {
    color: #1f3128;
    font-size: 17px;
    font-weight: 800;
}
QLabel#helpBody {
    color: #34483c;
    font-size: 14px;
    line-height: 170%;
}
QLabel#helpInfoLabel {
    color: #617267;
    font-size: 13px;
    font-weight: 700;
}
QLabel#helpInfoValue {
    color: #1f3128;
    font-size: 14px;
    font-weight: 600;
}
QListWidget#helpNavList {
    background: transparent;
    border: none;
    outline: none;
    color: #33493d;
    font-size: 14px;
    font-weight: 700;
}
QListWidget#helpNavList::item {
    padding: 12px 14px;
    margin: 3px 0;
    border-radius: 10px;
}
QListWidget#helpNavList::item:selected {
    background: #2f7d64;
    color: white;
}
QListWidget#helpNavList::item:selected:hover {
    background: #2f7d64;
    color: white;
}
QListWidget#helpNavList::item:hover {
    background: rgba(47, 125, 100, 0.12);
    color: #123026;
}
QPushButton#helpSecondaryButton {
    background: white;
    color: #3e5146;
    border: 1px solid #bdcbbb;
    border-radius: 11px;
    padding: 10px 18px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 700;
}
QPushButton#helpSecondaryButton:hover {
    background: #f6faf4;
}
QPushButton#helpPrimaryButton {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2f7d64, stop:1 #1f5f4b);
    color: white;
    border: none;
    border-radius: 11px;
    padding: 10px 18px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 800;
}
QPushButton#helpPrimaryButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b9274, stop:1 #287257);
}
)";

QString bulletText(const QStringList &lines)
{
    QStringList normalized;
    normalized.reserve(lines.size());
    for (const QString &line : lines) {
        normalized << QStringLiteral("• %1").arg(line);
    }
    return normalized.join(QStringLiteral("\n\n"));
}
}

Help::Help(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
{
    m_documents = {
        {tr("OAK 相机接入方案"),
         tr("工控机 OAK 相机接入、HTTP 接口、视频流和 systemd 托管说明。"),
         QStringLiteral("docs/develop/USB_OAK相机接入开发方案.md")},
        {tr("首页云台控制方案"),
         tr("PLC Modbus、云台/升降控制、键盘快捷键和安全限位说明。"),
         QStringLiteral("docs/develop/首页云台控制开发方案.md")},
        {tr("直线/多垄作业控制方案"),
         tr("双端点示教、直线保持、多垄切换、工控机控制闭环和鲁棒性说明。"),
         QStringLiteral("docs/develop/多垄穿梭作业闭环系统开发方案.md")},
        {tr("维护配置管理方案"),
         tr("维护页配置项、保存/应用、备份恢复、热加载和生产维护说明。"),
         QStringLiteral("docs/develop/维护界面配置管理开发方案.md")},
        {tr("帮助界面开发方案"),
         tr("帮助页定位、分类导航、故障排查、文档入口和运行信息说明。"),
         QStringLiteral("docs/develop/帮助界面开发方案.md")}
    };

    buildUi();
}

void Help::buildUi()
{
    if (!ui || !ui->helpPage) {
        return;
    }

    if (ui->helpLabel) {
        ui->helpLabel->hide();
    }

    auto *rootLayout = qobject_cast<QVBoxLayout *>(ui->helpPage->layout());
    if (!rootLayout) {
        rootLayout = new QVBoxLayout(ui->helpPage);
        ui->helpPage->setLayout(rootLayout);
    }
    while (QLayoutItem *item = rootLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(16);
    ui->helpPage->setObjectName(QStringLiteral("helpRoot"));
    ui->helpPage->setStyleSheet(QString::fromUtf8(kHelpStyle));

    auto *hero = new QFrame(ui->helpPage);
    hero->setObjectName(QStringLiteral("helpHero"));
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(22, 18, 22, 18);
    heroLayout->setSpacing(18);

    auto *titleBlock = new QVBoxLayout();
    titleBlock->setSpacing(8);
    auto *title = new QLabel(tr("使用帮助与现场排障"), hero);
    title->setObjectName(QStringLiteral("helpTitle"));
    auto *subtitle = new QLabel(tr("提供操作流程、常见故障排查、快捷键、开发文档和运行目录入口；参数修改仍在维护页完成。"), hero);
    subtitle->setObjectName(QStringLiteral("helpSubtitle"));
    subtitle->setWordWrap(true);
    titleBlock->addWidget(title);
    titleBlock->addWidget(subtitle);
    heroLayout->addLayout(titleBlock, 1);

    auto *safety = new QLabel(tr("执行路径、云台或底盘操作前，请先确认急停、遥控器和现场人员安全。"), hero);
    safety->setObjectName(QStringLiteral("helpSubtitle"));
    safety->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    safety->setWordWrap(true);
    heroLayout->addWidget(safety);
    rootLayout->addWidget(hero);

    auto *contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(16);

    auto *navCard = new QFrame(ui->helpPage);
    navCard->setObjectName(QStringLiteral("helpNavCard"));
    navCard->setMinimumWidth(190);
    navCard->setMaximumWidth(240);
    auto *navLayout = new QVBoxLayout(navCard);
    navLayout->setContentsMargins(12, 12, 12, 12);
    navLayout->setSpacing(8);
    auto *navTitle = new QLabel(tr("帮助分类"), navCard);
    navTitle->setObjectName(QStringLiteral("helpCardTitle"));
    navLayout->addWidget(navTitle);
    m_navList = new QListWidget(navCard);
    m_navList->setObjectName(QStringLiteral("helpNavList"));
    m_navList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_navList->setFocusPolicy(Qt::NoFocus);
    navLayout->addWidget(m_navList, 1);
    contentLayout->addWidget(navCard);

    m_stack = new QStackedWidget(ui->helpPage);
    contentLayout->addWidget(m_stack, 1);
    rootLayout->addLayout(contentLayout, 1);

    addTopic(tr("快速开始"), createQuickStartPage());
    addTopic(tr("系统概览"), createOverviewPage());
    addTopic(tr("安全规范"), createSafetyPage());
    addTopic(tr("作业流程"), createSopPage());
    addTopic(tr("首页功能"), createHomePage());
    addTopic(tr("地图与作业"), createMapWorkPage());
    addTopic(tr("维护配置"), createMaintenancePage());
    addTopic(tr("设备连接"), createConnectionPage());
    addTopic(tr("运维巡检"), createOperationsPage());
    addTopic(tr("故障排查"), createTroubleshootingPage());
    addTopic(tr("快捷键"), createShortcutPage());
    addTopic(tr("文档资料"), createDocumentPage());
    addTopic(tr("数据与日志"), createDataLogPage());
    addTopic(tr("运行信息"), createRuntimePage());

    connect(m_navList, &QListWidget::currentRowChanged, m_stack, &QStackedWidget::setCurrentIndex);
    if (m_navList->count() > 0) {
        m_navList->setCurrentRow(0);
    }
}

QWidget *Help::createThemePage(const QString &title, const QString &description)
{
    auto *page = new QWidget();
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(18);

    auto *header = new QFrame(content);
    header->setObjectName(QStringLiteral("helpCard"));
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(22, 20, 22, 20);
    headerLayout->setSpacing(10);
    auto *titleLabel = new QLabel(title, header);
    titleLabel->setObjectName(QStringLiteral("helpPageTitle"));
    auto *descLabel = new QLabel(description, header);
    descLabel->setObjectName(QStringLiteral("helpPageDesc"));
    descLabel->setWordWrap(true);
    headerLayout->addWidget(titleLabel);
    headerLayout->addWidget(descLabel);
    contentLayout->addWidget(header);

    scroll->setWidget(content);
    pageLayout->addWidget(scroll);
    return page;
}

QWidget *Help::createCard(const QString &title, const QStringList &lines, QWidget *parent) const
{
    auto *card = new QFrame(parent);
    card->setObjectName(QStringLiteral("helpCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(14);
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("helpCardTitle"));
    auto *body = new QLabel(bulletText(lines), card);
    body->setObjectName(QStringLiteral("helpBody"));
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->setMargin(2);
    layout->addWidget(titleLabel);
    layout->addWidget(body);
    return card;
}

QWidget *Help::createStepCard(const QString &title, const QStringList &steps, QWidget *parent) const
{
    QStringList numbered;
    numbered.reserve(steps.size());
    for (int i = 0; i < steps.size(); ++i) {
        numbered << QStringLiteral("%1. %2").arg(i + 1).arg(steps.at(i));
    }
    return createCard(title, numbered, parent);
}

QWidget *Help::createQuickStartPage()
{
    auto *page = createThemePage(tr("快速开始"),
                                 tr("按现场启动、检查、执行和收尾的顺序说明整套系统如何使用。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createStepCard(tr("推荐启动流程"),
                                     {tr("确认底盘控制器、工控机、PLC、相机服务已上电，并处于同一网络。"),
                                      tr("打开软件后先看首页状态日志、视频预览和云台连接状态。"),
                                      tr("进入地图页加载或新建地图，确认实时位姿在地图上正常更新。"),
                                      tr("选择路径或作业任务，确认现场安全后再启动执行。"),
                                      tr("如果设备 IP、端口或服务地址变化，进入维护页修改并点击保存并应用。")},
                                     content));
    layout->addWidget(createStepCard(tr("标准收尾流程"),
                                     {tr("停止当前路径、作业或手动控制，确认底盘速度为 0。"),
                                      tr("停止录像并确认文件已经保存到预期目录。"),
                                      tr("将云台和升降机构移动到安全位置，避免运输或断电后受力。"),
                                      tr("检查首页和运行日志中是否存在未处理错误。"),
                                      tr("如当天修改过配置，记录修改项并确认备份目录中存在可回退配置。")},
                                     content));
    layout->addWidget(createCard(tr("使用边界"),
                                 {tr("帮助页只提供说明和排障，不直接修改参数或下发控制命令。"),
                                  tr("参数修改、备份恢复和热加载统一在维护页完成。"),
                                  tr("路径执行、云台动作和底盘运动前，应先确认急停、遥控器和现场人员安全。"),
                                  tr("无法确认设备状态时，优先停止任务并保留日志，不要反复下发控制命令。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createOverviewPage()
{
    auto *page = createThemePage(tr("系统概览"),
                                 tr("从系统架构角度说明上位机、工控机、底盘控制器、PLC 和相机服务之间的职责边界。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("模块职责"),
                                 {tr("上位机：负责可视化交互、地图编辑、任务规划、配置维护、帮助文档和运行状态展示。"),
                                  tr("工控机：负责相机服务、融合位姿、作业服务和适合本地实时执行的控制逻辑。"),
                                  tr("底盘控制器：负责接收速度/控制指令，并执行底盘运动控制。"),
                                  tr("云台 PLC：负责升降、水平旋转、俯仰等机构动作和状态寄存器反馈。"),
                                  tr("相机/视觉节点：负责原始图像、处理后图像、拍照录像和视觉处理结果输出。")},
                                 content));
    layout->addWidget(createCard(tr("数据流向"),
                                 {tr("状态数据：控制器/工控机通过 HTTP 状态接口进入上位机首页和地图页。"),
                                  tr("视频数据：工控机相机服务或 OAK 后端提供预览流，首页负责显示和切换。"),
                                  tr("控制指令：上位机通过 WebSocket、HTTP 或 Modbus 客户端向对应设备下发。"),
                                  tr("作业任务：上位机规划任务，工控机侧服务负责接收、状态反馈和实时执行闭环。"),
                                  tr("配置数据：维护页写入 config.json，保存并应用后通知运行模块热加载。")},
                                 content));
    layout->addWidget(createCard(tr("设计原则"),
                                 {tr("规划、交互和可视化尽量保留在上位机，实时性强的控制闭环尽量放在工控机侧。"),
                                  tr("通用路径规划与特定垄间作业模式并存，避免为了特殊场景破坏通用能力。"),
                                  tr("帮助页和维护页分工明确：帮助页解释，维护页修改，运行页执行。"),
                                  tr("现场问题排查遵循链路化思路：设备供电、网络连通、服务在线、接口返回、上位机配置、运行日志。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createSafetyPage()
{
    auto *page = createThemePage(tr("安全规范"),
                                 tr("面向生产现场的安全约束。所有自动运行、云台动作和维护调参都应先满足这些前置条件。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("运行前安全检查"),
                                 {tr("确认急停开关有效，现场人员知道急停位置。"),
                                  tr("确认遥控器、上位机、控制器之间的控制权关系明确，避免多人同时控制。"),
                                  tr("确认车辆前后、垄间、云台旋转半径和升降机构附近无人员、线缆和障碍物。"),
                                  tr("确认相机、云台、升降机构没有超过机械极限，线缆没有被拉紧或缠绕。"),
                                  tr("首次运行或修改控制参数后，必须先低速、短距离验证。")},
                                 content));
    layout->addWidget(createCard(tr("运行中禁止事项"),
                                 {tr("禁止在车辆运动时进入车体前方、后方或狭窄垄间。"),
                                  tr("禁止在云台动作过程中手扶相机、支架或线缆。"),
                                  tr("禁止在定位明显跳变、地图位姿异常或控制日志持续报错时继续自动作业。"),
                                  tr("禁止在不清楚参数含义时随意扩大速度、软限位或超时时间。")},
                                 content));
    layout->addWidget(createCard(tr("异常处理原则"),
                                 {tr("出现人员风险、车辆偏离、云台卡滞、升降异常时，先急停或停止任务，再分析日志。"),
                                  tr("云台或升降到达极限后仍抖动时，应先断开控制输出或停机检查，禁止继续反向试探。"),
                                  tr("网络、定位或 PLC 通信异常时，不要高频重复点击控制按钮，应先确认设备在线和配置地址。"),
                                  tr("故障恢复后必须低速复测，再恢复正常作业。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createSopPage()
{
    auto *page = createThemePage(tr("标准作业流程"),
                                 tr("用于把软件操作固化为现场 SOP，减少不同人员操作差异。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createStepCard(tr("单次路径/作业执行 SOP"),
                                     {tr("运行前检查设备供电、网络、急停、遥控器和现场环境。"),
                                      tr("进入首页确认状态、视频、云台和日志无明显异常。"),
                                      tr("进入地图页确认位姿稳定，地图坐标和车辆实际位置一致。"),
                                      tr("加载或编辑目标路径/作业计划，检查起点、终点、方向、速度和停止点。"),
                                      tr("先短距离低速执行，确认转向、速度和停止逻辑正常。"),
                                      tr("正式启动任务后持续观察地图位姿、车辆实际轨迹和首页日志。"),
                                      tr("任务结束后停止控制、保存必要数据、记录异常和配置修改。")},
                                     content));
    layout->addWidget(createStepCard(tr("配置变更 SOP"),
                                     {tr("先在运行信息或维护页确认当前配置文件路径。"),
                                      tr("修改前确认已有启动备份，必要时手动复制当前 config.json。"),
                                      tr("只修改明确需要调整的参数，避免一次修改多个无关项。"),
                                      tr("点击保存并应用后观察首页、地图或云台日志是否提示配置已应用。"),
                                      tr("用小范围动作验证配置效果，确认正常后再进入作业。"),
                                      tr("如果结果不符合预期，使用恢复上次配置回退。")},
                                     content));
    layout->addWidget(createCard(tr("交接记录建议"),
                                 {tr("记录日期、操作者、车辆编号、作业区域和使用的地图/路径文件。"),
                                  tr("记录本次是否修改配置、修改原因和修改前后关键值。"),
                                  tr("记录异常现象、处理动作、保留的日志文件和是否已恢复。"),
                                  tr("记录设备状态：底盘、电池、工控机、PLC、相机、云台和网络。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createHomePage()
{
    auto *page = createThemePage(tr("首页功能"),
                                 tr("首页负责设备状态、视频预览、云台控制、拍照录像和手动控制。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("视频与相机"),
                                 {tr("默认视频流来自维护页配置的预览流 URL。"),
                                  tr("视频流选项可在维护页的视频/相机分组中维护，用于切换不同相机或处理后画面。"),
                                  tr("拍照、录像和状态查询依赖工控机相机控制基地址。"),
                                  tr("视频断开后会按配置的重连间隔自动重连。"),
                                  tr("录像前应确认保存目录、磁盘空间和编码格式，避免任务完成后没有可用录像文件。")},
                                 content));
    layout->addWidget(createCard(tr("云台与升降"),
                                 {tr("云台控制依赖 PLC Modbus TCP 连接和维护页中的软限位配置。"),
                                  tr("升降、水平、俯仰按钮支持按下持续动作，松开停止；安全超时会防止动作持续失控。"),
                                  tr("如果按钮不可点击，先检查维护页是否启用云台控制，以及 PLC 地址是否正确。"),
                                  tr("软限位只是一层软件保护，不能替代机械限位、行程开关和现场目视确认。")},
                                 content));
    layout->addWidget(createCard(tr("底盘与日志"),
                                 {tr("首页手动控制通过底盘 WebSocket 下发速度指令。"),
                                  tr("急停按钮用于立即停止当前底盘速度指令。"),
                                  tr("首页日志只应关注状态变化、错误和关键操作结果，重复状态不会持续刷屏。"),
                                  tr("手动控制主要用于测试、定位和短距离移动，不建议替代正式路径/作业任务。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createMapWorkPage()
{
    auto *page = createThemePage(tr("地图与作业"),
                                 tr("地图页负责地图编辑、路径规划、路径跟踪和直线/多垄作业任务。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("地图基础操作"),
                                 {tr("可加载、新建和保存地图文件。"),
                                  tr("地图基准经纬度由维护页维护，影响后续坐标换算。"),
                                  tr("实时位姿直接来自工控机融合服务；显示过期时检查源观测年龄、原点和地图绑定。"),
                                  tr("如果地图上的车辆位置与现场位置偏差明显，应暂停自动任务，先确认定位源和地图基准。")},
                                 content));
    layout->addWidget(createCard(tr("路径与任务执行"),
                                 {tr("路径规划适合通用场景，直线/多垄作业适合垄间来回作业场景。"),
                                  tr("路径执行前应确认起点、终点、方向和现场障碍物。"),
                                  tr("大幅调整路径跟踪参数后，建议先低速测试，再进入实际作业。"),
                                  tr("定位抖动、地面颠簸和轮胎打滑都会影响轨迹效果，现场应结合速度、加速度和控制增益综合调整。")},
                                 content));
    layout->addWidget(createCard(tr("直线/多垄作业"),
                                 {tr("作业计划下发依赖工控机作业服务地址。"),
                                  tr("单垄往返、多垄切换和端点掉头等逻辑由上位机规划，工控机侧负责实时控制执行。"),
                                  tr("如果状态不刷新，检查任务与位姿服务地址。重新连接不会自动启动，需重新获取操作权。"),
                                  tr("多垄切换任务应在地图上明确垄间间距、出垄距离、原地旋转方向和下一垄对准方向。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createMaintenancePage()
{
    auto *page = createThemePage(tr("维护配置"),
                                 tr("维护页用于生产现场修改运行参数，并提供保存、热加载、备份和恢复能力。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("按钮含义"),
                                 {tr("保存：只写入 config.json，不影响当前运行模块。"),
                                  tr("保存并应用：写入 config.json，并立即通知首页、地图、云台、相机和底盘等模块热加载。"),
                                  tr("恢复上次配置：恢复最近一次启动备份，并立即应用。"),
                                  tr("加载配置文件：只把所选 JSON 读入表单，不直接写盘，也不直接应用。")},
                                 content));
    layout->addWidget(createCard(tr("修改建议"),
                                 {tr("通信地址、PLC 地址、工控机服务地址修改前，应先确认目标设备在线。"),
                                  tr("云台软限位修改前必须确认机械极限和线缆干涉风险。"),
                                  tr("路径跟踪参数建议小幅调整，并在低速空旷环境先测试。"),
                                  tr("生产现场建议每次只修改一类参数，并记录修改时间、原因和验证结果。")},
                                 content));
    layout->addWidget(createCard(tr("配置风险等级"),
                                 {tr("低风险：显示相关、轮询周期、请求超时等，一般可直接保存并应用。"),
                                  tr("中风险：视频地址、作业服务地址、地图基准等，修改后需要重新验证对应功能。"),
                                  tr("高风险：云台软限位、底盘速度、路径跟踪增益、加速度限制等，修改后必须低速现场验证。"),
                                  tr("不确定含义的参数不要直接修改，应先查帮助、开发文档或保留当前备份。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createConnectionPage()
{
    auto *page = createThemePage(tr("设备连接"),
                                 tr("列出本系统主要外部设备、接口类型和对应维护配置位置。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("关键连接"),
                                 {tr("底盘控制器：WebSocket 用于手动速度、控制权和急停，HTTP 保留设备状态读取。"),
                                  tr("工控机相机服务：HTTP 用于视频流、拍照、录像和相机状态。"),
                                  tr("云台 PLC：Modbus TCP 用于升降、水平旋转和俯仰控制。"),
                                  tr("作业服务：HTTP 用于直线/多垄作业计划下发、启动、停止和状态查询。"),
                                  tr("所有设备应使用固定 IP 或可控的 DHCP 绑定，避免重启后地址变化导致服务不可用。")},
                                 content));
    layout->addWidget(createCard(tr("配置位置"),
                                 {tr("所有运行地址优先在维护页对应分组中修改。"),
                                  tr("配置文件路径可在维护页和帮助页运行信息中查看。"),
                                  tr("如果修改后无效，确认是否点击了保存并应用，而不是只点击保存。"),
                                  tr("网络故障排查顺序建议为：物理连接、IP 连通、端口服务、接口返回、上位机配置。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createOperationsPage()
{
    auto *page = createThemePage(tr("运维巡检"),
                                 tr("用于日常维护、班前检查、班后归档和版本/配置管理。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("班前检查清单"),
                                 {tr("检查电池电量、底盘电源、工控机电源、PLC 电源和相机供电。"),
                                  tr("检查网络连接，确认控制器、工控机和 PLC IP 地址与维护页配置一致。"),
                                  tr("检查云台升降和旋转机构是否松动、卡滞、线缆拉扯或接插件松脱。"),
                                  tr("检查相机镜头是否污染，画面是否正常，录像保存目录是否可写。"),
                                  tr("检查地图位姿是否稳定，车辆朝向和现场实际方向是否一致。")},
                                 content));
    layout->addWidget(createCard(tr("班后归档清单"),
                                 {tr("停止所有任务和手动控制，确认车辆处于安全停车状态。"),
                                  tr("保存地图、路径或作业计划变更，记录当天使用的配置版本。"),
                                  tr("导出或备份关键日志、异常截图、录像和配置文件。"),
                                  tr("记录未解决问题、临时绕行方案和下次启动前必须确认的事项。")},
                                 content));
    layout->addWidget(createCard(tr("版本与配置管理"),
                                 {tr("正式作业前应确认当前软件分支、提交版本和配置文件来源。"),
                                  tr("生产参数调整应形成记录，避免只保留在本地口头经验中。"),
                                  tr("远端仓库合并新功能后，应从最新 master 新建功能分支继续开发。"),
                                  tr("重要配置修改前后都应保留备份，现场回退应优先使用维护页恢复上次配置。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createTroubleshootingPage()
{
    auto *page = createThemePage(tr("故障排查"),
                                 tr("按现场最常见问题给出排查顺序。先看安全，再看连接，再看配置，再看日志。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createStepCard(tr("视频打不开"),
                                     {tr("确认工控机相机服务正在运行。"),
                                      tr("在维护页检查视频流 URL 和相机控制基地址。"),
                                      tr("确认上位机可以访问工控机 IP 和端口。"),
                                      tr("查看首页日志是否出现连接超时、HTTP 错误或视频流结束。"),
                                      tr("如果是多路视频流，逐个切换确认是全部失败还是某一路失败。")},
                                     content));
    layout->addWidget(createStepCard(tr("云台无反应或按钮不可点击"),
                                     {tr("确认维护页已启用云台控制。"),
                                      tr("确认 PLC IP、端口和 Unit ID 正确。"),
                                      tr("确认云台状态轮询能读到高度、水平角和俯仰角。"),
                                      tr("检查是否已经到达软限位或机械极限。"),
                                      tr("如机构卡住或持续抖动，先停止控制并断电检查机械结构，禁止继续反复点动。")},
                                     content));
    layout->addWidget(createStepCard(tr("地图收不到位姿"),
                                     {tr("确认状态读取 HTTP 地址正确。"),
                                      tr("确认工控机位姿服务正在发布带新观测时间的状态。"),
                                      tr("检查首页状态日志是否有状态请求失败或超时。"),
                                      tr("检查地图基准经纬度是否与现场坐标系一致。")},
                                     content));
    layout->addWidget(createStepCard(tr("路径下发后小车不动"),
                                     {tr("确认底盘 WebSocket 已连接。"),
                                      tr("确认急停未保持触发，遥控器和控制器处于允许远程控制状态。"),
                                      tr("检查路径是否为空、目标点是否已经被判定到达。"),
                                      tr("查看首页和地图日志，确认是否有命令超时或服务返回失败。")},
                                     content));
    layout->addWidget(createStepCard(tr("保存并应用后没有生效"),
                                     {tr("确认保存前配置自检通过。"),
                                      tr("确认点击的是保存并应用，而不是保存。"),
                                      tr("如果修改的是视频后端或地图基准，注意部分已有状态需要重新打开视频或重新加载地图才直观体现。"),
                                      tr("查看运行信息中的配置文件路径，确认修改的是当前程序实际使用的 config.json。")},
                                     content));
    layout->addWidget(createCard(tr("故障分级建议"),
                                 {tr("P0 安全风险：车辆失控、急停无效、云台卡滞、人员进入危险区域。处理方式是立即停机并现场检查。"),
                                  tr("P1 作业中断：底盘、定位、相机、PLC 或作业服务不可用。处理方式是停止任务、保留日志、按连接链路排查。"),
                                  tr("P2 功能异常：某个按钮、某一路视频、某个配置项异常。处理方式是记录复现步骤并回退最近配置。"),
                                  tr("P3 体验问题：显示、布局、文字、提示不清晰。处理方式是记录截图并进入后续 UI 优化。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createShortcutPage()
{
    auto *page = createThemePage(tr("快捷键"),
                                 tr("列出当前已实现的键盘和鼠标快捷操作，包括首页手动/云台控制与地图编辑快捷操作。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("首页底盘手动控制"),
                                 {tr("W：前进，按下开始持续发送前进速度，松开停止。"),
                                  tr("S：后退，按下开始持续发送后退速度，松开停止。"),
                                  tr("A：左转，按下开始持续发送左转角速度，松开停止。"),
                                  tr("D：右转，按下开始持续发送右转角速度，松开停止。"),
                                  tr("W/A/S/D 仅在首页且手动键盘控制条件满足时生效。")},
                                 content));
    layout->addWidget(createCard(tr("首页云台快捷键"),
                                 {tr("方向键上：升降上升。"),
                                  tr("方向键下：升降下降。"),
                                  tr("方向键左：水平左旋。"),
                                  tr("方向键右：水平右旋。"),
                                  tr("左 Ctrl + 方向键上：俯仰上仰。"),
                                  tr("左 Ctrl + 方向键下：俯仰下俯。")},
                                 content));
    layout->addWidget(createCard(tr("地图视图快捷操作"),
                                 {tr("鼠标左键拖拽地图空白区域：平移地图视图。"),
                                  tr("鼠标滚轮：缩放地图视图。"),
                                  tr("鼠标左键点击地图点：选中最近地图点，并同步右侧点位参数。"),
                                  tr("鼠标左键点击空白位置：更新当前鼠标位置对应的地图坐标输入值。"),
                                  tr("鼠标右键点击地图：打开地图点上下文菜单，可新建或删除地图点。")},
                                 content));
    layout->addWidget(createCard(tr("地图编辑组合键"),
                                 {tr("Ctrl + 鼠标左键拖拽地图点：移动该地图点，并自动刷新关联路径。"),
                                  tr("Alt + 鼠标左键拖拽地图点：旋转该地图点朝向，并自动刷新关联路径。"),
                                  tr("Ctrl + Alt + 鼠标左键点击一个地图点：选择路径起点。"),
                                  tr("再次 Ctrl + Alt + 鼠标左键点击另一个地图点：按当前路径类型创建起点到终点的路径。"),
                                  tr("Ctrl + A：切换地图路径显示/隐藏。")},
                                 content));
    layout->addWidget(createCard(tr("地图作业快捷操作"),
                                 {tr("启用“从地图添加点”后，鼠标左键点击地图位置可新增地图点。"),
                                  tr("启用“添加中间点”后，鼠标左键点击地图位置可新增直线/多垄作业中间点。"),
                                  tr("右键删除地图点会同步清理无效路径和路线队列。")},
                                 content));
    layout->addWidget(createCard(tr("使用限制"),
                                 {tr("首页方向键会被云台控制优先接管，避免触发顶部菜单切换。"),
                                  tr("云台快捷键只在云台控制启用、PLC 配置有效时才有实际效果。"),
                                  tr("地图编辑组合键只在地图编辑模式开启时生效。"),
                                  tr("按键持续动作，松开停止；云台安全超时会自动停止。"),
                                  tr("如果焦点在输入控件中，优先确认当前页面是否适合接收快捷键。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createDocumentPage()
{
    auto *page = createThemePage(tr("文档资料"),
                                 tr("打开项目已有开发文档。第一版使用系统默认程序打开 Markdown 文件。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());

    for (const DocumentLink &doc : m_documents) {
        auto *card = new QFrame(content);
        card->setObjectName(QStringLiteral("helpCard"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(22, 20, 22, 20);
        cardLayout->setSpacing(14);
        auto *title = new QLabel(doc.title, card);
        title->setObjectName(QStringLiteral("helpCardTitle"));
        auto *desc = new QLabel(doc.description + QStringLiteral("\n") + doc.relativePath, card);
        desc->setObjectName(QStringLiteral("helpBody"));
        desc->setWordWrap(true);
        desc->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *button = new QPushButton(tr("打开文档"), card);
        button->setObjectName(QStringLiteral("helpPrimaryButton"));
        connect(button, &QPushButton::clicked, this, [this, doc]() {
            openPath(repositoryPath(doc.relativePath), tr("打开文档失败"));
        });
        cardLayout->addWidget(title);
        cardLayout->addWidget(desc);
        cardLayout->addWidget(button, 0, Qt::AlignLeft);
        layout->addWidget(card);
    }

    auto *openDocsDir = new QPushButton(tr("打开开发文档目录"), content);
    openDocsDir->setObjectName(QStringLiteral("helpSecondaryButton"));
    connect(openDocsDir, &QPushButton::clicked, this, [this]() {
        openPath(repositoryPath(QStringLiteral("docs/develop")), tr("打开文档目录失败"));
    });
    layout->addWidget(openDocsDir, 0, Qt::AlignLeft);
    layout->addStretch(1);
    return page;
}

QWidget *Help::createDataLogPage()
{
    auto *page = createThemePage(tr("数据与日志"),
                                 tr("说明系统运行过程中应关注和保留的数据，便于故障追溯、版本回退和现场交接。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());
    layout->addWidget(createCard(tr("应保留的数据"),
                                 {tr("配置文件 config.json：记录通信地址、控制参数、云台限位、视频流和作业服务地址。"),
                                  tr("配置备份 config_backups：用于恢复启动前配置，最多保留最近 10 份。"),
                                  tr("运行日志 tenco.log / 会话日志：记录状态变化、请求错误、连接断开、配置应用和关键操作。"),
                                  tr("审计日志 audit.log：记录保存、恢复、导出和关键控制动作。"),
                                  tr("诊断导出目录 diagnostics：用于现场打包日志、脱敏配置和环境摘要。"),
                                  tr("视频、截图和录像：用于追溯现场环境、相机状态和作业过程。"),
                                  tr("地图、路径和作业计划文件：用于复现车辆当时执行的目标。")},
                                 content));
    layout->addWidget(createCard(tr("日志查看建议"),
                                 {tr("先看错误发生时间附近的 WARN/ERROR，再向前查看配置应用、连接状态和用户操作。"),
                                  tr("如果日志中持续出现同一错误，应优先处理根因，不要重复点击按钮制造更多噪声。"),
                                  tr("视频流、云台、底盘、作业服务问题应分别结合首页日志、运行日志、审计日志和维护页配置排查。"),
                                  tr("日志级别、轮转、脱敏和诊断导出可在维护页的“日志”页统一配置。"),
                                  tr("提交问题给开发人员时，应同时提供配置文件、相关日志、复现步骤和现场现象。")},
                                 content));
    layout->addWidget(createCard(tr("隐私与安全"),
                                 {tr("录像、截图和地图文件可能包含现场环境信息，外发前应确认是否允许共享。"),
                                  tr("配置文件可能包含设备地址和认证 token，不应随意发到公共渠道。"),
                                  tr("远程协助时建议只提供必要时间段日志和脱敏配置。")},
                                 content));
    layout->addStretch(1);
    return page;
}

QWidget *Help::createRuntimePage()
{
    auto *page = createThemePage(tr("运行信息"),
                                 tr("用于确认当前程序实际使用的配置、备份、日志和运行目录。"));
    auto *content = qobject_cast<QScrollArea *>(page->layout()->itemAt(0)->widget())->widget();
    auto *layout = qobject_cast<QVBoxLayout *>(content->layout());

    const ConfigManager &config = ConfigManager::instance();
    const QString configPath = config.configFilePath();
    const QString configDir = QFileInfo(configPath).absolutePath();
    const QString backupDir = config.backupDirectoryPath();
    const QString runtimeDir = QCoreApplication::applicationDirPath();
    const QString logsDir = LoggingManager::logDirectoryPath();
    const QString runtimeLog = LoggingManager::currentLogFilePath();
    const QString auditLog = LoggingManager::auditLogFilePath();
    const QString diagnosticsDir = LoggingManager::diagnosticsDirectoryPath();
    const QString docsDir = repositoryPath(QStringLiteral("docs/develop"));

    auto *infoCard = new QFrame(content);
    infoCard->setObjectName(QStringLiteral("helpCard"));
    auto *infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(22, 20, 22, 20);
    infoLayout->setSpacing(14);
    auto *title = new QLabel(tr("当前运行路径"), infoCard);
    title->setObjectName(QStringLiteral("helpCardTitle"));
    infoLayout->addWidget(title);
    infoLayout->addWidget(createInfoRow(tr("配置文件"), configPath, infoCard));
    infoLayout->addWidget(createInfoRow(tr("配置来自文件"), config.loadedFromFile() ? tr("是") : tr("否，当前使用内置默认配置"), infoCard));
    infoLayout->addWidget(createInfoRow(tr("程序运行目录"), runtimeDir, infoCard));
    infoLayout->addWidget(createInfoRow(tr("备份目录"), backupDir, infoCard));
    infoLayout->addWidget(createInfoRow(tr("日志目录"), logsDir, infoCard));
    infoLayout->addWidget(createInfoRow(tr("运行日志"), runtimeLog, infoCard));
    infoLayout->addWidget(createInfoRow(tr("审计日志"), auditLog, infoCard));
    infoLayout->addWidget(createInfoRow(tr("诊断导出目录"), diagnosticsDir, infoCard));
    infoLayout->addWidget(createInfoRow(tr("开发文档目录"), docsDir, infoCard));
    layout->addWidget(infoCard);

    auto *buttonCard = new QFrame(content);
    buttonCard->setObjectName(QStringLiteral("helpCard"));
    auto *buttonLayout = new QHBoxLayout(buttonCard);
    buttonLayout->setContentsMargins(22, 20, 22, 20);
    buttonLayout->setSpacing(12);
    auto makeButton = [&](const QString &text, const QString &path) {
        auto *button = new QPushButton(text, buttonCard);
        button->setObjectName(QStringLiteral("helpSecondaryButton"));
        connect(button, &QPushButton::clicked, this, [this, path]() {
            openPath(path, tr("打开目录失败"));
        });
        buttonLayout->addWidget(button);
    };
    makeButton(tr("打开配置目录"), configDir);
    makeButton(tr("打开备份目录"), backupDir);
    makeButton(tr("打开日志目录"), logsDir);
    makeButton(tr("打开文档目录"), docsDir);
    buttonLayout->addStretch(1);
    layout->addWidget(buttonCard);
    layout->addStretch(1);
    return page;
}

QWidget *Help::createInfoRow(const QString &label, const QString &value, QWidget *parent) const
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *labelWidget = new QLabel(label, row);
    labelWidget->setObjectName(QStringLiteral("helpInfoLabel"));
    labelWidget->setMinimumWidth(120);
    auto *valueWidget = new QLabel(value, row);
    valueWidget->setObjectName(QStringLiteral("helpInfoValue"));
    valueWidget->setWordWrap(true);
    valueWidget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(labelWidget);
    layout->addWidget(valueWidget, 1);
    return row;
}

void Help::addTopic(const QString &title, QWidget *page)
{
    if (!m_navList || !m_stack || !page) {
        return;
    }

    auto *item = new QListWidgetItem(title, m_navList);
    item->setSizeHint(QSize(160, 44));
    m_stack->addWidget(page);
}

void Help::openPath(const QString &path, const QString &errorTitle) const
{
    const QFileInfo info(path);
    if (!info.exists()) {
        QMessageBox::warning(ui ? ui->helpPage : nullptr,
                             errorTitle,
                             tr("路径不存在：\n%1").arg(QDir::toNativeSeparators(path)));
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()))) {
        QMessageBox::warning(ui ? ui->helpPage : nullptr,
                             errorTitle,
                             tr("无法打开路径：\n%1").arg(QDir::toNativeSeparators(path)));
    }
}

QString Help::appDataPath(const QString &child) const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return child.isEmpty() ? base : QDir(base).filePath(child);
}

QString Help::repositoryPath(const QString &relativePath) const
{
    QDir dir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        dir.filePath(relativePath),
        QDir(dir.filePath(QStringLiteral(".."))).filePath(relativePath),
        QDir(dir.filePath(QStringLiteral("../Tenco"))).filePath(relativePath),
        QDir::current().absoluteFilePath(relativePath)
    };

    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }

    return QDir::current().absoluteFilePath(relativePath);
}

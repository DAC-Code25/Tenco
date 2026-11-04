#include "about.h"

#include "configmanager.h"
#include "loggingmanager.h"
#include "supportpagestyle.h"
#include "ui_mainwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QObject>
#include <QOperatingSystemVersion>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>
#include <QtGlobal>

#ifdef TENCO_ENABLE_ABOUT_3D
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStackedWidget>
#endif

#include <functional>

namespace {
const char *kAboutStyle = R"(
QFrame#aboutHero {
    border-radius: 14px;
}
QFrame#aboutImageFrame {
    background: #f4f6f5;
    border: 1px solid #e5ebe7;
    border-radius: 12px;
}
QLabel#aboutTitle {
    color: #183a2c;
    font-size: 26px;
    font-weight: 700;
}
QLabel#aboutSubtitle {
    color: #62736a;
    font-size: 13px;
}
QLabel#aboutBody {
    color: #42564b;
    font-size: 13px;
}
QLabel#aboutCardTitle {
    color: #183a2c;
    font-size: 15px;
    font-weight: 700;
}
QLabel#aboutInfoLabel {
    color: #6a7c72;
    font-size: 13px;
}
QLabel#aboutInfoValue {
    color: #263f31;
    font-size: 13px;
}
QLabel#aboutBadge {
    background: #eaf3ee;
    color: #24775c;
    border: 1px solid #d6e7dd;
    border-radius: 8px;
    padding: 5px 10px;
    font-size: 12px;
    font-weight: 600;
}
QWidget#aboutContent {
    background: transparent;
}
)";

QString yesNo(bool value)
{
    return value ? QObject::tr("是") : QObject::tr("否");
}

QWidget *createStaticCarImageWidget(QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *carImage = new QLabel(container);
    carImage->setAlignment(Qt::AlignCenter);
    carImage->setMinimumSize(260, 170);
    carImage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPixmap pixmap(QStringLiteral(":/image/car.jpg"));
    if (!pixmap.isNull()) {
        carImage->setPixmap(pixmap.scaled(280, 185, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        carImage->setText(QObject::tr("车辆图片未加载"));
        carImage->setStyleSheet(QStringLiteral("color: #526259; font-weight: 700;"));
    }
    layout->addWidget(carImage);
    return container;
}
}

About::About(Ui::MainWindow *ui, QObject *parent)
    : QObject(parent)
    , ui(ui)
{
    buildUi();
}

void About::buildUi()
{
    if (!ui || !ui->aboutPage) {
        return;
    }

    if (ui->aboutLabel) {
        ui->aboutLabel->hide();
    }

    auto *rootLayout = qobject_cast<QVBoxLayout *>(ui->aboutPage->layout());
    if (!rootLayout) {
        rootLayout = new QVBoxLayout(ui->aboutPage);
        ui->aboutPage->setLayout(rootLayout);
    }
    while (QLayoutItem *item = rootLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    rootLayout->setContentsMargins(20, 20, 20, 20);
    rootLayout->setSpacing(14);
    ui->aboutPage->setObjectName(QStringLiteral("aboutRoot"));
    SupportPageStyle::apply(ui->aboutPage, QString::fromUtf8(kAboutStyle));

    auto *scroll = new QScrollArea(ui->aboutPage);
    scroll->setObjectName(QStringLiteral("aboutScrollArea"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("aboutContent"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(14);

    auto *heading = new QHBoxLayout();
    auto *headingText = new QVBoxLayout();
    headingText->setSpacing(5);
    auto *title = new QLabel(tr("关于 Tenco"), content);
    title->setObjectName(QStringLiteral("aboutTitle"));
    auto *subtitle = new QLabel(tr("产品概况、版本信息与本机运行环境。"), content);
    subtitle->setObjectName(QStringLiteral("aboutSubtitle"));
    subtitle->setWordWrap(true);
    headingText->addWidget(title);
    headingText->addWidget(subtitle);
    heading->addLayout(headingText, 1);
    auto *versionBadge = new QLabel(appVersionText(), content);
    versionBadge->setObjectName(QStringLiteral("aboutBadge"));
    versionBadge->setTextInteractionFlags(Qt::TextSelectableByMouse);
    heading->addWidget(versionBadge, 0, Qt::AlignTop);
    contentLayout->addLayout(heading);

    auto *hero = new QFrame(content);
    hero->setObjectName(QStringLiteral("aboutHero"));
    hero->setProperty("supportCard", true);
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(20, 18, 20, 18);
    heroLayout->setSpacing(20);

    auto *textBlock = new QVBoxLayout();
    textBlock->setSpacing(10);
    auto *productTitle = new QLabel(tr("巡检车上位机控制与运维平台"), hero);
    productTitle->setObjectName(QStringLiteral("aboutCardTitle"));
    auto *body = new QLabel(tr("面向温室、园区与实验室巡检车的现场调试和运行，将设备监视、地图作业与维护工具集中在一个工作台。"), hero);
    body->setObjectName(QStringLiteral("aboutBody"));
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textBlock->addWidget(productTitle);
    textBlock->addWidget(body);
    textBlock->addWidget(createBulletList({tr("设备监视 · 车辆状态、视频预览、拍照与录像。"),
                                          tr("手动控制 · 底盘、云台与升降 PLC。"),
                                          tr("地图作业 · 地图编辑、示教规划、常规/直线任务下发与监视。"),
                                          tr("运行维护 · 配置热加载、备份恢复、自检与现场帮助。")}, hero));
    textBlock->addStretch(1);
    heroLayout->addLayout(textBlock, 1);

    heroLayout->addWidget(createCarVisualWidget(hero));
    contentLayout->addWidget(hero);

    const ConfigManager &config = ConfigManager::instance();
    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    auto makeButton = [&](const QString &text, const QString &objectName, const std::function<void()> &handler) {
        auto *button = new QPushButton(text, content);
        button->setObjectName(objectName);
        button->setProperty("supportPrimary", objectName == QStringLiteral("aboutPrimaryButton"));
        button->setCursor(Qt::PointingHandCursor);
        connect(button, &QPushButton::clicked, this, handler);
        buttonRow->addWidget(button);
    };
    makeButton(tr("复制诊断信息"), QStringLiteral("aboutPrimaryButton"), [this]() { copyDiagnosticInfo(); });
    makeButton(tr("程序目录"), QStringLiteral("aboutSecondaryButton"), [this]() {
        openPath(QCoreApplication::applicationDirPath(), tr("打开程序目录失败"));
    });
    makeButton(tr("配置目录"), QStringLiteral("aboutSecondaryButton"), [this, configPath = config.configFilePath()]() {
        openPath(QFileInfo(configPath).absolutePath(), tr("打开配置目录失败"));
    });
    makeButton(tr("日志目录"), QStringLiteral("aboutSecondaryButton"), [this]() {
        openPath(LoggingManager::logDirectoryPath(), tr("打开日志目录失败"));
    });
    buttonRow->addStretch(1);
    contentLayout->addLayout(buttonRow);

    auto *grid = new QHBoxLayout();
    grid->setSpacing(14);

    auto *versionCard = createCard(tr("版本与构建"));
    auto *versionLayout = qobject_cast<QVBoxLayout *>(versionCard->layout());
    versionLayout->addWidget(createInfoRow(tr("应用版本"), appVersionText(), versionCard));
    versionLayout->addWidget(createInfoRow(tr("Qt 版本"), QString::fromLatin1(qVersion()), versionCard));
    versionLayout->addWidget(createInfoRow(tr("构建时间"), QStringLiteral("%1 %2").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)), versionCard));
    versionLayout->addWidget(createInfoRow(tr("Git 提交"), tr("未注入"), versionCard));
    versionLayout->addWidget(createInfoRow(tr("OAK 相机后端"), oakBackendStatus(), versionCard));
    versionLayout->addWidget(createInfoRow(tr("关于页三维模型"), about3dStatus(), versionCard));
    versionLayout->addStretch(1);
    grid->addWidget(versionCard, 2);

    auto *runtimeCard = createCard(tr("本机运行环境"));
    auto *runtimeLayout = qobject_cast<QVBoxLayout *>(runtimeCard->layout());
    runtimeLayout->addWidget(createInfoRow(tr("操作系统"), QSysInfo::prettyProductName(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("CPU 架构"), QSysInfo::currentCpuArchitecture(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("程序目录"), QCoreApplication::applicationDirPath(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("配置文件"), config.configFilePath(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("配置来自文件"), yesNo(config.loadedFromFile()), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("备份目录"), config.backupDirectoryPath(), runtimeCard));
    auto *logDirectoryRow = createInfoRow(tr("日志目录"), LoggingManager::logDirectoryPath(), runtimeCard);
    logDirectoryRow->findChild<QLabel *>(QStringLiteral("aboutInfoValue"))->setObjectName(QStringLiteral("aboutLogDirectory"));
    runtimeLayout->addWidget(logDirectoryRow);
    runtimeLayout->addWidget(createInfoRow(tr("运行日志"), LoggingManager::currentLogFilePath(), runtimeCard));
    grid->addWidget(runtimeCard, 3);
    contentLayout->addLayout(grid);

    auto *maintenanceCard = createCard(tr("维护与第三方说明"));
    auto *maintenanceLayout = qobject_cast<QVBoxLayout *>(maintenanceCard->layout());
    maintenanceLayout->addWidget(createBulletList({tr("本系统面向项目组内部研发、调试和现场运行使用。"),
                                                   tr("自动运行、云台动作和底盘控制前，应确认急停、遥控器和现场环境安全。"),
                                                   tr("提交问题时请提供版本信息、配置文件路径、相关日志、复现步骤和截图/录像。"),
                                                   tr("第三方组件包括 Qt；OAK 后端启用时使用 DepthAI/OpenCV，相关版权与许可归各权利人所有。")},
                                                  maintenanceCard));
    contentLayout->addWidget(maintenanceCard);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    rootLayout->addWidget(scroll);
}

QWidget *About::createCard(const QString &title, QWidget *content) const
{
    auto *card = new QFrame();
    card->setObjectName(QStringLiteral("aboutCard"));
    card->setProperty("supportCard", true);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setObjectName(QStringLiteral("aboutCardTitle"));
    layout->addWidget(titleLabel);
    if (content) {
        layout->addWidget(content);
    }
    return card;
}

QWidget *About::createInfoRow(const QString &label, const QString &value, QWidget *parent) const
{
    auto *row = new QWidget(parent);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto *labelWidget = new QLabel(label, row);
    labelWidget->setObjectName(QStringLiteral("aboutInfoLabel"));
    labelWidget->setFixedWidth(98);
    labelWidget->setWordWrap(true);
    labelWidget->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *valueWidget = new QLabel(value, row);
    valueWidget->setObjectName(QStringLiteral("aboutInfoValue"));
    valueWidget->setMinimumWidth(0);
    valueWidget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    valueWidget->setWordWrap(true);
    valueWidget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(labelWidget);
    layout->addWidget(valueWidget, 1);
    return row;
}

QWidget *About::createBulletList(const QStringList &items, QWidget *parent) const
{
    auto *label = new QLabel(parent);
    QStringList lines;
    lines.reserve(items.size());
    for (const QString &item : items) {
        lines << QStringLiteral("• %1").arg(item);
    }
    label->setText(lines.join(QStringLiteral("\n")));
    label->setObjectName(QStringLiteral("aboutBody"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QWidget *About::createCarVisualWidget(QWidget *parent) const
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("aboutImageFrame"));
    frame->setMinimumSize(288, 204);
    frame->setMaximumWidth(330);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

#ifdef TENCO_ENABLE_ABOUT_3D
    auto *stack = new QStackedWidget(frame);
    stack->setMinimumSize(260, 190);
    stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *viewer = new QQuickWidget(stack);
    viewer->setResizeMode(QQuickWidget::SizeRootObjectToView);
    viewer->setClearColor(QColor(QStringLiteral("#f4f6f5")));
    const QUrl modelSource = aboutModelSourceUrl();

    auto *fallback = createStaticCarImageWidget(stack);
    stack->addWidget(viewer);
    stack->addWidget(fallback);
    stack->setCurrentWidget(viewer);

    connect(viewer, &QQuickWidget::statusChanged, stack, [stack, viewer, modelSource](QQuickWidget::Status status) {
        if (status == QQuickWidget::Ready) {
            if (viewer->rootObject()) {
                viewer->rootObject()->setProperty("aboutCarModelSource", modelSource);
            }
            return;
        }

        if (status != QQuickWidget::Error) {
            return;
        }

        for (const QQmlError &error : viewer->errors()) {
            qWarning("%s", qPrintable(error.toString()));
        }
        stack->setCurrentIndex(1);
    });
    qInfo("About 3D model source: %s", qPrintable(QDir::toNativeSeparators(aboutModelFilePath())));
    viewer->setSource(QUrl(QStringLiteral("qrc:/qml/AboutCarViewer.qml")));
    if (viewer->rootObject()) {
        viewer->rootObject()->setProperty("aboutCarModelSource", modelSource);
    }

    layout->addWidget(stack, 1);
#else
    layout->addWidget(createStaticCarImageWidget(frame), 1);
#endif

    return frame;
}

QString About::diagnosticText() const
{
    const ConfigManager &config = ConfigManager::instance();
    return QStringList{
        QStringLiteral("Tenco diagnostic information"),
        QStringLiteral("Application version: %1").arg(appVersionText()),
        QStringLiteral("Qt version: %1").arg(QString::fromLatin1(qVersion())),
        QStringLiteral("Build time: %1 %2").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)),
        QStringLiteral("Git commit: 未注入"),
        QStringLiteral("OAK backend: %1").arg(oakBackendStatus()),
        QStringLiteral("About 3D model: %1").arg(about3dStatus()),
        QStringLiteral("OS: %1").arg(QSysInfo::prettyProductName()),
        QStringLiteral("CPU architecture: %1").arg(QSysInfo::currentCpuArchitecture()),
        QStringLiteral("Application dir: %1").arg(QCoreApplication::applicationDirPath()),
        QStringLiteral("Config file: %1").arg(config.configFilePath()),
        QStringLiteral("Config loaded from file: %1").arg(yesNo(config.loadedFromFile())),
        QStringLiteral("Backup dir: %1").arg(config.backupDirectoryPath()),
        QStringLiteral("Log dir: %1").arg(LoggingManager::logDirectoryPath()),
        QStringLiteral("Current log: %1").arg(LoggingManager::currentLogFilePath())
    }.join(QStringLiteral("\n"));
}

QString About::aboutModelFilePath() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/carmodel_about_centered.glb"));
}

QUrl About::aboutModelSourceUrl() const
{
    const QString path = aboutModelFilePath();
    if (QFileInfo::exists(path)) {
        qInfo("About 3D model source: %s", qPrintable(QDir::toNativeSeparators(path)));
    } else {
        qWarning("About 3D model file missing: %s", qPrintable(QDir::toNativeSeparators(path)));
    }
    return QUrl::fromLocalFile(path);
}

QString About::oakBackendStatus() const
{
#ifdef TENCO_ENABLE_OAK_CAMERA
    return tr("启用");
#else
    return tr("未启用");
#endif
}

QString About::about3dStatus() const
{
#ifdef TENCO_ENABLE_ABOUT_3D
    return tr("启用");
#else
    return tr("静态图片兜底");
#endif
}

QString About::appVersionText() const
{
    const QString version = QCoreApplication::applicationVersion().trimmed();
    return version.isEmpty() ? tr("开发构建") : version;
}

void About::copyDiagnosticInfo()
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        QMessageBox::warning(ui ? ui->aboutPage : nullptr, tr("复制失败"), tr("系统剪贴板不可用"));
        return;
    }

    clipboard->setText(diagnosticText());
    QMessageBox::information(ui ? ui->aboutPage : nullptr, tr("已复制"), tr("诊断信息已复制到剪贴板"));
}

void About::openPath(const QString &path, const QString &errorTitle) const
{
    const QFileInfo info(path);
    if (!info.exists()) {
        QMessageBox::warning(ui ? ui->aboutPage : nullptr,
                             errorTitle,
                             tr("路径不存在：\n%1").arg(QDir::toNativeSeparators(path)));
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(info.absoluteFilePath()))) {
        QMessageBox::warning(ui ? ui->aboutPage : nullptr,
                             errorTitle,
                             tr("无法打开路径：\n%1").arg(QDir::toNativeSeparators(path)));
    }
}

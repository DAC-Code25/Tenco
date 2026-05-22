#include "about.h"

#include "configmanager.h"
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
#include <QStandardPaths>
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
QWidget#aboutRoot {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #f5f7f2, stop:0.55 #eef3eb, stop:1 #e5ece3);
}
QFrame#aboutHero, QFrame#aboutCard {
    background: rgba(255, 255, 255, 0.97);
    border: 1px solid rgba(124, 143, 126, 0.22);
    border-radius: 18px;
}
QFrame#aboutImageFrame {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #fbfcf8, stop:1 #eef4ec);
    border: 1px solid rgba(124, 143, 126, 0.22);
    border-radius: 16px;
}
QLabel#aboutTitle {
    color: #132019;
    font-size: 32px;
    font-weight: 900;
}
QLabel#aboutSubtitle {
    color: #415248;
    font-size: 15px;
    font-weight: 600;
}
QLabel#aboutBody {
    color: #33483c;
    font-size: 14px;
    line-height: 165%;
}
QLabel#aboutCardTitle {
    color: #1f3128;
    font-size: 17px;
    font-weight: 850;
}
QLabel#aboutInfoLabel {
    color: #617267;
    font-size: 12px;
    font-weight: 750;
}
QLabel#aboutInfoValue {
    color: #1f3128;
    font-size: 13px;
    font-weight: 650;
}
QLabel#aboutBadge {
    background: rgba(47, 125, 100, 0.12);
    color: #1f5f4b;
    border: 1px solid rgba(47, 125, 100, 0.24);
    border-radius: 10px;
    padding: 7px 11px;
    font-size: 12px;
    font-weight: 800;
}
QPushButton#aboutSecondaryButton {
    background: white;
    color: #3e5146;
    border: 1px solid #bdcbbb;
    border-radius: 11px;
    padding: 10px 18px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 700;
}
QPushButton#aboutSecondaryButton:hover {
    background: #f6faf4;
}
QPushButton#aboutPrimaryButton {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2f7d64, stop:1 #1f5f4b);
    color: white;
    border: none;
    border-radius: 11px;
    padding: 10px 20px;
    min-height: 36px;
    font-size: 13px;
    font-weight: 850;
}
QPushButton#aboutPrimaryButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b9274, stop:1 #287257);
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
    carImage->setMinimumSize(300, 205);
    carImage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QPixmap pixmap(QStringLiteral(":/image/car.jpg"));
    if (!pixmap.isNull()) {
        carImage->setPixmap(pixmap.scaled(360, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
    rootLayout->setSpacing(16);
    ui->aboutPage->setObjectName(QStringLiteral("aboutRoot"));
    ui->aboutPage->setStyleSheet(QString::fromUtf8(kAboutStyle));

    auto *scroll = new QScrollArea(ui->aboutPage);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *content = new QWidget(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(16);

    auto *hero = new QFrame(content);
    hero->setObjectName(QStringLiteral("aboutHero"));
    auto *heroLayout = new QHBoxLayout(hero);
    heroLayout->setContentsMargins(24, 22, 24, 22);
    heroLayout->setSpacing(22);

    auto *textBlock = new QVBoxLayout();
    textBlock->setSpacing(12);
    auto *title = new QLabel(tr("Tenco"), hero);
    title->setObjectName(QStringLiteral("aboutTitle"));
    auto *subtitle = new QLabel(tr("巡检车上位机控制与运维平台"), hero);
    subtitle->setObjectName(QStringLiteral("aboutSubtitle"));
    auto *body = new QLabel(tr("面向巡检车现场调试、地图规划、视频预览、云台控制、直线/多垄作业和生产维护的桌面端系统。"), hero);
    body->setObjectName(QStringLiteral("aboutBody"));
    body->setWordWrap(true);
    body->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textBlock->addWidget(title);
    textBlock->addWidget(subtitle);
    textBlock->addWidget(body);

    auto *badgeRow = new QHBoxLayout();
    badgeRow->setSpacing(8);
    const QStringList badges{tr("Qt %1").arg(QString::fromLatin1(qVersion())),
                             appVersionText(),
                             tr("OAK 后端：%1").arg(oakBackendStatus()),
                             tr("车辆模型：%1").arg(about3dStatus())};
    for (const QString &badgeText : badges) {
        auto *badge = new QLabel(badgeText, hero);
        badge->setObjectName(QStringLiteral("aboutBadge"));
        badgeRow->addWidget(badge);
    }
    badgeRow->addStretch(1);
    textBlock->addLayout(badgeRow);
    heroLayout->addLayout(textBlock, 1);

    heroLayout->addWidget(createCarVisualWidget(hero));
    contentLayout->addWidget(hero);

    auto *grid = new QHBoxLayout();
    grid->setSpacing(16);

    auto *leftColumn = new QVBoxLayout();
    leftColumn->setSpacing(16);
    auto *productCard = createCard(tr("产品信息"));
    auto *productLayout = qobject_cast<QVBoxLayout *>(productCard->layout());
    productLayout->addWidget(createInfoRow(tr("产品名称"), tr("Tenco"), productCard));
    productLayout->addWidget(createInfoRow(tr("系统定位"), tr("巡检车上位机控制与运维平台"), productCard));
    productLayout->addWidget(createInfoRow(tr("适用场景"), tr("温室、园区、实验室巡检车调试与运行"), productCard));
    productLayout->addWidget(createBulletList({tr("车辆状态监控、视频预览和拍照录像。"),
                                               tr("云台/升降 PLC 控制和底盘手动控制。"),
                                               tr("地图路径规划、路径跟踪、直线/多垄作业。"),
                                               tr("运行配置维护、备份恢复、热加载和现场排障。")},
                                              productCard));
    leftColumn->addWidget(productCard);

    auto *capabilityCard = createCard(tr("核心能力"));
    auto *capabilityLayout = qobject_cast<QVBoxLayout *>(capabilityCard->layout());
    capabilityLayout->addWidget(createBulletList({tr("首页：状态轮询、视频流切换、相机控制、云台控制、底盘手动控制。"),
                                                  tr("地图：地图编辑、路径规划、路径执行、直线/多垄作业计划。"),
                                                  tr("维护：配置保存、保存并应用、启动备份、恢复上次配置、配置自检。"),
                                                  tr("帮助：现场手册、快捷键、故障分级、运维巡检和文档资料入口。")},
                                                 capabilityCard));
    leftColumn->addWidget(capabilityCard);
    grid->addLayout(leftColumn, 1);

    auto *rightColumn = new QVBoxLayout();
    rightColumn->setSpacing(16);
    auto *versionCard = createCard(tr("版本与构建"));
    auto *versionLayout = qobject_cast<QVBoxLayout *>(versionCard->layout());
    versionLayout->addWidget(createInfoRow(tr("应用版本"), appVersionText(), versionCard));
    versionLayout->addWidget(createInfoRow(tr("Qt 版本"), QString::fromLatin1(qVersion()), versionCard));
    versionLayout->addWidget(createInfoRow(tr("构建时间"), QStringLiteral("%1 %2").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)), versionCard));
    versionLayout->addWidget(createInfoRow(tr("Git 提交"), tr("未注入"), versionCard));
    versionLayout->addWidget(createInfoRow(tr("OAK 相机后端"), oakBackendStatus(), versionCard));
    versionLayout->addWidget(createInfoRow(tr("关于页三维模型"), about3dStatus(), versionCard));
    rightColumn->addWidget(versionCard);

    const ConfigManager &config = ConfigManager::instance();
    auto *runtimeCard = createCard(tr("运行环境"));
    auto *runtimeLayout = qobject_cast<QVBoxLayout *>(runtimeCard->layout());
    runtimeLayout->addWidget(createInfoRow(tr("操作系统"), QSysInfo::prettyProductName(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("CPU 架构"), QSysInfo::currentCpuArchitecture(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("程序目录"), QCoreApplication::applicationDirPath(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("配置文件"), config.configFilePath(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("配置来自文件"), yesNo(config.loadedFromFile()), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("备份目录"), config.backupDirectoryPath(), runtimeCard));
    runtimeLayout->addWidget(createInfoRow(tr("日志目录"), appDataPath(QStringLiteral("logs")), runtimeCard));
    rightColumn->addWidget(runtimeCard);
    grid->addLayout(rightColumn, 1);
    contentLayout->addLayout(grid);

    auto *maintenanceCard = createCard(tr("版权与维护说明"));
    auto *maintenanceLayout = qobject_cast<QVBoxLayout *>(maintenanceCard->layout());
    maintenanceLayout->addWidget(createBulletList({tr("本系统面向项目组内部研发、调试和现场运行使用。"),
                                                   tr("自动运行、云台动作和底盘控制前，应确认急停、遥控器和现场环境安全。"),
                                                   tr("提交问题时请提供版本信息、配置文件路径、相关日志、复现步骤和截图/录像。"),
                                                   tr("第三方依赖包括 Qt；OAK 构建启用时还依赖 DepthAI/OpenCV 运行环境。")},
                                                  maintenanceCard));
    contentLayout->addWidget(maintenanceCard);

    auto *actionsCard = createCard(tr("诊断与目录"));
    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    auto makeButton = [&](const QString &text, const QString &objectName, const std::function<void()> &handler) {
        auto *button = new QPushButton(text, actionsCard);
        button->setObjectName(objectName);
        connect(button, &QPushButton::clicked, this, handler);
        buttonRow->addWidget(button);
    };
    makeButton(tr("复制诊断信息"), QStringLiteral("aboutPrimaryButton"), [this]() { copyDiagnosticInfo(); });
    makeButton(tr("打开程序目录"), QStringLiteral("aboutSecondaryButton"), [this]() {
        openPath(QCoreApplication::applicationDirPath(), tr("打开程序目录失败"));
    });
    makeButton(tr("打开配置目录"), QStringLiteral("aboutSecondaryButton"), [this, configPath = config.configFilePath()]() {
        openPath(QFileInfo(configPath).absolutePath(), tr("打开配置目录失败"));
    });
    makeButton(tr("打开日志目录"), QStringLiteral("aboutSecondaryButton"), [this]() {
        openPath(appDataPath(QStringLiteral("logs")), tr("打开日志目录失败"));
    });
    buttonRow->addStretch(1);
    qobject_cast<QVBoxLayout *>(actionsCard->layout())->addLayout(buttonRow);
    contentLayout->addWidget(actionsCard);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    rootLayout->addWidget(scroll);
}

QWidget *About::createCard(const QString &title, QWidget *content) const
{
    auto *card = new QFrame();
    card->setObjectName(QStringLiteral("aboutCard"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(22, 20, 22, 20);
    layout->setSpacing(13);
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
    labelWidget->setMinimumWidth(110);
    auto *valueWidget = new QLabel(value, row);
    valueWidget->setObjectName(QStringLiteral("aboutInfoValue"));
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
    label->setText(lines.join(QStringLiteral("\n\n")));
    label->setObjectName(QStringLiteral("aboutBody"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QWidget *About::createCarVisualWidget(QWidget *parent) const
{
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("aboutImageFrame"));
    frame->setMinimumSize(330, 258);
    frame->setMaximumWidth(450);
    frame->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(8);

#ifdef TENCO_ENABLE_ABOUT_3D
    auto *stack = new QStackedWidget(frame);
    stack->setMinimumSize(340, 280);
    stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto *viewer = new QQuickWidget(stack);
    viewer->setResizeMode(QQuickWidget::SizeRootObjectToView);
    viewer->setClearColor(QColor(QStringLiteral("#f4f7f1")));
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
        QStringLiteral("Log dir: %1").arg(appDataPath(QStringLiteral("logs")))
    }.join(QStringLiteral("\n"));
}

QString About::appDataPath(const QString &child) const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return child.isEmpty() ? base : QDir(base).filePath(child);
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

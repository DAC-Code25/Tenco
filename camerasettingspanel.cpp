#include "camerasettingspanel.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

CameraSettingsPanel::CameraSettingsPanel(QWidget *parent) : QWidget(parent) {
    setObjectName("cameraSettingsPanel");
    auto *layout = new QVBoxLayout(this);
    layout->setAlignment(Qt::AlignTop); layout->setSpacing(12);
    auto *title = new QLabel(tr("工控机相机 · 采集与保存参数"), this);
    title->setObjectName("maintenancePageTitle"); layout->addWidget(title);
    auto *hint = new QLabel(tr("先读取相机参数，修改后点击“应用到工控机”。应用会短暂重连预览，录像期间请先停止录像。照片帧率表示照片源刷新频率，拍照仍由单次指令触发。"), this);
    hint->setWordWrap(true); layout->addWidget(hint);
    m_native = new QLabel(tr("尚未读取相机能力"), this); m_native->setWordWrap(true); layout->addWidget(m_native);
    auto *form = new QFormLayout; layout->addLayout(form);
    m_sensorFps = new QDoubleSpinBox(this); m_sensorFps->setObjectName("cameraSensorFps");
    m_sensorFps->setDecimals(2); m_sensorFps->setRange(1.42,30); m_sensorFps->setValue(30); m_sensorFps->setSuffix(" FPS");
    form->addRow(tr("原生传感器帧率"), m_sensorFps);
    const QStringList names{tr("预览"),tr("照片"),tr("录像")};
    const QStringList keys{"preview","photo","recording"};
    for (int i=0;i<3;++i) {
        auto *row = new QWidget(this); auto *box = new QHBoxLayout(row); box->setContentsMargins(0,0,0,0);
        m_size[i] = new QComboBox(row); m_size[i]->setObjectName("cameraSize_"+keys[i]);
        m_fps[i] = new QDoubleSpinBox(row); m_fps[i]->setObjectName("cameraFps_"+keys[i]);
        m_fps[i]->setDecimals(2); m_fps[i]->setRange(1,30); m_fps[i]->setSuffix(" FPS");
        box->addWidget(m_size[i],1); box->addWidget(m_fps[i]);
        form->addRow(names[i]+tr("尺寸 / 帧率"),row);
    }
    auto *buttons = new QHBoxLayout;
    m_read = new QPushButton(tr("读取工控机参数"),this); m_read->setObjectName("cameraSettingsRead");
    m_apply = new QPushButton(tr("应用到工控机"),this); m_apply->setObjectName("cameraSettingsApply");
    buttons->addWidget(m_read); buttons->addWidget(m_apply); buttons->addStretch(); layout->addLayout(buttons);
    m_result = new QLabel(tr("修改只在点击“应用到工控机”后生效；页面底部的本地配置保存按钮不下发相机参数。"),this);
    m_result->setObjectName("cameraSettingsResult"); m_result->setWordWrap(true); layout->addWidget(m_result);
    m_runtime = new QLabel(this); m_runtime->setObjectName("cameraSettingsRuntime"); m_runtime->setWordWrap(true); layout->addWidget(m_runtime);
    connect(m_read,&QPushButton::clicked,this,[this] { if (prepareConnection()) m_client.read(); });
    connect(m_apply,&QPushButton::clicked,this,[this] {
        if (!prepareConnection()) return;
        if (m_config.isEmpty()) { m_result->setText(tr("连接地址已变化，请先重新读取。")); return; }
        for (int i=0;i<3;++i) if (m_fps[i]->value() > m_sensorFps->value()) {
            m_result->setText(tr("输出帧率不能超过传感器帧率。")); return;
        }
        if (m_client.apply(editedSettings())) m_result->setText(tr("正在等待工控机应用参数……"));
    });
    connect(&m_client,&CameraSettingsClient::loaded,this,&CameraSettingsPanel::load);
    connect(&m_client,&CameraSettingsClient::busyChanged,this,[this] { updateEnabled(); });
    connect(&m_client,&CameraSettingsClient::finished,this,[this](bool ok,const QString &message) {
        m_config=m_client.configuration(); m_result->setText(message);
        if (!ok && m_config.isEmpty()) m_ready=false;
        updateEnabled();
    });
    connect(&m_client,&CameraSettingsClient::statusReceived,this,[this](const QJsonObject &status) {
        m_ready=status.value("cameraConnected").toBool();
        const auto remote=status.value("mediaConfiguration").toObject();
        if (!remote.isEmpty() && remote.value("revision")!=m_config.value("revision")) {
            m_ready=false; m_result->setText(tr("工控机参数已变化，请重新读取后编辑。"));
        }
        const bool recording=status.value("recording").toBool(); m_config["recording"]=recording;
        m_runtime->setText(status.isEmpty() ? tr("相机状态暂不可达，请检查连接。") :
            tr("相机%1；%2；录像实测 %3 FPS，已写入 %4 帧，丢弃 %5 帧。")
            .arg(m_ready?tr("在线"):tr("未就绪"),recording?tr("正在录像"):tr("未录像"))
            .arg(status.value("recordingActualFps").toDouble(),0,'f',2)
            .arg(status.value("recordingFrames").toDouble(),0,'f',0).arg(status.value("recordingDroppedFrames").toDouble(),0,'f',0));
        updateEnabled();
    });
    auto *monitor=new QTimer(this); monitor->setInterval(1000);
    connect(monitor,&QTimer::timeout,this,[this] { if(isVisible() && !m_config.isEmpty()) m_client.refreshStatus(); }); monitor->start();
    updateEnabled();
}
bool CameraSettingsPanel::prepareConnection() {
    if (!m_provider) { m_result->setText(tr("尚未配置相机地址")); return false; }
    const auto next=m_provider();
    if (next.url!=m_connection.url || next.token!=m_connection.token || next.timeoutMs!=m_connection.timeoutMs) {
        m_client.configure(next.url,next.token,next.timeoutMs); m_connection=next; m_config={}; m_ready=false;
    }
    return true;
}
void CameraSettingsPanel::load(const QJsonObject &config,const QJsonObject &caps) {
    m_config=config; m_ready=config.value("cameraConnected").toBool();
    const auto native=caps.value("native").toObject();
    m_native->setText(tr("%1：原生 %2 × %3，%4～%5 FPS。尺寸比例不同时保留完整视野并补黑边；高分辨率会增加负载。")
        .arg(caps.value("sensorName").toString()).arg(native.value("width").toInt()).arg(native.value("height").toInt())
        .arg(native.value("minFps").toDouble()).arg(native.value("maxFps").toDouble()));
    m_sensorFps->setRange(native.value("minFps").toDouble(1.42),native.value("maxFps").toDouble(30));
    const auto settings=config.value("settings").toObject(); m_sensorFps->setValue(settings.value("sensorFps").toDouble());
    const QStringList keys{"preview","photo","recording"};
    for (int i=0;i<3;++i) {
        m_size[i]->clear();
        if (i>0) m_size[i]->addItem(tr("原生尺寸"),QJsonObject{{"width",0},{"height",0}}.toVariantMap());
        for (const auto &value:caps.value("resolutions").toArray()) {
            const auto r=value.toObject(); const int w=r.value("width").toInt(),h=r.value("height").toInt();
            if (i==0 && (w>caps.value("previewMaxWidth").toInt(1920)||h>caps.value("previewMaxHeight").toInt(1080))) continue;
            m_size[i]->addItem(QString::number(w)+" × "+QString::number(h),r.toVariantMap());
        }
        const auto output=settings.value(keys[i]).toObject();
        const QVariantMap size{{"width",output.value("width").toInt()},{"height",output.value("height").toInt()}};
        int index=m_size[i]->findData(size);
        if (index<0) { m_size[i]->addItem(QString::number(output.value("width").toInt())+" × "+QString::number(output.value("height").toInt()),size); index=m_size[i]->count()-1; }
        m_size[i]->setCurrentIndex(index); m_fps[i]->setMaximum(native.value("maxFps").toDouble(30)); m_fps[i]->setValue(output.value("fps").toDouble());
    }
    updateEnabled();
}
QJsonObject CameraSettingsPanel::editedSettings() const {
    QJsonObject result{{"sensorFps",m_sensorFps->value()}};
    const QStringList keys{"preview","photo","recording"};
    for (int i=0;i<3;++i) {
        auto output=QJsonObject::fromVariantMap(m_size[i]->currentData().toMap()); output["fps"]=m_fps[i]->value(); result[keys[i]]=output;
    }
    return result;
}
void CameraSettingsPanel::updateEnabled() {
    const bool busy=m_client.busy(),editable=!m_config.isEmpty()&&!busy;
    m_read->setEnabled(!busy); m_sensorFps->setEnabled(editable);
    for(int i=0;i<3;++i) { m_size[i]->setEnabled(editable);m_fps[i]->setEnabled(editable); }
    m_apply->setEnabled(editable && m_ready && !m_config.value("recording").toBool() && m_config.value("applyState")!="pending");
}

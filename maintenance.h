#ifndef MAINTENANCE_H
#define MAINTENANCE_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QStringList>

#include "configmanager.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QFormLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QLabel;
class QWidget;

namespace Ui {
class MainWindow;
}

class Maintenance : public QObject
{
    Q_OBJECT

public:
    explicit Maintenance(Ui::MainWindow *ui, QObject *parent = nullptr);
    bool confirmLeaveIfDirty(QWidget *parentWidget = nullptr);
    bool hasUnsavedChanges() const;

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void buildUi();
    void loadFromConfig();
    bool saveConfig(bool applyAfterSave);
    void loadSnapshotToForm(const ConfigManager::ConfigSnapshot &snapshot);
    void loadConfigFile();
    void restoreLatestBackup();
    void runConfigSelfCheck();
    void updateActionState();
    void markDirty();
    QJsonObject snapshotFingerprint(const ConfigManager::ConfigSnapshot &snapshot) const;
    ConfigManager::ConfigSnapshot collectSnapshot() const;
    bool validateSnapshot(const ConfigManager::ConfigSnapshot &snapshot, QString *errorMessage) const;

    QWidget *createNetworkPage();
    QWidget *createVideoPage();
    QWidget *createGimbalPage();
    QWidget *createRowWorkPage();
    QWidget *createControlPage();
    QWidget *createVehiclePage();
    QWidget *createProductionPage();
    QWidget *createFormPage(const QString &title, const QString &description, QFormLayout **outLayout) const;
    void refreshProductionInfo();

    QLineEdit *addLineEdit(QFormLayout *layout, const QString &key, const QString &label, const QString &placeholder = QString());
    QSpinBox *addSpinBox(QFormLayout *layout, const QString &key, const QString &label, int min, int max, const QString &suffix = QString());
    QDoubleSpinBox *addDoubleSpinBox(QFormLayout *layout, const QString &key, const QString &label, double min, double max, int decimals, const QString &suffix = QString());
    QCheckBox *addCheckBox(QFormLayout *layout, const QString &key, const QString &label);
    QComboBox *addComboBox(QFormLayout *layout, const QString &key, const QString &label, const QStringList &items);
    QPlainTextEdit *addPlainTextEdit(QFormLayout *layout, const QString &key, const QString &label, const QString &placeholder = QString());

    QString lineValue(const QString &key) const;
    int intValue(const QString &key) const;
    double doubleValue(const QString &key) const;
    bool boolValue(const QString &key) const;
    QString comboValue(const QString &key) const;
    QString plainTextValue(const QString &key) const;

    void setLineValue(const QString &key, const QString &value);
    void setIntValue(const QString &key, int value);
    void setDoubleValue(const QString &key, double value);
    void setBoolValue(const QString &key, bool value);
    void setComboValue(const QString &key, const QString &value);
    void setPlainTextValue(const QString &key, const QString &value);

    Ui::MainWindow *ui;
    QLabel *m_statusLabel = nullptr;
    QPushButton *m_loadConfigButton = nullptr;
    QPushButton *m_restoreBackupButton = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_applyButton = nullptr;
    QLabel *m_configPathValue = nullptr;
    QLabel *m_backupDirValue = nullptr;
    QLabel *m_backupCountValue = nullptr;
    QLabel *m_latestBackupValue = nullptr;
    QLabel *m_runtimeDirValue = nullptr;
    QLabel *m_loadedFromFileValue = nullptr;
    QJsonObject m_cleanFingerprint;
    bool m_loading = false;
    QHash<QString, QLineEdit *> m_lineEdits;
    QHash<QString, QSpinBox *> m_spinBoxes;
    QHash<QString, QDoubleSpinBox *> m_doubleSpinBoxes;
    QHash<QString, QCheckBox *> m_checkBoxes;
    QHash<QString, QComboBox *> m_comboBoxes;
    QHash<QString, QPlainTextEdit *> m_plainTextEdits;
};

#endif // MAINTENANCE_H


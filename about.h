#ifndef ABOUT_H
#define ABOUT_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QLabel;
class QWidget;

namespace Ui {
class MainWindow;
}

class About : public QObject
{
    Q_OBJECT

public:
    explicit About(Ui::MainWindow *ui, QObject *parent = nullptr);

private:
    void buildUi();
    QWidget *createCard(const QString &title, QWidget *content = nullptr) const;
    QWidget *createInfoRow(const QString &label, const QString &value, QWidget *parent = nullptr) const;
    QWidget *createBulletList(const QStringList &items, QWidget *parent = nullptr) const;
    QWidget *createCarVisualWidget(QWidget *parent = nullptr) const;
    QString diagnosticText() const;
    QString appDataPath(const QString &child = QString()) const;
    QString aboutModelFilePath() const;
    QUrl aboutModelSourceUrl() const;
    QString oakBackendStatus() const;
    QString about3dStatus() const;
    QString appVersionText() const;
    void copyDiagnosticInfo();
    void openPath(const QString &path, const QString &errorTitle) const;

    Ui::MainWindow *ui;
};

#endif // ABOUT_H


#ifndef HELP_H
#define HELP_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QWidget;

namespace Ui {
class MainWindow;
}

class Help : public QObject
{
    Q_OBJECT

public:
    explicit Help(Ui::MainWindow *ui, QObject *parent = nullptr);

private:
    struct DocumentLink {
        QString title;
        QString description;
        QString relativePath;
    };

    void buildUi();
    QWidget *createThemePage(const QString &title, const QString &description);
    QWidget *createQuickStartPage();
    QWidget *createOverviewPage();
    QWidget *createSafetyPage();
    QWidget *createSopPage();
    QWidget *createHomePage();
    QWidget *createMapWorkPage();
    QWidget *createMaintenancePage();
    QWidget *createConnectionPage();
    QWidget *createOperationsPage();
    QWidget *createTroubleshootingPage();
    QWidget *createShortcutPage();
    QWidget *createDocumentPage();
    QWidget *createDataLogPage();
    QWidget *createRuntimePage();

    QWidget *createCard(const QString &title, const QStringList &lines, QWidget *parent = nullptr) const;
    QWidget *createStepCard(const QString &title, const QStringList &steps, QWidget *parent = nullptr) const;
    QWidget *createInfoRow(const QString &label, const QString &value, QWidget *parent = nullptr) const;
    void addTopic(const QString &title, QWidget *page);
    void openPath(const QString &path, const QString &errorTitle) const;
    QString appDataPath(const QString &child) const;
    QString repositoryPath(const QString &relativePath = QString()) const;

    Ui::MainWindow *ui;
    QListWidget *m_navList = nullptr;
    QStackedWidget *m_stack = nullptr;
    QVector<DocumentLink> m_documents;
};

#endif // HELP_H


#ifndef HELP_H
#define HELP_H

#include <QObject>

namespace Ui {
class MainWindow;
}

class Help : public QObject
{
    Q_OBJECT

public:
    explicit Help(Ui::MainWindow *ui, QObject *parent = nullptr);

private:
    Ui::MainWindow *ui;
};

#endif // HELP_H


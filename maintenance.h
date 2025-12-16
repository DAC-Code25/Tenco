#ifndef MAINTENANCE_H
#define MAINTENANCE_H

#include <QObject>

namespace Ui {
class MainWindow;
}

class Maintenance : public QObject
{
    Q_OBJECT

public:
    explicit Maintenance(Ui::MainWindow *ui, QObject *parent = nullptr);

private:
    Ui::MainWindow *ui;
};

#endif // MAINTENANCE_H


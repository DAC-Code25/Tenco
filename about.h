#ifndef ABOUT_H
#define ABOUT_H

#include <QObject>

namespace Ui {
class MainWindow;
}

class About : public QObject
{
    Q_OBJECT

public:
    explicit About(Ui::MainWindow *ui, QObject *parent = nullptr);

private:
    Ui::MainWindow *ui;
};

#endif // ABOUT_H


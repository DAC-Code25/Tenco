#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setWindowIcon(QIcon(":/icon.ico"));
    MainWindow w;
    w.setWindowFlags(Qt::FramelessWindowHint); // 移除系统默认标题栏

    w.show();
    return a.exec();
}

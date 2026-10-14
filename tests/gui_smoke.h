#pragma once
#include "mainwindow.h"
#include <QApplication>
#include <QDir>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <memory>

inline void installGuiSmoke(QApplication& app, MainWindow& window, const QString& output) {
    QDir().mkpath(output);
    auto phase = std::make_shared<int>(0);
    auto* timer = new QTimer(&window);
    auto click = [&window](const char* name) {
        auto* button = window.findChild<QPushButton*>(name);
        if (!button) qFatal("Missing navigation button");
        button->click();
    };
    auto capture = [&window, output](const QString& name) {
        if (!window.grab().save(QDir(output).filePath(name + ".png"))) qFatal("Screenshot failed");
    };
    QObject::connect(timer, &QTimer::timeout, &window, [&, phase, timer, click, capture] {
        switch ((*phase)++) {
        case 0: window.resize(1280,900); click("homeButton"); break;
        case 1: capture("home"); click("mapButton"); break;
        case 2: capture("map-route"); window.findChild<QTabWidget*>("mapTaskTabWidget")->setCurrentIndex(1); break;
        case 3: capture("map-row"); window.findChild<QTabWidget*>("mapRowWorkModeTabWidget")->setCurrentIndex(1); break;
        case 4: capture("map-mission"); window.resize(1100,760); break;
        case 5: capture("map-small"); click("maintenanceButton"); break;
        case 6: capture("maintenance"); click("helpButton"); break;
        case 7: capture("help"); click("aboutButton"); break;
        case 8: capture("about"); timer->stop(); app.quit(); break;
        }
    });
    timer->start(500);
}

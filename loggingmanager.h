#ifndef LOGGINGMANAGER_H
#define LOGGINGMANAGER_H

#include <QtGlobal>

class LoggingManager
{
public:
    static void initialize();
    static void shutdown();

private:
    static void rotateLogs(const char *basePath, int maxFiles, qint64 maxBytes);
};

#endif // LOGGINGMANAGER_H

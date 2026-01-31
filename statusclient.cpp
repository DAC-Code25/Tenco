#include "statusclient.h"

#include "homenetworkworker.h"

#include <QMetaObject>
#include <QThread>

StatusClient::StatusClient(QObject *parent)
    : QObject(parent)
{
}

StatusClient::~StatusClient()
{
    stop();
    teardownWorker();
}

void StatusClient::configure(const QUrl &url, const QJsonArray &requests, int intervalMs)
{
    m_url = url;
    m_requests = requests;
    m_intervalMs = intervalMs;

    // HomeNetworkWorker::configure is not a slot; rebuild to apply config safely.
    rebuildWorker(m_running);
}

void StatusClient::start()
{
    if (m_running) {
        return;
    }
    m_running = true;

    if (!m_worker) {
        rebuildWorker(false);
    }

    if (!m_worker) {
        m_running = false;
        return;
    }

    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void StatusClient::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
    }
}

void StatusClient::rebuildWorker(bool restartAfterwards)
{
    stop();
    teardownWorker();

    if (!m_url.isValid() || m_requests.isEmpty()) {
        return;
    }

    m_worker = new HomeNetworkWorker();
    m_worker->configure(m_url, m_requests, m_intervalMs);

    m_thread = new QThread(this);
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &HomeNetworkWorker::statusReceived, this, &StatusClient::statusReceived);
    connect(m_worker, &HomeNetworkWorker::requestFailed, this, &StatusClient::requestFailed);

    m_thread->start();

    if (restartAfterwards) {
        m_running = true;
        QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
    }
}

void StatusClient::teardownWorker()
{
    if (m_worker) {
        // Ensure the worker is deleted in its own thread before the event loop stops.
        QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_worker, "deleteLater", Qt::QueuedConnection);
        m_worker = nullptr;
    }

    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread = nullptr;
    }
}


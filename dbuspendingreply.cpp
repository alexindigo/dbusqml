#include "dbuspendingreply.h"
#include "dbusconnection.h"

#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

DBusPendingReply::DBusPendingReply(QObject *parent) : QObject(parent) {}

DBusPendingReply::~DBusPendingReply() {}

void DBusPendingReply::setWatcher(QDBusPendingCallWatcher *watcher) {
    m_watcher = watcher;
    // SingleShotConnection: auto-disconnects after firing once, so the
    // callback doesn't hold a reference that would prevent GC. The watcher
    // is deleted immediately after the callback completes — safe because
    // this is the only slot (SingleShotConnection).
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &DBusPendingReply::onFinished,
            Qt::SingleShotConnection);
}

bool DBusPendingReply::isError() const {
    if (m_cached)
        return m_isError;
    return m_watcher ? m_watcher->isError() : true;
}

bool DBusPendingReply::isValid() const {
    if (m_cached)
        return m_isValid;
    return m_watcher && m_watcher->isValid() && !m_watcher->isError();
}

DBusError DBusPendingReply::error() const {
    if (m_cached)
        return m_error;
    if (m_watcher)
        return DBusError(m_watcher->error());
    return DBusError(QDBusError(QDBusError::InternalError, "No pending call"));
}

QVariant DBusPendingReply::value() const {
    if (m_cached)
        return m_value;
    if (!m_watcher || m_watcher->isError())
        return {};

    QDBusMessage reply = m_watcher->reply();
    if (reply.arguments().isEmpty())
        return {};

    QVariant val = reply.arguments().first();
    return unwrapDbus(val);
}

QVariantList DBusPendingReply::values() const {
    if (m_cached)
        return m_values;
    if (!m_watcher || m_watcher->isError())
        return {};

    QVariantList args = m_watcher->reply().arguments();
    for (int i = 0; i < args.size(); ++i)
        args[i] = unwrapDbus(args[i]);
    return args;
}

void DBusPendingReply::onFinished(QDBusPendingCallWatcher *watcher) {
    if (m_watcher && !m_cached) {
        m_isError = watcher->isError();
        m_isValid = watcher->isValid() && !watcher->isError();

        if (m_isError) {
            m_error = DBusError(watcher->error());
        } else {
            QDBusMessage reply = watcher->reply();
            QVariantList args = reply.arguments();
            for (int i = 0; i < args.size(); ++i)
                args[i] = unwrapDbus(args[i]);
            m_values = args;
            m_value = args.isEmpty() ? QVariant() : args.first();
        }

        m_cached = true;
    }

    m_finished = true;
    emit finished();

    // Clean up the watcher after the signal is delivered. SingleShotConnection
    // means this is the only slot, so the signal emission completes before the
    // destructor runs. Immediate delete (not deleteLater) is safe here.
    delete watcher;
    m_watcher = nullptr;
}

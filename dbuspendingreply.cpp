#include "dbuspendingreply.h"
#include "dbusconnection.h"

#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

DBusPendingReply::DBusPendingReply(QObject *parent)
    : QObject(parent)
{
}

DBusPendingReply::~DBusPendingReply()
{
}

void DBusPendingReply::setWatcher(QDBusPendingCallWatcher *watcher)
{
    m_watcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &DBusPendingReply::onFinished);
}

bool DBusPendingReply::isError() const
{
    if (m_cached)
        return m_isError;
    return m_watcher ? m_watcher->isError() : true;
}

bool DBusPendingReply::isValid() const
{
    if (m_cached)
        return m_isValid;
    return m_watcher && m_watcher->isValid() && !m_watcher->isError();
}

DBusError DBusPendingReply::error() const
{
    if (m_cached)
        return m_error;
    if (m_watcher)
        return DBusError(m_watcher->error());
    return DBusError(QDBusError(QDBusError::InternalError, "No pending call"));
}

QVariant DBusPendingReply::value() const
{
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

QVariantList DBusPendingReply::values() const
{
    if (m_cached)
        return m_values;
    if (!m_watcher || m_watcher->isError())
        return {};

    QVariantList args = m_watcher->reply().arguments();
    for (int i = 0; i < args.size(); ++i)
        args[i] = unwrapDbus(args[i]);
    return args;
}

void DBusPendingReply::onFinished(QDBusPendingCallWatcher *watcher)
{
    Q_UNUSED(watcher);

    if (m_watcher && !m_cached) {
        m_isError = m_watcher->isError();
        m_isValid = m_watcher->isValid() && !m_watcher->isError();

        if (m_isError) {
            m_error = DBusError(m_watcher->error());
        } else {
            QDBusMessage reply = m_watcher->reply();
            QVariantList args = reply.arguments();
            for (int i = 0; i < args.size(); ++i)
                args[i] = unwrapDbus(args[i]);
            m_values = args;
            m_value = args.isEmpty() ? QVariant() : args.first();
        }

        m_cached = true;
        m_watcher = nullptr;
    }

    m_finished = true;
    // Queue the emission: a synchronous emit here can outrun a caller's
    // `reply.finished.connect(...)` on fast local-bus round trips (the reply
    // arrives before the QML connect line runs). Posting it guarantees the
    // synchronous connect-after-call pattern always lands first.
    QMetaObject::invokeMethod(this, "finished", Qt::QueuedConnection);
}

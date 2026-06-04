#include "dbuspendingreply.h"

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
    return m_watcher ? m_watcher->isError() : true;
}

bool DBusPendingReply::isValid() const
{
    return m_watcher && m_watcher->isValid() && !m_watcher->isError();
}

DBusError DBusPendingReply::error() const
{
    if (m_watcher)
        return DBusError(m_watcher->error());
    return DBusError(QDBusError(QDBusError::InternalError, "No pending call"));
}

QVariant DBusPendingReply::value() const
{
    if (!m_watcher || m_watcher->isError())
        return {};

    QDBusMessage reply = m_watcher->reply();
    if (reply.arguments().isEmpty())
        return {};

    QVariant val = reply.arguments().first();
    if (val.userType() == qMetaTypeId<QDBusVariant>())
        val = val.value<QDBusVariant>().variant();
    return val;
}

QVariantList DBusPendingReply::values() const
{
    if (!m_watcher || m_watcher->isError())
        return {};

    QVariantList args = m_watcher->reply().arguments();
    for (int i = 0; i < args.size(); ++i) {
        if (args[i].userType() == qMetaTypeId<QDBusVariant>())
            args[i] = args[i].value<QDBusVariant>().variant();
    }
    return args;
}

void DBusPendingReply::onFinished(QDBusPendingCallWatcher *watcher)
{
    m_finished = true;
    emit finished();
}

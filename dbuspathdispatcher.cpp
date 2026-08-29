#include "dbuspathdispatcher.h"

#include "dbusadaptor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <qqmlinfo.h>

namespace {

using PathKey = QPair<QString, QString>;    // (connection name, path)
using ServiceKey = QPair<QString, QString>; // (connection name, service)

QMutex &registryMutex() {
    static QMutex m;
    return m;
}

QHash<PathKey, DBusPathDispatcher *> &dispatchers() {
    static QHash<PathKey, DBusPathDispatcher *> h;
    return h;
}

QHash<ServiceKey, int> &serviceRefs() {
    static QHash<ServiceKey, int> h;
    return h;
}

} // namespace

DBusPathDispatcher::DBusPathDispatcher(const QString &connName, const QString &path,
                                       const QDBusConnection &conn)
    : m_connName(connName), m_path(path), m_conn(conn) {}

bool DBusPathDispatcher::attach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor) {
    const QString connName = conn.name();
    const PathKey key{connName, path};

    DBusPathDispatcher *disp = nullptr;
    {
        QMutexLocker locker(&registryMutex());
        auto it = dispatchers().find(key);
        if (it != dispatchers().end()) {
            disp = it.value();
        } else {
            disp = new DBusPathDispatcher(connName, path, conn);
            if (!conn.registerVirtualObject(path, disp)) {
                qmlInfo(adaptor) << "Failed to register object at" << path;
                delete disp;
                return false;
            }
            dispatchers().insert(key, disp);
        }
    }

    disp->attachAdaptor(adaptor);

    if (!service.isEmpty()) {
        const ServiceKey svcKey{connName, service};
        QMutexLocker locker(&registryMutex());
        auto it = serviceRefs().find(svcKey);
        const int refs = (it == serviceRefs().end()) ? 0 : it.value();
        if (refs == 0) {
            if (!conn.registerService(service))
                qmlInfo(adaptor) << "Failed to register service" << service;
        }
        serviceRefs().insert(svcKey, refs + 1);
    }

    return true;
}

void DBusPathDispatcher::detach(QDBusConnection conn, const QString &path, const QString &service,
                                DBusAdaptor *adaptor) {
    const QString connName = conn.name();

    DBusPathDispatcher *disp = nullptr;
    {
        QMutexLocker locker(&registryMutex());
        auto it = dispatchers().find({connName, path});
        if (it != dispatchers().end())
            disp = it.value();
    }
    if (disp)
        disp->detachAdaptor(adaptor);

    if (!service.isEmpty()) {
        const ServiceKey svcKey{connName, service};
        QMutexLocker locker(&registryMutex());
        auto it = serviceRefs().find(svcKey);
        if (it != serviceRefs().end()) {
            const int refs = it.value() - 1;
            if (refs <= 0) {
                serviceRefs().erase(it);
                if (!conn.unregisterService(service))
                    qmlInfo(adaptor) << "Failed to unregister service" << service;
            } else {
                it.value() = refs;
            }
        }
    }
}

int DBusPathDispatcher::liveCount() {
    QMutexLocker locker(&registryMutex());
    return dispatchers().size();
}

void DBusPathDispatcher::attachAdaptor(DBusAdaptor *adaptor) {
    // Duplicate iface at one path: warn, keep attaching — first-attached wins
    // for iface-scoped calls (attach order is the routing order).
    const QString iface = adaptor->iface();
    if (!iface.isEmpty()) {
        for (const auto &a : m_adaptors) {
            if (a && a->iface() == iface) {
                qWarning("dbusqml: duplicate iface %s at path %s — first-attached adaptor wins",
                         qPrintable(iface), qPrintable(m_path));
                break;
            }
        }
    }
    m_adaptors.append(QPointer<DBusAdaptor>(adaptor));
}

void DBusPathDispatcher::detachAdaptor(DBusAdaptor *adaptor) {
    for (int i = 0; i < m_adaptors.size(); ++i) {
        if (m_adaptors.at(i).data() == adaptor) {
            m_adaptors.removeAt(i);
            break;
        }
    }

    if (!m_adaptors.isEmpty())
        return;

    // Last adaptor gone — drop the path and tear down the dispatcher.
    {
        QMutexLocker locker(&registryMutex());
        dispatchers().remove({m_connName, m_path});
    }
    m_conn.unregisterObject(m_path);
    deleteLater();
}

QString DBusPathDispatcher::introspect(const QString &) const {
    QString xml;
    for (const auto &a : m_adaptors) {
        if (a)
            xml += a->introspect(QString());
    }
    return xml;
}

bool DBusPathDispatcher::handleMessage(const QDBusMessage &msg, const QDBusConnection &conn) {
    const QString interface = msg.interface();

    // QtDBus serves introspect() for the Introspectable interface.
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        // Route by the interface ARGUMENT to the matching adaptor.
        QString reqIface;
        if (!msg.arguments().isEmpty())
            reqIface = msg.arguments().first().toString();
        if (!reqIface.isEmpty()) {
            for (const auto &a : m_adaptors) {
                if (a && a->iface() == reqIface)
                    return a->handleMessage(msg, conn);
            }
            // No matching adaptor — route to the first so it reports
            // "No such interface" (preserves the single-adaptor error).
            if (!m_adaptors.isEmpty() && m_adaptors.first())
                return m_adaptors.first()->handleMessage(msg, conn);
            return false;
        }
        // Empty interface arg — try all in attach order, first that handles.
        for (const auto &a : m_adaptors) {
            if (a && a->handleMessage(msg, conn))
                return true;
        }
        return false;
    }

    if (!interface.isEmpty()) {
        for (const auto &a : m_adaptors) {
            if (a && a->iface() == interface)
                return a->handleMessage(msg, conn);
        }
        return false;
    }

    // Empty interface — member-name dispatch across adaptors in attach order.
    for (const auto &a : m_adaptors) {
        if (a && a->handleMessage(msg, conn))
            return true;
    }
    return false;
}

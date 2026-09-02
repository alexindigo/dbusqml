#pragma once

#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QVariantMap>
#include <qqmlregistration.h>

#include "dbusconnection.h"

// ObjectManager client (org.freedesktop.DBus.ObjectManager): fetches and
// exposes GetManagedObjects for a service+path, and re-emits
// InterfacesAdded/InterfacesRemoved as QML signals with unwrapped args.
// The BlueZ / KDE-Connect / Valent consumer class.
class DBusObjectManager : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBusObjectManager)

    Q_PROPERTY(
        DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QVariantMap managedObjects READ managedObjects NOTIFY managedObjectsChanged)

public:
    explicit DBusObjectManager(QObject *parent = nullptr);
    ~DBusObjectManager() override;

    DBusConnection *connection() const { return m_conn.data(); }
    void setConnection(DBusConnection *v);

    QString service() const { return m_service; }
    void setService(const QString &v);

    QString path() const { return m_path; }
    void setPath(const QString &v);

    bool ready() const { return m_ready; }

    QVariantMap managedObjects() const { return m_objects; }

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

Q_SIGNALS:
    void connectionChanged();
    void serviceChanged();
    void pathChanged();
    void readyChanged();
    void managedObjectsChanged();
    // Unwrapped (objectPath, {iface → {prop → value}}).
    void interfacesAdded(const QString &objectPath, const QVariantMap &ifacesAndProps);
    // Unwrapped (objectPath, [iface, ...]).
    void interfacesRemoved(const QString &objectPath, const QStringList &ifaces);

private:
    void setup();
    void fetchManagedObjects();

private slots:
    void onInterfacesAdded(const QDBusMessage &msg);
    void onInterfacesRemoved(const QDBusMessage &msg);

private:
    QPointer<DBusConnection> m_conn;
    QString m_service;
    QString m_path;
    bool m_ready = false;
    bool m_complete = false;
    bool m_subscribed = false;
    QVariantMap m_objects;
};

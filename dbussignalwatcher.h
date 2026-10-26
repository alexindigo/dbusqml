#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <qqmlregistration.h>

#include "dbusconnection.h"

// Standalone signal watcher: subscribes to (service, path, iface, member) on
// any bus with NO proxy and NO introspection requirement. Empty fields are
// wildcards (empty member = every member of the interface; empty service =
// any sender) — the dbus-monitor-style use case, including NameOwnerChanged
// on the daemon and system-bus signals.
class DBusSignalWatcher : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBusSignalWatcher)

    Q_PROPERTY(
        DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged)
    Q_PROPERTY(QString member READ member WRITE setMember NOTIFY memberChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit DBusSignalWatcher(QObject *parent = nullptr);
    ~DBusSignalWatcher() override;

    DBusConnection *connection() const { return m_conn.data(); }
    void setConnection(DBusConnection *v);

    QString service() const { return m_service; }
    void setService(const QString &v);

    QString path() const { return m_path; }
    void setPath(const QString &v);

    QString iface() const { return m_iface; }
    void setIface(const QString &v);

    QString member() const { return m_member; }
    void setMember(const QString &v);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool v);

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

Q_SIGNALS:
    void connectionChanged();
    void serviceChanged();
    void pathChanged();
    void ifaceChanged();
    void memberChanged();
    void enabledChanged();
    // Every matching message: the wire member name and its unwrapped args.
    void received(const QString &member, const QVariantList &args);

private:
    void resubscribe();
    void unsubscribe();

private slots:
    void onSignal(const QDBusMessage &msg);

private:
    QPointer<DBusConnection> m_conn;
    // Connection configuration owns the transport, independently of the QML
    // wrapper and of the live subscription's filter/enable state.
    QDBusConnection m_bus = QDBusConnection::sessionBus();
    QString m_service;
    QString m_path;
    QString m_iface;
    QString m_member;
    bool m_enabled = true;
    bool m_connected = false;
    bool m_complete = false;
    // Recorded identity of the live subscription (Class D): the exact
    // connection + tuple passed to connect(). Disconnect ALWAYS uses this —
    // QtDBus requires exact-arg match — never the re-resolved current
    // fields (which may name a different tuple or, through a dead/null
    // m_conn, an entirely different bus). The session-bus initializer is
    // only a placeholder: it is overwritten at every successful connect
    // and never used while m_connected is false.
    QDBusConnection m_subConn = QDBusConnection::sessionBus();
    QString m_subService;
    QString m_subPath;
    QString m_subIface;
    QString m_subMember;
};

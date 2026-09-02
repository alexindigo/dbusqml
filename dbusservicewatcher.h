#pragma once

#include <QObject>
#include <QQmlParserStatus>
#include <QString>
#include <qqmlregistration.h>

#include "dbusconnection.h"

#include <QDBusServiceWatcher>

// Standalone service watcher: appears/disappears/owner changes for one
// well-known name on any bus — the standalone complement to the proxy's
// watchServiceStatus.
class DBusServiceWatcher : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBusServiceWatcher)

    Q_PROPERTY(
        DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(bool registered READ registered NOTIFY registeredChanged)

public:
    explicit DBusServiceWatcher(QObject *parent = nullptr);
    ~DBusServiceWatcher() override;

    DBusConnection *connection() const { return m_conn.data(); }
    void setConnection(DBusConnection *v);

    QString service() const { return m_service; }
    void setService(const QString &v);

    bool registered() const { return m_registered; }

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

Q_SIGNALS:
    void connectionChanged();
    void serviceChanged();
    void registeredChanged();
    void ownerChanged(const QString &oldOwner, const QString &newOwner);

private:
    void setup();
    void setRegistered(bool v);

    QPointer<DBusConnection> m_conn;
    QString m_service;
    bool m_registered = false;
    bool m_complete = false;
    QDBusServiceWatcher *m_watcher = nullptr;
};

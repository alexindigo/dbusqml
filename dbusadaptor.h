#pragma once

#include <QDBusVirtualObject>
#include <QJSValue>
#include <QObject>
#include <QQmlParserStatus>
#include <qqmlregistration.h>

#include "dbusconnection.h"

class DBusAdaptor : public QDBusVirtualObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBusAdaptor)

    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged)
    Q_PROPERTY(DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)

public:
    explicit DBusAdaptor(QObject *parent = nullptr);
    ~DBusAdaptor() override;

    QString service() const { return m_service; }
    void setService(const QString &v);

    QString path() const { return m_path; }
    void setPath(const QString &v);

    QString iface() const { return m_iface; }
    void setIface(const QString &v);

    DBusConnection *connection() const { return m_conn; }
    void setConnection(DBusConnection *v);

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

    // QDBusVirtualObject
    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

    Q_INVOKABLE void emitSignal(const QString &name, const QJSValue &arguments = QJSValue::UndefinedValue);

Q_SIGNALS:
    void serviceChanged();
    void pathChanged();
    void ifaceChanged();
    void connectionChanged();

private:
    QString generateXml() const;
    QDBusConnection bus() const;

    QString m_service;
    QString m_path;
    QString m_iface;
    DBusConnection *m_conn = nullptr;
};

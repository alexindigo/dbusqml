#pragma once

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <QQmlParserStatus>
#include <QQmlPropertyMap>
#include <QVariantList>
#include <qqmlregistration.h>

#include "dbusconnection.h"

class DBusProxy : public QQmlPropertyMap, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBus)

    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged)
    Q_PROPERTY(DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool signalsEnabled READ signalsEnabled WRITE setSignalsEnabled NOTIFY signalsEnabledChanged)
    Q_PROPERTY(bool watchServiceStatus READ watchServiceStatus WRITE setWatchServiceStatus NOTIFY watchServiceStatusChanged)
    Q_PROPERTY(bool serviceAvailable READ serviceAvailable NOTIFY serviceAvailableChanged)
    Q_PROPERTY(bool propertiesEnabled READ propertiesEnabled WRITE setPropertiesEnabled NOTIFY propertiesEnabledChanged)

public:
    enum Status { Null, Loading, Ready, Error };
    Q_ENUM(Status)

    explicit DBusProxy(QObject *parent = nullptr);
    ~DBusProxy() override;

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

    QString service() const { return m_service; }
    void setService(const QString &v);

    QString path() const { return m_path; }
    void setPath(const QString &v);

    QString iface() const { return m_iface; }
    void setIface(const QString &v);

    DBusConnection *connection() const { return m_conn; }
    void setConnection(DBusConnection *v);

    Status status() const { return m_status; }

    Q_INVOKABLE DBusPendingReply *call(const QString &method, const QVariantList &args = {});
    Q_INVOKABLE DBusPendingReply *getProperty(const QString &name);
    Q_INVOKABLE void setProperty(const QString &name, const QVariant &value);
    Q_INVOKABLE void emitSignal(const QString &name, const QVariantList &args = {});
    Q_INVOKABLE static DBusConnection *connectToBus(const QString &address);
    Q_INVOKABLE static void reloadTypes();
    Q_INVOKABLE static void emitSignal(const QString &service, const QString &path,
                                        const QString &iface, const QString &name,
                                        const QVariantList &args = {});

    bool signalsEnabled() const { return m_signalsEnabled; }
    void setSignalsEnabled(bool v);

    bool watchServiceStatus() const { return m_watchServiceStatus; }
    void setWatchServiceStatus(bool v);

    bool serviceAvailable() const { return m_serviceAvailable; }

    bool propertiesEnabled() const { return m_propertiesEnabled; }
    void setPropertiesEnabled(bool v);

Q_SIGNALS:
    void serviceChanged();
    void pathChanged();
    void ifaceChanged();
    void connectionChanged();
    void signalReceived(const QString &name, const QVariantList &args);
    void statusChanged();
    void introspectionCompleted();
    void signalsEnabledChanged();
    void watchServiceStatusChanged();
    void serviceAvailableChanged();
    void propertiesEnabledChanged();

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

private Q_SLOTS:
    void onPropertiesChanged(const QDBusMessage &msg);

private:
    void fetchProperties();
    void doIntrospect();
    void onIntrospectionReady(const QString &xml);
    void setupDynamicMethods(const QStringList &methodNames);

    QString m_service;
    QString m_path;
    QString m_iface;
    DBusConnection *m_conn = nullptr;
    QDBusConnection m_bus;
    bool m_signalsConnected = false;
    bool m_signalsEnabled = true;
    bool m_watchServiceStatus = false;
    bool m_serviceAvailable = false;
    bool m_propertiesEnabled = true;
    bool m_componentComplete = false;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;
    Status m_status = Null;
    QDBusPendingCallWatcher *m_introspectWatcher = nullptr;
    QList<QJSValue> m_cachedFunctions;
    QStringList m_dynamicMethodKeys;   // qml-cased method names currently installed on the property map
    QHash<QString, QString> m_introspectCache;
    QHash<QString, QStringList> m_methodArgTypes;
};

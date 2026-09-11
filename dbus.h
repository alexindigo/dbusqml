#pragma once

#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QQmlParserStatus>
#include <QQmlPropertyMap>
#include <QSet>
#include <QVariantList>
#include <qqmlregistration.h>

#include "dbusconnection.h"
#include "dbuspendingreply.h"

class DBusProxy : public QQmlPropertyMap, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBus)

    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged)
    Q_PROPERTY(
        DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool signalsEnabled READ signalsEnabled WRITE setSignalsEnabled NOTIFY
                   signalsEnabledChanged)
    Q_PROPERTY(bool watchServiceStatus READ watchServiceStatus WRITE setWatchServiceStatus NOTIFY
                   watchServiceStatusChanged)
    Q_PROPERTY(bool serviceAvailable READ serviceAvailable NOTIFY serviceAvailableChanged)
    Q_PROPERTY(bool propertiesEnabled READ propertiesEnabled WRITE setPropertiesEnabled NOTIFY
                   propertiesEnabledChanged)
    Q_PROPERTY(bool reactiveBindingsSupported READ hasReactiveBindings CONSTANT)
    Q_PROPERTY(QVariantMap _signatures READ signatures WRITE setSignatures NOTIFY signaturesChanged)
    // Governs ALL proxy traffic: call(), dynamic methods, property reads and
    // writes, and the internal Introspect/GetAll startup calls (ms; −1 =
    // Qt default). Per-call message.timeout overrides on raw asyncCall.
    Q_PROPERTY(int callTimeout READ callTimeout WRITE setCallTimeout NOTIFY callTimeoutChanged)

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

    DBusConnection *connection() const { return m_conn.data(); }
    void setConnection(DBusConnection *v);

    Status status() const { return m_status; }

    QVariantMap signatures() const { return m_signatures; }
    void setSignatures(const QVariantMap &v);

    // Effective call-argument signatures for a method, in precedence order:
    // explicit _signatures override → introspection/catalog discovery. Empty
    // when neither declares the method (caller falls back to inference).
    QStringList argTypesForMethod(const QString &method) const;

    Q_INVOKABLE DBusPendingReply *call(const QString &method, const QVariantList &args = {});
    Q_INVOKABLE DBusPendingReply *getProperty(const QString &name);
    Q_INVOKABLE void setProperty(const QString &name, const QVariant &value);
    // Fire-and-forget method call: NO_REPLY_EXPECTED implied, nothing comes
    // back.
    Q_INVOKABLE void send(const QString &method, const QVariantList &args = {});

    int callTimeout() const { return m_callTimeout; }
    void setCallTimeout(int v) {
        if (m_callTimeout == v)
            return;
        m_callTimeout = v;
        emit callTimeoutChanged();
    }

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

    static bool reactiveBindingsSupported();
    bool hasReactiveBindings() const;

protected:
    QVariant updateValue(const QString &key, const QVariant &input) override;

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
    void signaturesChanged();
    void callTimeoutChanged();
    // P8 (features train, Phase 7): a property write the service
    // rejected (failed Set). R3 (road-to-one, Call 3): the proxy
    // re-fetches the property from the service instead of rolling back —
    // the map converges to the server's value, never a local inference.
    void propertyWriteFailed(const QString &name, const QString &errorName, const QString &message);

private Q_SLOTS:
    void onPropertiesChanged(const QDBusMessage &msg);
    // P5: the bus this proxy talks on died. Flip to Error, drop match
    // subscriptions (dead on a dead bus), mark the service unavailable.
    void onBusDisconnected();

private:
    void fetchProperties();
    void doIntrospect();
    void scheduleIntrospect();
    void onIntrospectionReady(const QString &xml);
    void setupDynamicMethods(const QStringList &methodNames);
    void disconnectSignals();
    void prepopulateFromCatalog();

    QString m_service;
    QString m_path;
    QString m_iface;
    QPointer<DBusConnection> m_conn;

    QDBusConnection m_bus;
    bool m_signalsConnected = false;
    bool m_signalsEnabled = true;
    int m_callTimeout = -1;
    // A3/D3: re-Get invalidated property names after a PropertiesChanged
    // carrying invalidated_properties (stale kept + one warning on error).
    // CF-10: each in-flight re-fetch carries a generation epoch — a stale
    // reply arriving after a repoint or a newer value is dropped.
    void refetchInvalidated(const QStringList &names);
    quint64 m_refetchEpoch = 0;

    // Idempotent service-watcher readiness: create-or-rewire on the CURRENT
    // bus and (re-)run the initial NameHasOwner check. Safe from any entry
    // point in any order — componentComplete() backfills whatever the
    // engine's binding application order skipped.
    void ensureServiceWatcher();
    // P5: session-bus disconnect watch for proxies on the default
    // connection (no DBusConnection object to relay it). Idempotent.
    void ensureSessionDisconnectWatch();

    bool m_watchServiceStatus = false;
    bool m_serviceAvailable = false;
    bool m_sessionDisconnectWatched = false;
    // CF-26: per-service claim cache for emitSignal — the blocking
    // registerService round-trip runs once per service value, not once
    // per emission. Reset implicitly: a new service value != cached one
    // re-arms the single attempt.
    QString m_claimAttemptedService;
    bool m_propertiesEnabled = true;
    bool m_componentComplete = false;
    bool m_introspectQueued = false;
    QDBusServiceWatcher *m_serviceWatcher = nullptr;
    Status m_status = Null;
    QDBusPendingCallWatcher *m_introspectWatcher = nullptr;
    QList<QJSValue> m_cachedFunctions;
    QStringList
        m_dynamicMethodKeys; // qml-cased method names currently installed on the property map
    QHash<QString, QString> m_introspectCache;
    QHash<QString, QStringList> m_methodArgTypes;
    QVariantMap m_signatures;
    // Maps QML camelCase property names → original D-Bus PascalCase names.
    // Populated whenever a property is learned (fetchProperties,
    // onPropertiesChanged, catalog/live pre-populate).
    QHash<QString, QString> m_qmlToDbusName;

    // Recorded signal connection parameters for exact disconnect.
    // QtDBus disconnect requires exact-arg match with connect args.
    QStringList m_connectedSignals;
    QString m_connectedService;
    QString m_connectedPath;
    QString m_connectedIface;

    // P9 (features train, Phase 8): pending-op dedupe maps, keyed by
    // property (KDE dbusproperties.cpp:32 guard semantics). Duplicate
    // in-flight Get coalesced (one wire call, all waiters answered);
    // Set dedupe = latest-wins queue (interleaved set/set converges to
    // the last value). No new surface.
    struct PendingGet {
        QDBusPendingCallWatcher *watcher = nullptr;
        DBusPendingReply *owner = nullptr;
        QList<DBusPendingReply *> waiters;
    };
    QHash<QString, PendingGet> m_pendingGets;
    struct PendingSet {
        QDBusPendingCallWatcher *watcher = nullptr;
        QVariant latestValue;
        bool queued = false; // a newer value arrived while in flight
        // The QML-side key for the propertyWriteFailed signal (the
        // fresh-call lambdas carry it as a capture; the chained path has
        // no lambda, so it carries it here).
        // CF-15: destination identity snapshot — a repoint mid-queue
        // must not fire the queued write at the NEW service.
        QString qmlKey;
        QString service;
        QString path;
        QString iface;
    };
    QHash<QString, PendingSet> m_pendingSets;
    // R3 (road-to-one, Call 3): wire names whose Set failed, awaiting
    // chain settle for the one post-settle re-fetch (server truth replaces
    // the optimistic value; the proxy never invents one).
    QSet<QString> m_failedSetRefetch;
    void finishPendingGet(const QString &dbusName, const QDBusMessage &reply);
    void finishPendingSet(const QString &dbusName, const QDBusMessage &reply);
    void maybeRefetchAfterFailedWrite(const QString &wireName);
};

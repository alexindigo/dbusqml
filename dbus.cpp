#include "dbus.h"
#include "dbuscatalog.h"
#include "dbuspendingreply.h"
#include "dbusintrospection.h"
#include "dbuspendingreply.h"
#include "dbustypes.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusVariant>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QXmlStreamReader>

// Map a D-Bus type signature to the corresponding C++ QVariant type.
// Delegates to the signature-driven marshaller in dbusconnection.cpp.
// The marshaller handles all types — basic coercion, ay (string→UTF-8),
// as (via DBusAsArray), a{sv}, a{sa{sv}}, and nested containers.
static QVariant toTypedDbusVariant(const QVariant &v, const QString &dbusType) {
    return marshalBySignature(dbusType, v);
}

// Helper object exposed to the JS engine so evaluated functions can make D-Bus calls
class DbusMethodHelper : public QObject {
    Q_OBJECT
public:
    DbusMethodHelper(DBusProxy *proxy, QObject *parent = nullptr)
        : QObject(parent), m_proxy(proxy) {}

    Q_INVOKABLE DBusPendingReply *callMethod(const QString &method, const QVariantList &args) {
        QDBusConnection bus = m_proxy->connection()
                                  ? static_cast<QDBusConnection>(*m_proxy->connection())
                                  : QDBusConnection::sessionBus();

        // Convert arguments to match expected D-Bus types (override → introspected → inference).
        QVariantList converted = args;
        const QStringList types = m_proxy->argTypesForMethod(method);
        for (int i = 0; i < converted.size() && i < types.size(); ++i)
            converted[i] = toTypedDbusVariant(converted[i], types[i]);

        for (int i = 0; i < converted.size(); ++i) {
            if (!wireMarshalable(converted.at(i))) {
                qWarning("dbusqml: argument %d of %s is not marshalable (type %s) — failing call "
                         "locally",
                         i, qPrintable(method), QMetaType(converted.at(i).userType()).name());
                auto *fail = new DBusPendingReply(m_proxy);
                fail->setEngine(qmlEngine(m_proxy));
                fail->completeLocalError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                         QStringLiteral("argument %1 is not marshalable").arg(i));
                return fail;
            }
        }

        QDBusMessage msg = QDBusMessage::createMethodCall(m_proxy->service(), m_proxy->path(),
                                                          m_proxy->iface(), method);
        if (!converted.isEmpty())
            msg.setArguments(converted);
        auto pending = bus.asyncCall(msg, m_proxy ? m_proxy->callTimeout() : -1);
        auto watcher = new QDBusPendingCallWatcher(pending, this);
        auto reply = new DBusPendingReply(this);
        reply->setEngine(qmlEngine(m_proxy));
        reply->setWatcher(watcher);
        return reply;
    }

private:
    DBusProxy *m_proxy;
};

// A14/D5: the CLIENT fold (upper-runs collapse) — a documented mode of the
// shared dbusFoldName; never a second implementation.
static QString dbusPropToQml(const QString &name) {
    return dbusFoldName(name, true);
}

DBusProxy::DBusProxy(QObject *parent)
    : QQmlPropertyMap(this, parent), m_bus(QDBusConnection::sessionBus()) {}

DBusProxy::~DBusProxy() {
    disconnectSignals();
}

void DBusProxy::componentComplete() {
    m_componentComplete = true;
    ensureServiceWatcher();
    // P5: the default session-bus connection has no DBusConnection object
    // to emit disconnected() — watch the session bus directly.
    if (!m_conn)
        ensureSessionDisconnectWatch();
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        doIntrospect();
}

void DBusProxy::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    ensureServiceWatcher();
    emit serviceChanged();
    prepopulateFromCatalog();
    if (!m_iface.isEmpty() && !m_path.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
    prepopulateFromCatalog();
    if (!m_iface.isEmpty() && !m_service.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::setIface(const QString &v) {
    if (m_iface == v)
        return;
    QString oldIface = m_iface;
    m_iface = v;
    emit ifaceChanged();
    prepopulateFromCatalog();

    disconnectSignals();

    if (!m_service.isEmpty() && !m_path.isEmpty())
        scheduleIntrospect();
}

void DBusProxy::scheduleIntrospect() {
    if (!m_componentComplete) {
        // C++-created proxy — no QML lifecycle, introspect directly.
        doIntrospect();
        return;
    }
    if (m_introspectQueued)
        return;
    m_introspectQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_introspectQueued = false;
        doIntrospect();
    });
}

void DBusProxy::doIntrospect() {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    // Cancel any in-flight watcher — a newer introspection request
    // supersedes the old one.
    if (m_introspectWatcher) {
        m_introspectWatcher->disconnect(this);
        m_introspectWatcher->deleteLater();
        m_introspectWatcher = nullptr;
    }

    QString cacheKey = m_service + QLatin1Char('|') + m_path;
    auto it = m_introspectCache.find(cacheKey);
    if (it != m_introspectCache.end()) {
        onIntrospectionReady(it.value());
        return;
    }

    m_status = Loading;
    emit statusChanged();

    QDBusMessage call = QDBusMessage::createMethodCall(
        m_service, m_path, "org.freedesktop.DBus.Introspectable", "Introspect");
    auto pending = m_bus.asyncCall(call, m_callTimeout);
    m_introspectWatcher = new QDBusPendingCallWatcher(pending, this);

    connect(m_introspectWatcher, &QDBusPendingCallWatcher::finished, this,
            [this, cacheKey](QDBusPendingCallWatcher *w) {
                // Ignore stale watchers — a newer introspection was started
                if (w != m_introspectWatcher) {
                    w->deleteLater();
                    return;
                }
                m_introspectWatcher = nullptr;
                QDBusPendingReply<QString> reply = *w;
                if (!reply.isError()) {
                    m_introspectCache.insert(cacheKey, reply.value());
                    onIntrospectionReady(reply.value());
                } else if (m_propertiesEnabled) {
                    // Introspection failed — fall back to GetAll. Many
                    // services (NM Ip4Config, AccessPoint, Connection.Active)
                    // return empty or failing Introspect but answer
                    // Properties.GetAll fine. Empty XML means no methods or
                    // signals from introspection; the catalog can fill in.
                    onIntrospectionReady(QString());
                } else {
                    m_status = Error;
                    emit statusChanged();
                }
                w->deleteLater();
            });
}

void DBusProxy::setSignalsEnabled(bool v) {
    if (m_signalsEnabled == v)
        return;
    m_signalsEnabled = v;
    disconnectSignals();
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        scheduleIntrospect();
    emit signalsEnabledChanged();
}

void DBusProxy::setPropertiesEnabled(bool v) {
    if (m_propertiesEnabled == v)
        return;
    m_propertiesEnabled = v;
    if (v && !m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        fetchProperties();
    emit propertiesEnabledChanged();
}

void DBusProxy::setWatchServiceStatus(bool v) {
    if (m_watchServiceStatus == v)
        return;
    m_watchServiceStatus = v;

    if (v)
        ensureServiceWatcher();

    if (!v && m_serviceWatcher) {
        m_serviceWatcher->deleteLater();
        m_serviceWatcher = nullptr;
    }

    emit watchServiceStatusChanged();
}

void DBusProxy::ensureSessionDisconnectWatch() {
    // P5: proxies on the default session-bus connection have no
    // DBusConnection object to relay loss — poll the session bus
    // liveness through the shared loss-probe pattern (a daemon-facing
    // NameHasOwner ping fails with Disconnected when the bus dies).
    // Idempotent; no resubscribe (documented, FD5). CF-5: the alive
    // completion re-arms through the same single-shot gate as the
    // connection twin (3000 ms constant) — the old flag-reset re-armed
    // on next entry only (loss after the startup window undetected),
    // while a synchronous re-arm here would storm like the twin did.
    if (m_sessionDisconnectWatched || m_conn)
        return;
    m_sessionDisconnectWatched = true;
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    ping.setArguments({QDBusConnection::sessionBus().baseService()});
    QDBusPendingCall call = m_bus.asyncCall(ping);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<bool> reply = *w;
        w->deleteLater();
        if (reply.isError() && reply.error().type() == QDBusError::Disconnected) {
            onBusDisconnected();
            return;
        }
        // Alive: release the watch so a LATER entry re-arms (loss after
        // the startup window is detected), then gate the re-arm on a
        // single-shot — CF-5, same 3000 ms constant as the twin.
        if (m_conn)
            return;
        m_sessionDisconnectWatched = false;
        auto *gate = new QTimer(this);
        gate->setSingleShot(true);
        gate->callOnTimeout(this, [this, gate] {
            gate->deleteLater();
            ensureSessionDisconnectWatch();
        });
        gate->start(3000);
    });
}

void DBusProxy::onBusDisconnected() {
    // P5 loss handling: tear down match subscriptions (dead on a dead
    // bus — QtDBus disconnects are no-ops there), flip to Error, mark
    // the service unavailable. No automatic resubscribe (FD5).
    disconnectSignals();
    if (m_status != Error) {
        m_status = Error;
        emit statusChanged();
    }
    if (m_serviceAvailable) {
        m_serviceAvailable = false;
        emit serviceAvailableChanged();
    }
}

// Idempotent service-watcher readiness (Nemo-shaped): create-or-rewire on
// the CURRENT bus, then (re-)run the initial async NameHasOwner. Guarded by
// watchServiceStatus && service — every entry point in every order lands
// here, and componentComplete() backfills whatever binding order skipped.
void DBusProxy::ensureServiceWatcher() {
    if (!m_watchServiceStatus || m_service.isEmpty())
        return;

    if (!m_serviceWatcher) {
        m_serviceWatcher = new QDBusServiceWatcher(m_service, m_bus,
                                                   QDBusServiceWatcher::WatchForRegistration |
                                                       QDBusServiceWatcher::WatchForUnregistration,
                                                   this);
        connect(m_serviceWatcher, &QDBusServiceWatcher::serviceRegistered, this, [this]() {
            m_serviceAvailable = true;
            emit serviceAvailableChanged();
            // Late-start/restart recovery: the provider is back —
            // re-introspect and repopulate methods/properties (Nemo
            // re-connects everything on registration).
            scheduleIntrospect();
        });
        connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() {
            m_serviceAvailable = false;
            emit serviceAvailableChanged();
        });
    } else {
        m_serviceWatcher->setWatchedServices({m_service});
    }

    // (Re-)check initial state: NameHasOwner on the bus daemon. Gated on
    // the service still being ours — a queued reply from a previous
    // identity must not overwrite the current answer.
    const QString watched = m_service;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    msg.setArguments({watched});
    QDBusPendingReply<bool> nameReply = m_bus.asyncCall(msg, m_callTimeout);
    auto *watcher = new QDBusPendingCallWatcher(nameReply, this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watched](QDBusPendingCallWatcher *w) {
                QDBusPendingReply<bool> reply = *w;
                if (!reply.isError() && watched == m_service) {
                    m_serviceAvailable = reply.value();
                    emit serviceAvailableChanged();
                }
                w->deleteLater();
            });
}

void DBusProxy::setSignatures(const QVariantMap &v) {
    if (m_signatures == v)
        return;
    m_signatures = v;
    emit signaturesChanged();
}

QStringList DBusProxy::argTypesForMethod(const QString &method) const {
    // Explicit override wins over discovered signatures — the author corrects
    // wrong or missing introspection. Value is a concatenated in-arg signature,
    // split per-arg.
    auto it = m_signatures.constFind(method);
    if (it == m_signatures.constEnd())
        it = m_signatures.constFind(dbusPropToQml(method));
    if (it != m_signatures.constEnd()) {
        QStringList out;
        const QString sig = it.value().toString();
        int pos = 0;
        while (pos < sig.size()) {
            const QString argSig = firstCompleteType(sig, pos);
            if (argSig.isEmpty())
                break;
            out << argSig;
        }
        // A non-empty override that splits to nothing is a typo — say so
        // instead of silently falling back to inference.
        if (out.isEmpty() && !sig.isEmpty())
            qWarning("dbusqml: unparseable declared signature '%s' for %s — ignoring (inference "
                     "used)",
                     qPrintable(sig), qPrintable(method));
        return out;
    }
    return m_methodArgTypes.value(method);
}

void DBusProxy::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;

    disconnectSignals();
    m_introspectCache.clear();
    if (m_conn)
        disconnect(m_conn, &DBusConnection::disconnected, this, &DBusProxy::onBusDisconnected);

    m_conn = v;
    if (v) {
        m_bus = static_cast<QDBusConnection>(*v);
        connect(v, &DBusConnection::disconnected, this, &DBusProxy::onBusDisconnected);
    } else {
        m_bus = QDBusConnection::sessionBus();
        ensureSessionDisconnectWatch();
    }
    emit connectionChanged();

    // The watcher is bound to the old bus — destroy + recreate on the new
    // bus and re-run the initial check.
    if (m_serviceWatcher) {
        m_serviceWatcher->deleteLater();
        m_serviceWatcher = nullptr;
    }
    ensureServiceWatcher();

    // Re-introspect on the new bus to re-establish signal subscriptions
    if (!m_service.isEmpty() && !m_path.isEmpty() && !m_iface.isEmpty())
        scheduleIntrospect();
}

DBusConnection *DBusProxy::connectToBus(const QString &address) {
    // Delegate to DBusConnection so both entry points share one counter
    // and can never collide on connection names.
    return DBusConnection::connectToBus(address);
}

void DBusProxy::emitSignal(const QString &name, const QVariantList &args) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    // Try to claim the service name so the signal appears to come from the
    // expected service (e.g. org.freedesktop.portal.Desktop).
    // If the name is already owned (by the real portal), this silently fails.
    if (!m_service.startsWith(':'))
        m_bus.registerService(m_service);

    QDBusMessage msg = QDBusMessage::createSignal(m_path, m_iface, name);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        for (int i = 0; i < converted.size(); ++i) {
            if (!wireMarshalable(converted.at(i))) {
                // Signals have no error-reply channel — warn and skip, matching
                // the adaptor-side convention.
                qWarning("dbusqml: signal %s argument %d is not marshalable (type %s) — skipping "
                         "send",
                         qPrintable(name), i, QMetaType(converted.at(i).userType()).name());
                return;
            }
        }
        msg.setArguments(converted);
    }
    m_bus.send(msg);
}

void DBusProxy::emitSignal(const QString &service, const QString &path, const QString &iface,
                           const QString &name, const QVariantList &args) {
    QDBusMessage msg = QDBusMessage::createSignal(path, iface, name);
    if (!args.isEmpty()) {
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i)
            converted[i] = toDbusVariant(converted[i]);
        for (int i = 0; i < converted.size(); ++i) {
            if (!wireMarshalable(converted.at(i))) {
                qWarning("dbusqml: signal %s argument %d is not marshalable (type %s) — skipping "
                         "send",
                         qPrintable(name), i, QMetaType(converted.at(i).userType()).name());
                return;
            }
        }
        msg.setArguments(converted);
    }
    QDBusConnection::sessionBus().send(msg);
}

DBusPendingReply *DBusProxy::call(const QString &method, const QVariantList &args) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return nullptr;

    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, m_iface, method);
    if (!args.isEmpty()) {
        QStringList types = argTypesForMethod(method);
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i) {
            QString expectedType;
            if (i < types.size())
                expectedType = types[i];
            converted[i] = toTypedDbusVariant(converted[i], expectedType);
        }
        for (int i = 0; i < converted.size(); ++i) {
            if (!wireMarshalable(converted.at(i))) {
                qWarning("dbusqml: argument %d of %s is not marshalable (type %s) — failing call "
                         "locally",
                         i, qPrintable(method), QMetaType(converted.at(i).userType()).name());
                auto *fail = new DBusPendingReply(this);
                fail->setEngine(qmlEngine(this));
                fail->completeLocalError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                         QStringLiteral("argument %1 is not marshalable").arg(i));
                return fail;
            }
        }
        msg.setArguments(converted);
    }
    auto pending = m_bus.asyncCall(msg, m_callTimeout);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setEngine(qmlEngine(this));
    reply->setWatcher(watcher);
    return reply;
}

DBusPendingReply *DBusProxy::getProperty(const QString &name) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return nullptr;

    // P9: duplicate in-flight Get coalesced (KDE dbusproperties.cpp:32
    // guard semantics). The first caller owns the wire call; later
    // callers attach waiters answered from the same reply. Keyed by the
    // WIRE name (the map key the reply resolves under).
    const QString wireName = m_qmlToDbusName.value(name, name);
    auto pit = m_pendingGets.find(wireName);
    if (pit != m_pendingGets.end()) {
        auto *coalesced = new DBusPendingReply(this);
        coalesced->setEngine(qmlEngine(this));
        pit->waiters.append(coalesced);
        return coalesced;
    }

    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Get");
    msg.setArguments({m_iface, name});

    auto pending = m_bus.asyncCall(msg, m_callTimeout);
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setEngine(qmlEngine(this));
    // NOTE: no setWatcher — the proxy's own finished lambda below drives
    // BOTH the owner's caching (via completeFromReply) and the waiters'.
    // setWatcher would ALSO cache via onFinished (SingleShot) — double
    // completion is benign (completeFromReply early-outs when cached),
    // but the extra connection is pointless; the single path keeps one
    // completion site.
    PendingGet pg;
    pg.watcher = watcher;
    pg.owner = reply;
    m_pendingGets.insert(wireName, pg);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, wireName](QDBusPendingCallWatcher *w) {
                QDBusMessage r = w->reply();
                w->deleteLater();
                finishPendingGet(wireName, r);
            });
    return reply;
}

void DBusProxy::finishPendingGet(const QString &dbusName, const QDBusMessage &reply) {
    // P9: answer the owner AND every coalesced waiter from the single
    // wire reply (direct emit — see completeFromReply's delivery note),
    // then drop the record.
    auto it = m_pendingGets.find(dbusName);
    if (it == m_pendingGets.end())
        return;
    PendingGet pg = std::move(it.value());
    m_pendingGets.erase(it);
    if (pg.owner)
        pg.owner->completeFromReply(reply);
    for (DBusPendingReply *w : pg.waiters) {
        if (w)
            w->completeFromReply(reply);
    }
}

void DBusProxy::setProperty(const QString &name, const QVariant &value) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    // B12: convert FIRST (gadget values to wire shapes), then guard — parity
    // with updateValue (a DBus.* gadget dropped through one API and passed
    // through the other).
    const QVariant converted = toDbusVariant(value);
    if (!wireMarshalable(converted)) {
        qWarning("dbusqml: value for property %s is not marshalable (type %s) — dropping write",
                 qPrintable(name), QMetaType(value.userType()).name());
        return;
    }
    // The wire name resolves through the recorded map (A16's placeholder
    // recording feeds it); unknown keys stay verbatim.
    const QString wireName = m_qmlToDbusName.value(name, name);
    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({m_iface, wireName, QVariant::fromValue(QDBusVariant(converted))});
    // P9: the invokable write path shares the latest-wins queue with
    // updateValue (one queue per wire name — both entry points collapse
    // genuinely overlapping Sets).
    auto sit = m_pendingSets.find(wireName);
    if (sit != m_pendingSets.end()) {
        sit->latestValue = converted;
        sit->queued = true;
        return;
    }
    // P8: capture the prior QML-visible value; on error reply restore it
    // (KDE dbusproperties.cpp:154-158) + warn + propertyWriteFailed.
    // The optimistic value is NOT inserted here (unlike updateValue's
    // immediate insert — see below): the map already holds the caller's
    // value when driven through QML bindings; setProperty restores on
    // failure only.
    const QVariant prior = QQmlPropertyMap::value(name);
    QDBusPendingCall call = m_bus.asyncCall(msg, m_callTimeout);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    // P9: the fresh wire call registers its pending-Set record (the
    // latest-wins queue entry other overlapping writes collapse into).
    PendingSet ps;
    ps.watcher = watcher;
    ps.latestValue = converted;
    ps.qmlKey = name;
    ps.prior = prior;
    m_pendingSets.insert(wireName, ps);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, name, wireName, prior](QDBusPendingCallWatcher *w) {
                QDBusMessage r = w->reply();
                const bool failed = r.type() == QDBusMessage::ErrorMessage;
                const QString errName = failed ? r.errorName() : QString();
                const QString errMsg = failed ? r.errorMessage() : QString();
                w->deleteLater();
                finishPendingSet(wireName, r);
                if (failed) {
                    // Roll back the QML-visible value to the prior one.
                    if (prior.isValid())
                        insert(name, prior);
                    else
                        clear(name);
                    qWarning("dbusqml: Set of property %s failed (%s: %s) — value restored",
                             qPrintable(wireName), qPrintable(errName), qPrintable(errMsg));
                    emit propertyWriteFailed(name, errName, errMsg);
                }
            });
}

void DBusProxy::send(const QString &method, const QVariantList &args) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;

    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path, m_iface, method);
    if (!args.isEmpty()) {
        QStringList types = argTypesForMethod(method);
        QVariantList converted = args;
        for (int i = 0; i < converted.size(); ++i) {
            QString expectedType;
            if (i < types.size())
                expectedType = types[i];
            converted[i] = toTypedDbusVariant(converted[i], expectedType);
        }
        for (int i = 0; i < converted.size(); ++i) {
            if (!wireMarshalable(converted.at(i))) {
                qWarning("dbusqml: argument %d of %s is not marshalable (type %s) — dropping "
                         "send",
                         i, qPrintable(method), QMetaType(converted.at(i).userType()).name());
                return;
            }
        }
        msg.setArguments(converted);
    }
    m_bus.send(msg); // fire-and-forget: NO_REPLY_EXPECTED implied
}

QVariant DBusProxy::updateValue(const QString &key, const QVariant &input) {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return input;

    // Map the QML camelCase name back to the D-Bus PascalCase name.
    // Unknown keys fall back to the verbatim name (services with lowercase
    // property names exist).
    const QVariant converted = toDbusVariant(input);
    if (!wireMarshalable(converted)) {
        qWarning("dbusqml: value for property %s is not marshalable (type %s) — dropping write",
                 qPrintable(key), QMetaType(converted.userType()).name());
        return input;
    }
    const QString dbusName = m_qmlToDbusName.value(key, key);
    // P9: Set dedupe = latest-wins queue. A Set already in flight keeps
    // the wire; the newer value OVERWRITES the queued one and is sent
    // when the in-flight Set completes (interleaved set/set converges
    // to the last value, one extra wire call max per overlap).
    //
    // RE-ENTRANCY NOTE: the first Set's watcher completes on the bus
    // (the loop must turn for delivery — callers pump between writes);
    // a write arriving while NO record exists starts a fresh wire call.
    // SYNCHRONOUS-WRITE NOTE: back-to-back setProperty calls with NO
    // loop turn between them all complete synchronously at the daemon
    // (each Set answers before the next is issued) — no record is ever
    // alive at entry, so each takes the fresh-call path and the count
    // is N. The dedupe ONLY collapses genuinely overlapping (in-flight)
    // Sets; the test pumps between writes to create the overlap.
    auto sit = m_pendingSets.find(dbusName);
    if (sit != m_pendingSets.end()) {
        sit->latestValue = converted;
        sit->queued = true;
        return input;
    }
    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({m_iface, dbusName, QVariant::fromValue(QDBusVariant(converted))});
    // P8: the map insert below is optimistic (QQmlPropertyMap reactivity
    // needs the value synchronously). Capture the PRIOR value first; on
    // error reply roll back + warn + propertyWriteFailed (KDE shape).
    // NOTE: updateValue's return value IS the inserted value — the caller
    // (QQmlPropertyMap::insert) applies it after we return, so the
    // rollback on failure replaces it asynchronously. That is the KDE
    // semantic (restore on error), just one event-loop turn later.
    const QVariant prior = QQmlPropertyMap::value(key);
    QDBusPendingCall call = m_bus.asyncCall(msg, m_callTimeout);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    PendingSet ps;
    ps.watcher = watcher;
    ps.latestValue = converted;
    ps.qmlKey = key;
    ps.prior = prior;
    m_pendingSets.insert(dbusName, ps);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, key, dbusName, prior](QDBusPendingCallWatcher *w) {
                QDBusMessage r = w->reply();
                const QString errName =
                    r.type() == QDBusMessage::ErrorMessage ? r.errorName() : QString();
                const QString errMsg =
                    r.type() == QDBusMessage::ErrorMessage ? r.errorMessage() : QString();
                w->deleteLater();
                finishPendingSet(dbusName, r);
                if (r.type() == QDBusMessage::ErrorMessage) {
                    if (prior.isValid())
                        insert(key, prior);
                    else
                        clear(key);
                    qWarning("dbusqml: Set of property %s failed (%s: %s) — value restored",
                             qPrintable(dbusName), qPrintable(errName), qPrintable(errMsg));
                    emit propertyWriteFailed(key, errName, errMsg);
                }
            });
    return input;
}

void DBusProxy::finishPendingSet(const QString &dbusName, const QDBusMessage &reply) {
    // P9: latest-wins drain. If a newer value queued while the Set was
    // in flight, send it now (one chained call); otherwise drop the
    // record. Errors on the chained call run the SAME P8 rollback +
    // warn + propertyWriteFailed as the fresh-call paths (the chained
    // watcher below carries the qmlKey/prior from the PendingSet).
    Q_UNUSED(reply);
    auto it = m_pendingSets.find(dbusName);
    if (it == m_pendingSets.end())
        return;
    PendingSet ps = std::move(it.value());
    m_pendingSets.erase(it);
    if (!ps.queued)
        return;
    QDBusMessage msg =
        QDBusMessage::createMethodCall(m_service, m_path, "org.freedesktop.DBus.Properties", "Set");
    msg.setArguments({m_iface, dbusName, QVariant::fromValue(QDBusVariant(ps.latestValue))});
    QDBusPendingCall call = m_bus.asyncCall(msg, m_callTimeout);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    PendingSet ps2;
    ps2.watcher = watcher;
    ps2.latestValue = ps.latestValue;
    ps2.qmlKey = ps.qmlKey;
    ps2.prior = ps.prior;
    m_pendingSets.insert(dbusName, ps2);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, dbusName](QDBusPendingCallWatcher *w) {
                QDBusMessage r = w->reply();
                w->deleteLater();
                if (r.type() == QDBusMessage::ErrorMessage) {
                    // P8 on the chained call: restore + warn + signal.
                    auto it2 = m_pendingSets.find(dbusName);
                    QString qmlKey = dbusName;
                    QVariant prior;
                    if (it2 != m_pendingSets.end()) {
                        qmlKey = it2.value().qmlKey;
                        prior = it2.value().prior;
                    }
                    if (qmlKey.isEmpty())
                        qmlKey = dbusName;
                    if (prior.isValid())
                        insert(qmlKey, prior);
                    else
                        clear(qmlKey);
                    qWarning("dbusqml: Set of property %s failed (%s: %s) — value restored",
                             qPrintable(dbusName), qPrintable(r.errorName()),
                             qPrintable(r.errorMessage()));
                    emit propertyWriteFailed(qmlKey, r.errorName(), r.errorMessage());
                }
                finishPendingSet(dbusName, r);
            });
}

void DBusProxy::disconnectSignals() {
    if (!m_signalsConnected)
        return;

    // Disconnect each recorded per-signal hook with the exact arguments
    // used at connect time. QtDBus disconnect requires exact-arg match.
    // On a dead bus these are no-ops (QtDBus guards internally) — safe
    // to call from onBusDisconnected.
    for (const QString &sigName : std::as_const(m_connectedSignals)) {
        m_bus.disconnect(QString(), m_connectedPath, m_connectedIface, sigName, this,
                         SLOT(onPropertiesChanged(QDBusMessage)));
    }
    m_bus.disconnect(m_connectedService, m_connectedPath, "org.freedesktop.DBus.Properties",
                     "PropertiesChanged", this, SLOT(onPropertiesChanged(QDBusMessage)));

    m_connectedSignals.clear();
    m_connectedService.clear();
    m_connectedPath.clear();
    m_connectedIface.clear();
    m_signalsConnected = false;
}

void DBusProxy::fetchProperties() {
    QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path,
                                                      "org.freedesktop.DBus.Properties", "GetAll");
    msg.setArguments({m_iface});

    auto pending = m_bus.asyncCall(msg, m_callTimeout);
    auto watcher = new QDBusPendingCallWatcher(pending, this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QVariantMap> reply = *w;
        if (!reply.isError()) {
            QVariantMap props = reply.value();
            for (auto it = props.begin(); it != props.end(); ++it) {
                QString qmlName = dbusPropToQml(it.key());
                // Don't overwrite dynamic method callbacks with property values
                if (!m_methodArgTypes.contains(it.key()) && !m_methodArgTypes.contains(qmlName)) {
                    m_qmlToDbusName.insert(qmlName, it.key());
                    insert(qmlName, unwrapDbus(it.value()));
                }
            }
            m_status = Ready;
        } else {
            m_status = Error;
        }
        w->deleteLater();
        emit statusChanged();
        emit introspectionCompleted();
    });
}

void DBusProxy::onPropertiesChanged(const QDBusMessage &msg) {
    if (msg.type() == QDBusMessage::SignalMessage) {
        QVariantList unwrapped;
        for (const QVariant &arg : msg.arguments())
            unwrapped.append(unwrapDbus(arg));
        emit signalReceived(msg.member(), unwrapped);

        if (msg.member() == "PropertiesChanged") {
            // A2: the interface argument decides — a co-located adaptor's
            // PropertiesChanged at the same path must not contaminate this
            // proxy's map (Nemo/Quickshell precedent).
            if (msg.interface() != QLatin1String("org.freedesktop.DBus.Properties"))
                return;
            if (msg.arguments().isEmpty() || msg.arguments().first().toString() != m_iface)
                return;
            if (msg.arguments().size() >= 2) {
                QVariantMap changed = qdbus_cast<QVariantMap>(msg.arguments()[1]);
                for (auto it = changed.begin(); it != changed.end(); ++it) {
                    QString qmlName = dbusPropToQml(it.key());
                    m_qmlToDbusName.insert(qmlName, it.key());
                    insert(qmlName, unwrapDbus(it.value()));
                }
            }
            // A3/D3: invalidated_properties re-Get — the client re-fetches
            // each name; on error the stale value is kept with one warning
            // (never silently dropped, never silently stale).
            if (msg.arguments().size() >= 3) {
                const QStringList invalidated = qdbus_cast<QStringList>(msg.arguments()[2]);
                if (!invalidated.isEmpty())
                    refetchInvalidated(invalidated);
            }
        }
    }
}

void DBusProxy::setupDynamicMethods(const QStringList &methodNames) {
    auto *engine = qmlEngine(this);

    // 1. Drop stale dynamic method keys from the property map.
    //    Without this, switching iface leaves the old iface's methods
    //    live and callable, dispatching D-Bus method-not-found errors.
    for (const QString &key : std::as_const(m_dynamicMethodKeys))
        clear(key);
    m_dynamicMethodKeys.clear();

    // 2. Release cached QJSValues held from prior introspection.
    m_cachedFunctions.clear();

    if (methodNames.isEmpty())
        return;
    if (!engine)
        return;

    // Shared factory — evaluated once per proxy, method names passed as
    // VALUES (not interpolated into JS source). Eliminates JS injection
    // via remote-provided method names.
    static const char kFactorySrc[] =
        "(function(helper, name) {"
        "  return function(...args) { return helper.callMethod(name, args); };"
        "})";
    QJSValue factory = engine->evaluate(QString::fromLatin1(kFactorySrc));
    if (factory.isError()) {
        qWarning("DBusProxy: failed to evaluate method factory: %s",
                 qPrintable(factory.toString()));
        return;
    }

    // Ownership audit (0.7.0): the helper is parented to the proxy. A parented
    // QObject is never collected by the JS GC, so the dynamic method wrappers
    // keep a live helper for the proxy's lifetime — no explicit ownership
    // flip is needed here (parented ⇒ GC-safe).
    auto *helper = new DbusMethodHelper(this, this);
    QJSValue helperObj = engine->newQObject(helper);

    for (const QString &name : methodNames) {
        if (name.isEmpty())
            continue;

        // Validate: D-Bus member names must be valid identifiers
        if (!name.contains(QRegularExpression(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$")))) {
            qWarning("DBusProxy: skipping invalid method name '%s'", qPrintable(name));
            continue;
        }

        QJSValue fn = factory.call({helperObj, QJSValue(name)});
        if (fn.isError()) {
            qWarning("DBusProxy: failed to create method '%s': %s", qPrintable(name),
                     qPrintable(fn.toString()));
            continue;
        }

        m_cachedFunctions.append(fn);
        QString qmlName = dbusPropToQml(name);
        insert(qmlName, QVariant::fromValue(fn));
        m_dynamicMethodKeys.append(qmlName);
    }
}

void DBusProxy::onIntrospectionReady(const QString &xml) {
    m_methodArgTypes.clear();

    DBusIntrospectionData data = parseDBusIntrospection(xml, m_iface);
    QStringList signalNames = data.signalNames;
    QStringList methodNames = data.methodNames;
    m_methodArgTypes = data.methodArgTypes;
    QStringList propertyNames = data.propertyNames;

    // Merge with user-land catalog (interface descriptors from XDG paths and
    // bundled resources). Live introspection wins on arg types; catalog fills
    // in missing methods / signals.
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (auto it = spec->methods.constBegin(); it != spec->methods.constEnd(); ++it) {
            const QString &methodName = it.key();
            if (!methodNames.contains(methodName)) {
                methodNames << methodName;
                m_methodArgTypes.insert(methodName, it.value().argTypes);
                m_methodArgTypes.insert(dbusPropToQml(methodName), it.value().argTypes);
            }
        }
        for (auto it = spec->signals_.constBegin(); it != spec->signals_.constEnd(); ++it) {
            if (!signalNames.contains(it.key()))
                signalNames << it.key();
        }
    } else if (methodNames.isEmpty() && signalNames.isEmpty()) {
        static QSet<QString> warned;
        if (!warned.contains(m_iface)) {
            warned.insert(m_iface);
            qWarning().nospace() << "DBusProxy: interface " << m_iface
                                 << " has no introspection data and no catalog entry. "
                                 << "Falling back to Properties.GetAll for property access. "
                                 << "For method calls use proxy.call(\"MethodName\", args), "
                                 << "or drop " << m_iface << ".xml at "
                                 << QStandardPaths::writableLocation(
                                        QStandardPaths::GenericConfigLocation)
                                 << "/dbusqml/types/";
        }
    }

    if (m_signalsEnabled) {
        for (const QString &sigName : signalNames) {
            m_bus.connect(QString(), m_path, m_iface, sigName, this,
                          SLOT(onPropertiesChanged(QDBusMessage)));
        }

        m_bus.connect(m_service, m_path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                      this, SLOT(onPropertiesChanged(QDBusMessage)));

        // Record exact connect args for exact disconnect later
        m_connectedSignals = signalNames;
        m_connectedService = m_service;
        m_connectedPath = m_path;
        m_connectedIface = m_iface;
        m_signalsConnected = true;
    }

    // Pre-populate null placeholders for every property declared in the
    // introspection XML. This makes keys exist at QML binding-evaluation
    // time, so QQmlPropertyMap's built-in reactivity can update them
    // when GetAll/PropertiesChanged arrive. Without this, properties
    // auto-created by the engine resolve to invalid QVariant (undefined)
    // and never re-evaluate when the real value is later inserted.
    // A16: the wire name is recorded HERE — a write before GetAll backfills
    // the map must go out under the wire name, not the camelCase fallback.
    for (const QString &propName : std::as_const(propertyNames)) {
        QString qmlName = dbusPropToQml(propName);
        m_qmlToDbusName.insert(qmlName, propName);
        if (!contains(qmlName))
            insert(qmlName, QVariant::fromValue(nullptr));
    }

    setupDynamicMethods(methodNames);

    if (m_propertiesEnabled) {
        fetchProperties();
    } else {
        // No property fetch — still need to signal Ready and
        // introspectionCompleted so consumers don't wait forever.
        m_status = Ready;
        emit statusChanged();
        emit introspectionCompleted();
    }
}

// A3/D3: re-Get invalidated property names. Success → the fresh value is
// inserted; error → the stale value is kept and one warning is emitted.
void DBusProxy::refetchInvalidated(const QStringList &names) {
    for (const QString &wireName : names) {
        const QString service = m_service;
        QDBusMessage msg = QDBusMessage::createMethodCall(m_service, m_path,
                                                          "org.freedesktop.DBus.Properties", "Get");
        msg.setArguments({m_iface, wireName});
        auto pending = m_bus.asyncCall(msg, m_callTimeout);
        auto *watcher = new QDBusPendingCallWatcher(pending, this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, wireName, service](QDBusPendingCallWatcher *w) {
                    QDBusPendingReply<QVariant> reply = *w;
                    if (reply.isError()) {
                        qWarning("dbusqml: re-fetch of %s after invalidation failed: %s",
                                 qPrintable(wireName), qPrintable(reply.error().message()));
                    } else if (service == m_service) {
                        const QString qmlName = dbusPropToQml(wireName);
                        m_qmlToDbusName.insert(qmlName, wireName);
                        insert(qmlName, unwrapDbus(reply.value()));
                    }
                    w->deleteLater();
                });
    }
}

void DBusProxy::reloadTypes() {
    DBusCatalog::instance().reload();
}

bool DBusProxy::reactiveBindingsSupported() {
    // Reactive bindings are the only supported mode since 0.3.0 — catalog
    // and introspection pre-population is unconditional. Property kept for
    // consumer compat (they may already read it to detect the capability).
    return true;
}

bool DBusProxy::hasReactiveBindings() const {
    return reactiveBindingsSupported();
}

void DBusProxy::prepopulateFromCatalog() {
    if (m_service.isEmpty() || m_path.isEmpty() || m_iface.isEmpty())
        return;
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (const auto &p : spec->properties) {
            QString qmlName = dbusPropToQml(p.name);
            m_qmlToDbusName.insert(qmlName, p.name);
            insert(qmlName, QVariant::fromValue(nullptr));
        }
    }
}

#include "dbus.moc"

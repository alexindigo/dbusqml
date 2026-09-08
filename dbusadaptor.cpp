#include "dbusadaptor.h"
#include "dbusintrospection.h"
#include "dbuscatalog.h"
#include "dbusconnection.h" // wireMarshalable — shared marshal-boundary guard
#include "dbusheldreply.h"
#include "dbuspathdispatcher.h"
#include "dbustypes.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QJSValue>
#include <QJSValueList>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QRegularExpression>
#include <QTimer>
#include <qqmlinfo.h>

// Map a D-Bus PascalCase member name to the QML camelCase convention.
// Same rule as the proxy's dbusPropToQml: fold only the first character.
// QML forbids uppercase-initial method names, so a spec-faithful D-Bus
// member like "ReadOne" is declared in QML as "readOne".
// A14/D5: the SERVER fold (first character only) — the other documented
// mode of the shared dbusFoldName.
static QString dbusMemberToQml(const QString &name) {
    return dbusFoldName(name, false);
}

// Underscore-prefixed adaptor properties are library meta-config (e.g.
// _signatures), not part of the served D-Bus surface. They are never exported
// via generateXml or Properties.Get/GetAll/Set.
static bool isPrivateProperty(const QString &name) {
    return name.startsWith(QLatin1Char('_'));
}

// Helper: forwards QML signal emissions to D-Bus.
// One relay per signal, with the signal name baked in at construction.
class PropertiesChangedRelay;
class SignalRelay : public QObject {
    Q_OBJECT
public:
    SignalRelay(DBusAdaptor *adaptor, const QString &signalName, QObject *parent = nullptr)
        : QObject(parent), m_adaptor(adaptor), m_name(signalName) {}

public slots:
    void forward() { sendArgs({}); }
    void forward(QVariant a0) { sendArgs({std::move(a0)}); }
    void forward(QVariant a0, QVariant a1) { sendArgs({std::move(a0), std::move(a1)}); }
    void forward(QVariant a0, QVariant a1, QVariant a2) {
        sendArgs({std::move(a0), std::move(a1), std::move(a2)});
    }
    void forward(QVariant a0, QVariant a1, QVariant a2, QVariant a3) {
        sendArgs({std::move(a0), std::move(a1), std::move(a2), std::move(a3)});
    }
    void forward(QVariant a0, QVariant a1, QVariant a2, QVariant a3, QVariant a4) {
        sendArgs({std::move(a0), std::move(a1), std::move(a2), std::move(a3), std::move(a4)});
    }

private:
    // C0 relay guard (the last unguarded value-bearing send site): unwrap
    // QJSValue-carrying args, mirror emitSignal's wireMarshalable warn+skip
    // (signals have no error-reply channel — an unmarshalable arg would kill
    // the connection), and log send failures instead of dropping them.
    void sendArgs(QVariantList raw) {
        for (QVariant &a : raw) {
            if (a.userType() == qMetaTypeId<QJSValue>())
                a = qjsValueToVariant(a.value<QJSValue>());
        }
        QVariantList args;
        args.reserve(raw.size());
        for (const QVariant &a : raw)
            args.append(toDbusVariant(a));
        // A6: when the signal's arg types are declared (_signals / catalog),
        // marshal through them so the wire shape equals the advertised one;
        // a value that cannot produce the declared type warns + skips (a
        // signal has no error-reply channel).
        const QStringList declared = m_adaptor->declaredSignalTypes(m_name);
        if (!declared.isEmpty()) {
            if (declared.size() != args.size()) {
                qWarning("dbusqml: signal %s carries %d args but %d declared — skipping send",
                         qPrintable(m_name), args.size(), declared.size());
                return;
            }
            QVariantList typed;
            typed.reserve(args.size());
            for (int i = 0; i < args.size(); ++i) {
                QVariant v = marshalBySignature(declared.at(i), args.at(i));
                if (!wireMarshalable(v)) {
                    qWarning("dbusqml: signal %s arg %d cannot produce declared type '%s' — "
                             "skipping send",
                             qPrintable(m_name), i, qPrintable(declared.at(i)));
                    return;
                }
                typed << v;
            }
            args = typed;
        }
        for (const QVariant &a : args) {
            if (!wireMarshalable(a)) {
                qWarning("dbusqml: signal %s arg is not marshalable (type %s) — skipping send",
                         qPrintable(m_name), QMetaType(a.userType()).name());
                return;
            }
        }
        QDBusMessage msg =
            QDBusMessage::createSignal(m_adaptor->path(), m_adaptor->iface(), m_name);
        if (!args.isEmpty())
            msg.setArguments(args);
        QDBusConnection conn = busConn();
        if (!conn.send(msg))
            qWarning("dbusqml: signal %s send failed: %s", qPrintable(m_name),
                     qPrintable(conn.lastError().message()));
    }

    QDBusConnection busConn() const {
        return m_adaptor->connection() ? static_cast<QDBusConnection>(*m_adaptor->connection())
                                       : QDBusConnection::sessionBus();
    }
    DBusAdaptor *m_adaptor;
    QString m_name;
};

// Helper: forwards a property's notify signal to a PropertiesChanged
// emission. One relay per served property, the property index baked in.
class PropertiesChangedRelay : public QObject {
    Q_OBJECT
public:
    PropertiesChangedRelay(DBusAdaptor *adaptor, int propIndex, QObject *parent)
        : QObject(parent), m_adaptor(adaptor), m_prop(propIndex) {}

public slots:
    void changed() {
        if (m_adaptor)
            m_adaptor->emitPropertiesChanged(m_prop);
    }

private:
    QPointer<DBusAdaptor> m_adaptor;
    int m_prop;
};

// P0 (for-all-times Phase 0): RAII dispatch-context stack. Each handler
// invocation pushes a fresh PendingCall; destruction restores the outer
// context. Nested dispatches (re-entrant handleMessage on the same
// adaptor) must not clobber the outer call's message/held flag — the
// single-slot m_currentCall did exactly that (inner dispatch overwrote
// it; inner exit cleared m_inDispatch mid-outer-handler).
class DBusAdaptor::DispatchScope {
public:
    explicit DispatchScope(DBusAdaptor *adaptor, const QDBusMessage &msg,
                           const QDBusConnection &conn, const QString &member)
        : m_adaptor(adaptor) {
        m_adaptor->m_callStack.push(m_adaptor->m_currentCall);
        m_adaptor->m_currentCall.msg = msg;
        m_adaptor->m_currentCall.conn = conn;
        m_adaptor->m_currentCall.member = member;
        m_adaptor->m_currentCall.held = false;
        m_adaptor->m_currentCall.reply = nullptr;
        m_savedInDispatch = m_adaptor->m_inDispatch;
        m_adaptor->m_inDispatch = true;
    }
    ~DispatchScope() {
        m_adaptor->m_currentCall = m_adaptor->m_callStack.pop();
        m_adaptor->m_inDispatch = m_savedInDispatch;
    }

private:
    DBusAdaptor *m_adaptor;
    bool m_savedInDispatch = false;
};

DBusAdaptor::DBusAdaptor(QObject *parent)
    : QDBusVirtualObject(parent),
      m_currentCall{QDBusMessage(), QDBusConnection::sessionBus(), QString(), false, nullptr} {}

DBusAdaptor::~DBusAdaptor() {
    // Error out any held reply that was never settled. The adaptor is being
    // destroyed, so the deferred reply can never be answered — send an error
    // reply rather than leaving the caller to time out.
    const auto heldReplies = findChildren<DBusHeldReply *>();
    for (DBusHeldReply *reply : heldReplies) {
        if (!reply->isSettled()) {
            reply->sendError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                             QStringLiteral("adaptor destroyed with reply pending"));
        }
    }

    if (m_attached)
        DBusPathDispatcher::detach(bus(), m_path, m_service, this);
}

void DBusAdaptor::setService(const QString &v) {
    if (m_service == v)
        return;
    m_service = v;
    emit serviceChanged();
}

void DBusAdaptor::setPath(const QString &v) {
    if (m_path == v)
        return;
    m_path = v;
    emit pathChanged();
}

void DBusAdaptor::setIface(const QString &v) {
    if (m_iface == v)
        return;
    m_iface = v;
    emit ifaceChanged();
}

void DBusAdaptor::setConnection(DBusConnection *v) {
    if (m_conn == v)
        return;
    m_conn = v;
    emit connectionChanged();
}

void DBusAdaptor::setSignatures(const QVariantMap &v) {
    if (m_signatures == v)
        return;
    m_signatures = v;
    emit signaturesChanged();
}

void DBusAdaptor::setSignalSpecs(const QVariantMap &v) {
    if (m_signals == v)
        return;
    m_signals = v;
    emit _signalsChanged();
}

void DBusAdaptor::setMemberAliases(const QVariantMap &v) {
    if (m_members == v)
        return;
    m_members = v;
    emit _membersChanged();
}

void DBusAdaptor::setOptionSpecs(const QVariantMap &v) {
    // P10a: per-method option whitelist { Method: { key: sig } }.
    if (m_options == v)
        return;
    m_options = v;
    emit _optionsChanged();
}

void DBusAdaptor::setAllowedSender(const QString &v) {
    // P10b: sender authorization (empty = open).
    if (m_allowedSender == v)
        return;
    m_allowedSender = v;
    emit allowedSenderChanged();
}

void DBusAdaptor::setHeldReplyTimeout(int v) {
    // Phase 9: TTL opt-in (ms, 0 = disabled). Negative clamps to 0.
    if (v < 0)
        v = 0;
    if (m_heldReplyTimeout == v)
        return;
    m_heldReplyTimeout = v;
    emit heldReplyTimeoutChanged();
}

QVariantMap DBusAdaptor::optionWhitelist(const QString &wireMember) const {
    // Ladder-consistent lookup: exact wire name → folded QML name →
    // alias (mirrors declaredOutTypes above).
    auto it = m_options.constFind(wireMember);
    if (it == m_options.constEnd())
        it = m_options.constFind(dbusMemberToQml(wireMember));
    if (it == m_options.constEnd()) {
        const QString aliased = m_members.value(wireMember).toString();
        if (!aliased.isEmpty())
            it = m_options.constFind(aliased);
    }
    if (it == m_options.constEnd())
        return {};
    // The value must be a map { key: sig }; anything else is a typo —
    // loud, and treated as no whitelist.
    if (!it.value().canConvert<QVariantMap>()) {
        qWarning("dbusqml: _options entry for %s is not a map — ignored", qPrintable(wireMember));
        return {};
    }
    return it.value().toMap();
}

void DBusAdaptor::validateOptionSpecs() {
    // FD4: an _options entry is validated against the method's
    // metaobject arity (the only in-arg shape visible without a
    // catalog): an entry naming an UNKNOWN method, or a method with
    // ZERO in-args (no trailing dict possible), → attach-time warning,
    // entry ignored. A method WITH in-args keeps its entry — the
    // dispatch-time shape check (last arg must be a QVariantMap)
    // decides per call.
    const QMetaObject *meta = metaObject();
    for (auto it = m_options.begin(); it != m_options.end();) {
        const QString key = it.key();
        const QStringList candidates = candidateQmlNames(key);
        const QMetaMethod *found = nullptr;
        for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
            QMetaMethod m = meta->method(i);
            if (m.methodType() != QMetaMethod::Method && m.methodType() != QMetaMethod::Slot)
                continue;
            if (candidates.contains(QString::fromLatin1(m.name()))) {
                found = &m;
                break;
            }
        }
        // Copy the arity out (found points at a loop-local copy).
        int arity = -1;
        if (found)
            arity = found->parameterCount();
        if (arity <= 0) {
            qWarning("dbusqml: _options entry for %s names a method without in-args — ignored",
                     qPrintable(key));
            it = m_options.erase(it);
        } else {
            ++it;
        }
    }
}

QVariantMap DBusAdaptor::filterOptions(const QVariantMap &whitelist, const QVariantMap &options,
                                       QString *error) const {
    // xdp xdp_filter_options shape (xdp-utils.c:248-305): unknown keys
    // silently dropped (forward-compat); present-but-mistyped keys → the
    // caller sends InvalidArgs. The FILTERED dict is what the handler
    // receives.
    //
    // Mistype rule (sound for the whitelist's scalar slots): marshal the
    // value against the declared sig, then check the marshaled value
    // still converts back through the same slot. The telling case is a
    // JS string where a numeric slot is declared: toUInt()/toInt() on a
    // non-numeric string yields 0 — silent corruption. So: a declared
    // numeric/bool slot rejects non-numeric/non-bool JS strings (and
    // vice versa: a declared string slot accepts anything via
    // toString()). Container slots (a*, (...), {...}) require the
    // value to already be a list/map of the right shape — checked by
    // attempting the marshal and requiring a valid, same-kind result.
    QVariantMap out;
    for (auto it = options.begin(); it != options.end(); ++it) {
        auto wit = whitelist.constFind(it.key());
        if (wit == whitelist.constEnd())
            continue; // unknown: silently dropped
        const QString sig = wit.value().toString();
        const QVariant &v = it.value();
        bool mistyped = false;
        if (sig == QStringLiteral("s") || sig == QStringLiteral("o") ||
            sig == QStringLiteral("g")) {
            mistyped = false; // everything stringifies
        } else if (sig == QStringLiteral("b")) {
            mistyped = v.userType() != QMetaType::Bool && v.userType() != QMetaType::QString &&
                       v.userType() != QMetaType::Int && v.userType() != QMetaType::UInt;
        } else if (sig == QStringLiteral("y") || sig == QStringLiteral("n") ||
                   sig == QStringLiteral("q") || sig == QStringLiteral("i") ||
                   sig == QStringLiteral("u") || sig == QStringLiteral("x") ||
                   sig == QStringLiteral("t") || sig == QStringLiteral("d")) {
            // Numeric slots: bools never coerce; strings must be numeric.
            if (v.userType() == QMetaType::Bool) {
                mistyped = true;
            } else if (v.userType() == QMetaType::QString) {
                bool ok = false;
                v.toString().toDouble(&ok);
                mistyped = !ok;
            } else if (v.userType() != QMetaType::Int && v.userType() != QMetaType::UInt &&
                       v.userType() != QMetaType::LongLong &&
                       v.userType() != QMetaType::ULongLong && v.userType() != QMetaType::Double) {
                mistyped = true;
            }
        } else {
            // Container slots: the value must already be shaped (list
            // for arrays, map for dicts/structs-as-maps); the marshal
            // must produce a valid result.
            const QVariant marshaled = marshalBySignature(sig, v);
            mistyped = !marshaled.isValid();
        }
        if (mistyped) {
            if (error)
                *error =
                    QStringLiteral("option '%1' has wrong type (expected %2)").arg(it.key(), sig);
            return {};
        }
        out.insert(it.key(), v);
    }
    return out;
}

void DBusAdaptor::setAllowReplacement(bool v) {
    if (m_allowReplacement == v)
        return;
    m_allowReplacement = v;
    emit allowReplacementChanged();
}

void DBusAdaptor::setReplaceExisting(bool v) {
    if (m_replaceExisting == v)
        return;
    m_replaceExisting = v;
    emit replaceExistingChanged();
}

void DBusAdaptor::setQueueOnBusy(bool v) {
    if (m_queueOnBusy == v)
        return;
    m_queueOnBusy = v;
    emit queueOnBusyChanged();
}

void DBusAdaptor::nameAcquiredInternal() {
    emit nameAcquired();
}

void DBusAdaptor::nameLostInternal() {
    emit nameLost();
}

QDBusConnection DBusAdaptor::bus() const {
    if (m_conn)
        return static_cast<QDBusConnection>(*m_conn);
    return QDBusConnection::sessionBus();
}

void DBusAdaptor::componentComplete() {
    // A12/D2: an alias targeting a PRIVATE property is refused everywhere —
    // privacy wins over the alias. Strip it at attach with a warning; the
    // wire name then resolves to nothing (Get/Set/GetAll/XML all refuse).
    for (auto it = m_members.begin(); it != m_members.end();) {
        if (isPrivateProperty(it.value().toString())) {
            qWarning("dbusqml: _members alias %s targets a private property — refused",
                     qPrintable(it.key()));
            it = m_members.erase(it);
        } else {
            ++it;
        }
    }
    // P10a/FD4: an _options entry naming a method without a trailing
    // a{sv} in-arg is an attach-time warning + ignored entry (the
    // whitelist can only filter a trailing options dict).
    validateOptionSpecs();
    if (m_iface.isEmpty())
        qmlInfo(this)
            << "DBusAdaptor: iface is empty — introspection XML will have an empty interface name";

    QDBusConnection conn = bus();
    m_attached = DBusPathDispatcher::attach(conn, m_path, m_service, this, m_allowReplacement,
                                            m_replaceExisting, m_queueOnBusy);
    if (!m_attached)
        return;

    // L1 (ledger-zero): engine-teardown safety. A declarative adaptor has
    // no C++ owner — destroying its QQmlEngine orphans it (no parent, no
    // JS heap) without running the destructor, leaving held replies
    // unsettled (FD2b root cause, fork-VM-proven: orphan alive after
    // engine delete, spy=0). Hook the engine's destroyed() signal: when
    // the engine goes, run the same tail as unregister() — error pending
    // held replies, detach the claim — so live-reload (engine destroy +
    // recreate in one process) never hangs the caller. Queued, idempotent
    // (detach is idempotent; settled replies are skipped), and harmless
    // when the adaptor dies first (guarded QPointer context + m_attached
    // check — the lambda never runs on a dead adaptor).
    if (QQmlEngine *engine = qmlEngine(this)) {
        QObject::connect(
            engine, &QObject::destroyed, this,
            [this] {
                const auto heldReplies = findChildren<DBusHeldReply *>();
                for (DBusHeldReply *reply : heldReplies) {
                    if (!reply->isSettled()) {
                        reply->sendError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                         QStringLiteral("engine destroyed with reply pending"));
                    }
                }
                if (m_attached) {
                    DBusPathDispatcher::detach(bus(), m_path, m_service, this);
                    m_attached = false;
                }
            },
            Qt::QueuedConnection);
    }

    // Auto-connect user-defined QML signals to D-Bus
    const QMetaObject *meta = metaObject();
    static const QStringList builtInSignals = {
        QStringLiteral("destroyed"),         QStringLiteral("objectNameChanged"),
        QStringLiteral("serviceChanged"),    QStringLiteral("pathChanged"),
        QStringLiteral("ifaceChanged"),      QStringLiteral("connectionChanged"),
        QStringLiteral("signaturesChanged"), QStringLiteral("_signalsChanged"),
        QStringLiteral("_membersChanged"),   QStringLiteral("_optionsChanged")};

    // A7a: warn when a QML method folds onto a library mechanism name — the
    // dispatch skip list would silently turn wire calls into UnknownMethod.
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod m = meta->method(i);
        if (m.methodType() != QMetaMethod::Method && m.methodType() != QMetaMethod::Slot)
            continue;
        const QString mname = QString::fromLatin1(m.name());
        if (mname == QStringLiteral("unregister") || mname == QStringLiteral("holdReply") ||
            mname == QStringLiteral("emitSignal") || mname == QStringLiteral("callerService")) {
            qmlInfo(this) << mname
                          << " member cannot be served under this name — declare an "
                             "alias in `_members`";
        }
    }

    // Property notify signals are NOT relayed as broadcast signals (A5 → A6):
    // they drive org.freedesktop.DBus.Properties.PropertiesChanged instead.
    QSet<int> notifyIndexes;
    QList<QPair<int, QMetaMethod>> notifies; // property index → notify signal
    // A4: notify indexes are recorded for EVERY property — the signal loop
    // below must know which signals are property notifies so they never get
    // raw signal relays. Privacy/library exclusions apply at the Properties
    // -Changed RELAY ATTACH (notifies), not before recording.
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        if (!prop.hasNotifySignal())
            continue;
        const QMetaMethod ns = prop.notifySignal();
        notifyIndexes.insert(ns.methodIndex());
        const QString pname = QString::fromLatin1(prop.name());
        if (isPrivateProperty(pname))
            continue;
        // A15: a QObject*-derived property is never advertised (no wire
        // representation) — its relay must not attach either (no
        // PropertiesChanged noise for a value GetAll can never serve).
        if (prop.metaType().flags().testFlag(QMetaType::PointerToQObject))
            continue;
        if (pname == QStringLiteral("objectName") || pname == QStringLiteral("service") ||
            pname == QStringLiteral("path") || pname == QStringLiteral("iface") ||
            pname == QStringLiteral("connection"))
            continue;
        // P10a: library config maps never get PropertiesChanged relays
        // (they are not served properties — same exclusion as Get/GetAll).
        if (pname == QStringLiteral("_options") || pname == QStringLiteral("_signatures") ||
            pname == QStringLiteral("_signals") || pname == QStringLiteral("_members"))
            continue;
        notifies.append({i, ns});
    }

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod sig = meta->method(i);
        if (sig.methodType() != QMetaMethod::Signal)
            continue;
        if (notifyIndexes.contains(sig.methodIndex()))
            continue;
        QString name = QString::fromLatin1(sig.name());
        if (builtInSignals.contains(name))
            continue;
        // A11: qml*-prefixed signals are never advertised (XML filter) — the
        // relay must not broadcast them either (attach-set = XML-set).
        if (name.startsWith(QStringLiteral("qml")))
            continue;

        int paramCount = sig.parameterCount();
        if (paramCount > 5) {
            qmlInfo(this) << "Signal" << name << "has" << paramCount
                          << "parameters — max 5 supported for auto-forwarding";
            continue;
        }

        auto *relay = new SignalRelay(this, advertisedName(name), this);
        int slotIdx = relay->metaObject()->methodOffset() + paramCount;
        QMetaMethod slot = relay->metaObject()->method(slotIdx);
        QByteArray signalSig = "2" + sig.methodSignature();
        QByteArray slotSig = "1" + QByteArray(slot.methodSignature());
        QObject::connect(this, signalSig.constData(), relay, slotSig.constData());
    }

    // Property changes emit org.freedesktop.DBus.Properties.PropertiesChanged
    // (the spec-conformant channel; the client side already subscribes to it).
    for (const auto &n : notifies) {
        const auto *relay = new PropertiesChangedRelay(this, n.first, this);
        const QByteArray sigSig = "2" + n.second.methodSignature();
        QObject::connect(this, sigSig.constData(), relay, "1changed()");
    }
}

void DBusAdaptor::emitPropertiesChanged(int propIndex) {
    const QMetaObject *meta = metaObject();
    if (propIndex < 0 || propIndex >= meta->propertyCount())
        return;
    QMetaProperty prop = meta->property(propIndex);
    const QString qmlName = QString::fromLatin1(prop.name());

    QVariant val = prop.read(this);
    if (val.userType() == qMetaTypeId<QJSValue>())
        val = qjsValueToVariant(val.value<QJSValue>());
    val = toDbusVariantNested(val);

    QDBusMessage msg =
        QDBusMessage::createSignal(m_path, QStringLiteral("org.freedesktop.DBus.Properties"),
                                   QStringLiteral("PropertiesChanged"));
    if (wireMarshalable(val)) {
        msg.setArguments({m_iface, QVariantMap{{advertisedName(qmlName), val}}, QStringList()});
    } else {
        // A value with no wire representation goes into invalidated_properties
        // instead of the changed dict (0.5.2 guard precedent — GetAll skips it
        // for the same reason).
        qWarning("dbusqml: property %s on %s is not marshalable (type %s) — reporting as "
                 "invalidated",
                 qPrintable(qmlName), qPrintable(m_iface), QMetaType(val.userType()).name());
        msg.setArguments({m_iface, QVariantMap(), QStringList{advertisedName(qmlName)}});
    }
    QDBusConnection conn = bus();
    if (!conn.send(msg))
        qmlInfo(this) << conn.lastError();
}

QString DBusAdaptor::introspect(const QString &) const {
    return generateXml();
}

// Convert a QJSValue to QVariant for D-Bus marshaling. Gadget types
// (DBus::Variant, DBus::Dict, etc.) are preserved by QJSValue::toVariant()
// in Qt 6 when the gadget's metatype is registered — which the plugin's
// static initializer ensures.
//
// No shape-guessing here: a former heuristic treated any single-"value"-key
// map as a flattened gadget (and its all-doubles list payload as a struct),
// silently turning a legitimate `{value: 42}` dict into `v(i)`. Gadgets
// survive QJSValue conversion intact; a flattened-gadget shape is a real
// dict. Explicit typing is `new DBusQML.variant(x)` / `struct_` — the
// documented contract since 0.3.x/0.4.0.
QVariant qjsValueToVariant(const QJSValue &jsval) {
    return jsval.toVariant();
}

void DBusAdaptor::emitSignal(const QString &name, const QJSValue &arguments) {
    QDBusMessage msg = QDBusMessage::createSignal(m_path, m_iface, name);
    if (!arguments.isUndefined()) {
        QVariantList args;
        if (arguments.isArray()) {
            int len = arguments.property(QStringLiteral("length")).toInt();
            for (int i = 0; i < len; ++i) {
                QJSValue item = arguments.property(i);
                args.append(toDbusVariant(qjsValueToVariant(item)));
            }
        } else {
            args.append(toDbusVariant(qjsValueToVariant(arguments)));
        }
        // A6: declared signal types at emission (the emitSignal path — the
        // exact inverse of the out-args rule): the wire shape must equal the
        // advertised one when the types are declared.
        const QStringList declared =
            declaredSignalTypes(advertisedName(name).isEmpty() ? name : advertisedName(name));
        if (!declared.isEmpty()) {
            if (declared.size() != args.size()) {
                qWarning("dbusqml: signal %s carries %d args but %d declared — skipping send",
                         qPrintable(name), args.size(), declared.size());
                return;
            }
            QVariantList typed;
            for (int i = 0; i < args.size(); ++i) {
                QVariant v = marshalBySignature(declared.at(i), args.at(i));
                if (!wireMarshalable(v)) {
                    qWarning("dbusqml: signal %s arg %d cannot produce declared type '%s' — "
                             "skipping send",
                             qPrintable(name), i, qPrintable(declared.at(i)));
                    return;
                }
                typed << v;
            }
            args = typed;
        }
        // Robustness guard: an unmarshalable arg would kill the connection.
        // Signals have no error-reply channel, so warn and skip the send.
        for (const QVariant &a : std::as_const(args)) {
            if (!wireMarshalable(a)) {
                qWarning("dbusqml: signal %s arg is not marshalable (type %s) — skipping send",
                         qPrintable(name), QMetaType(a.userType()).name());
                return;
            }
        }
        msg.setArguments(args);
    }
    QDBusConnection conn = bus();
    if (!conn.send(msg))
        qmlInfo(this) << conn.lastError();
}
// Map a QMetaType to its D-Bus type signature.
static QString metaTypeToDbusSignature(int typeId) {
    // B8: gadget types name their own wire codes (o/g/h) instead of "v".
    if (typeId == qMetaTypeId<QDBusObjectPath>())
        return QStringLiteral("o");
    if (typeId == qMetaTypeId<QDBusSignature>())
        return QStringLiteral("g");
    if (typeId == qMetaTypeId<QDBusUnixFileDescriptor>())
        return QStringLiteral("h");
    switch (typeId) {
    case QMetaType::UChar:
        return QStringLiteral("y");
    case QMetaType::Float:
        return QStringLiteral("d");
    case QMetaType::Long:
        return QStringLiteral("x");
    case QMetaType::Bool:
        return QStringLiteral("b");
    case QMetaType::Int:
        return QStringLiteral("i");
    case QMetaType::UInt:
        return QStringLiteral("u");
    case QMetaType::Short:
        return QStringLiteral("n");
    case QMetaType::UShort:
        return QStringLiteral("q");
    case QMetaType::LongLong:
        return QStringLiteral("x");
    case QMetaType::ULongLong:
        return QStringLiteral("t");
    case QMetaType::Double:
        return QStringLiteral("d");
    case QMetaType::QString:
        return QStringLiteral("s");
    case QMetaType::QByteArray:
        return QStringLiteral("ay");
    case QMetaType::QStringList:
        return QStringLiteral("as");
    case QMetaType::QVariantList:
        return QStringLiteral("av");
    case QMetaType::QVariantMap:
        return QStringLiteral("a{sv}");
    case QMetaType::QVariant:
        return QStringLiteral("v");
    default:
        return QStringLiteral("v");
    }
}

QString DBusAdaptor::generateXml() const {
    const QMetaObject *meta = metaObject();
    QString xml;
    xml += QStringLiteral("  <interface name=\"%1\">\n").arg(m_iface);

    // Signals — the naming ladder, deduped by case-folded name across tiers:
    // explicit _signals → catalog declaration → QML-declared (metaobject,
    // wire-cased via the fold). First tier that declares a folded name wins.
    QSet<QString> servedSignals; // folded names already advertised
    auto addSignal = [&](const QString &wireName, const QStringList &argTypes,
                         const QStringList &argNames) {
        if (wireName.isEmpty())
            return;
        const QString folded = dbusMemberToQml(wireName);
        if (servedSignals.contains(folded))
            return;
        servedSignals.insert(folded);
        xml += QStringLiteral("    <signal name=\"%1\">\n").arg(wireName);
        for (int j = 0; j < argTypes.size(); ++j) {
            const QString argName =
                j < argNames.size() ? argNames.at(j) : QStringLiteral("arg%1").arg(j);
            xml += QStringLiteral("      <arg name=\"%1\" type=\"%2\"/>\n")
                       .arg(argName, argTypes.at(j));
        }
        xml += QStringLiteral("    </signal>\n");
    };

    // Tier 1: explicit _signals — wire name → concatenated arg signature.
    for (auto it = m_signals.cbegin(); it != m_signals.cend(); ++it) {
        const QString sig = it.value().toString();
        QStringList types;
        int pos = 0;
        bool ok = true;
        while (pos < sig.size()) {
            const QString t = firstCompleteType(sig, pos);
            if (t.isEmpty()) {
                ok = false;
                break;
            }
            types << t;
        }
        if (!ok) {
            qWarning("dbusqml: _signals entry %s has a malformed signature '%s' — skipped",
                     qPrintable(it.key()), qPrintable(sig));
            continue;
        }
        addSignal(it.key(), types, {});
    }

    // Tier 2: catalog-declared signals.
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (const auto &sig : spec->signals_)
            addSignal(sig.name, sig.argTypes, {});
    }

    // Property notify signals are never advertised (they are emitted as
    // org.freedesktop.DBus.Properties.PropertiesChanged instead).
    QSet<QString> notifyNames;
    for (int i = 0; i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        if (prop.hasNotifySignal())
            notifyNames.insert(QString::fromLatin1(prop.notifySignal().name()));
    }

    // Tier 3: QML-declared signals, wire-cased via the fold (Q4 — emission
    // and introspection can never diverge; SignalRelay emits the same name).
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Signal)
            continue;
        const QString name = QString::fromLatin1(method.name());
        if (name.startsWith(QStringLiteral("qml")))
            continue;
        if (notifyNames.contains(name))
            continue;
        // A11: the XML set equals the attach set — a >5-param signal is not
        // relayed (max 5 supported), so it is not advertised either.
        if (method.parameterCount() > 5) {
            qmlInfo(this) << "Signal" << name << "has" << method.parameterCount()
                          << "parameters — max 5 supported for auto-forwarding";
            continue;
        }
        QStringList types;
        const auto paramTypes = method.parameterTypes();
        for (const auto &t : paramTypes)
            types << metaTypeToDbusSignature(QMetaType::fromName(t).id());
        const auto paramNames = method.parameterNames();
        QStringList names;
        for (const auto &n : paramNames)
            names << QString::fromLatin1(n);
        addSignal(advertisedName(name), types, names);
    }

    // Properties — wire-cased names via the ladder. Offset-based: the
    // library's own base-class properties (service/path/iface/connection,
    // objectName) sit below propertyOffset and can never appear here by
    // construction — no name filter needed.
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        QMetaProperty prop = meta->property(i);
        QString name = QString::fromLatin1(prop.name());
        if (isPrivateProperty(name))
            continue;
        // A QObject*-derived property has no D-Bus representation — advertising
        // it (the signature fallback would promise "v") offers a value GetAll
        // can never serve. var (QVariant-typed) properties stay advertised "v".
        if (prop.metaType().flags().testFlag(QMetaType::PointerToQObject))
            continue;

        QString dbusType = metaTypeToDbusSignature(static_cast<int>(prop.typeId()));

        QString access = prop.isWritable() ? QStringLiteral("readwrite") : QStringLiteral("read");
        // A9: the catalog's declared type/access win over the metaobject
        // inference (a declared `u` must not serve as metaobject `i`/`v`).
        if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
            for (const auto &p : spec->properties) {
                if (p.name == name || dbusMemberToQml(p.name) == name) {
                    if (!p.type.isEmpty())
                        dbusType = p.type;
                    if (!p.access.isEmpty())
                        access = p.access;
                    break;
                }
            }
        }
        xml += QStringLiteral("    <property name=\"%1\" type=\"%2\" access=\"%3\"/>\n")
                   .arg(advertisedName(name), dbusType, access);
    }

    // Methods — iterate over Q_INVOKABLE/Q_SLOTS methods. Offset-based: the
    // library's own Q_INVOKABLEs and Qt's methods sit below methodOffset and
    // can never appear here by construction. The qml* prefix filter stays —
    // a user-declared qml-prefixed method is not a library name.
    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        QString name = QString::fromLatin1(method.name());
        if (name.startsWith(QStringLiteral("qml")))
            continue;
        // A10: library-mechanism names are never served — the XML must not
        // advertise what dispatch refuses (advertised-but-UnknownMethod).
        if (name == QStringLiteral("holdReply") || name == QStringLiteral("unregister") ||
            name == QStringLiteral("emitSignal") || name == QStringLiteral("callerService"))
            continue;

        const QString wireName = advertisedName(name);
        xml += QStringLiteral("    <method name=\"%1\">\n").arg(wireName);

        // In-args: the catalog's declared types replace the metaobject-derived
        // variants. A8: the lookup matches the FOLD (QML name), the EXACT
        // C++ name, and the RESOLVED WIRE name (aliased methods included).
        const int inCount = method.parameterCount();
        QStringList catalogArgTypes;
        if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
            for (const auto &m : spec->methods) {
                if (dbusMemberToQml(m.name) == name || m.name == name || m.name == wireName) {
                    catalogArgTypes = m.argTypes;
                    break;
                }
            }
        }
        const auto paramTypes = method.parameterTypes();
        for (int j = 0; j < inCount; ++j) {
            QString dbusType;
            if (j < catalogArgTypes.size()) {
                dbusType = catalogArgTypes.at(j);
            } else {
                int typeId = QMetaType::fromName(paramTypes.at(j)).id();
                dbusType = metaTypeToDbusSignature(typeId);
            }
            xml += QStringLiteral("      <arg name=\"arg%1\" type=\"%2\" direction=\"in\"/>\n")
                       .arg(j)
                       .arg(dbusType);
        }
        // A7: declared out-args win — a declaration with NO out-args means
        // the method is void (no phantom `result v`); the inference fallback
        // (C++ typed returns) applies only when nothing is declared.
        bool declared = false;
        const QStringList outTypes = declaredOutTypes(wireName, &declared);
        if (declared) {
            for (const QString &t : outTypes)
                xml += QStringLiteral("      <arg type=\"%1\" direction=\"out\"/>\n").arg(t);
        } else if (method.returnType() != QMetaType::Void) {
            QString retType = metaTypeToDbusSignature(method.returnType());
            xml += QStringLiteral("      <arg name=\"result\" type=\"%1\" direction=\"out\"/>\n")
                       .arg(retType);
        }
        xml += QStringLiteral("    </method>\n");
    }

    xml += QStringLiteral("  </interface>\n");
    return xml;
}

// The naming ladder (explicit _members → catalog → first-char-upper fold):
// the wire name advertised for a QML member name (methods and properties).
// The fold is stable inference — same input, same output, forever; wire
// names are never guessed from data.
QString DBusAdaptor::advertisedName(const QString &qmlName) const {
    // 1. Explicit _members alias (wire name → QML name; reverse lookup).
    for (auto it = m_members.cbegin(); it != m_members.cend(); ++it) {
        if (it.value().toString() == qmlName)
            return it.key();
    }
    // 2. Explicit _signals declaration (A5): the declared key IS the wire
    // name — a non-fold-invertible declared signal must be relayed under it.
    for (auto it = m_signals.cbegin(); it != m_signals.cend(); ++it) {
        if (it.key() == qmlName || dbusMemberToQml(it.key()) == qmlName)
            return it.key();
    }
    // 3. Catalog name whose fold matches the QML name (methods, properties,
    // and now also DECLARED SIGNALS — the relay's ":can-never-diverge" claim
    // was false for non-fold-invertible catalog signal names).
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (const auto &m : spec->methods) {
            if (dbusMemberToQml(m.name) == qmlName)
                return m.name;
        }
        for (const auto &p : spec->properties) {
            if (dbusMemberToQml(p.name) == qmlName)
                return p.name;
        }
        for (const auto &sig : spec->signals_) {
            if (dbusMemberToQml(sig.name) == qmlName)
                return sig.name;
        }
    }
    // 3. Stable inference: the deterministic first-char-uppercase fold.
    if (!qmlName.isEmpty())
        return qmlName.at(0).toUpper() + qmlName.mid(1);
    return qmlName;
}

// Dispatch resolution: incoming wire member → QML name candidates in ladder
// order (explicit _members alias → exact → first-char-lower fold).
QStringList DBusAdaptor::candidateQmlNames(const QString &wireName) const {
    QStringList candidates;
    const QString aliased = m_members.value(wireName).toString();
    if (!aliased.isEmpty())
        candidates << aliased;
    candidates << wireName;
    // A14/D5: both folds accepted on lookup — the server mode (first char)
    // and the client mode (upper-runs collapse). A client-folded key ("url"
    // for URL, "xmlConfig" for XMLConfig) must resolve to the same member.
    const QString folded = dbusMemberToQml(wireName);
    if (!folded.isEmpty() && folded != wireName)
        candidates << folded;
    const QString runsFolded = dbusFoldName(wireName, true);
    if (!runsFolded.isEmpty() && runsFolded != wireName && !candidates.contains(runsFolded))
        candidates << runsFolded;
    return candidates;
}

// A6: resolve the declared per-arg types for a signal, in precedence order
// (explicit _signals → catalog declaration). Empty when undeclared — the
// caller falls back to inference. Matches by exact name or fold.
QStringList DBusAdaptor::declaredSignalTypes(const QString &name) const {
    const QString folded = dbusMemberToQml(name);
    for (auto it = m_signals.cbegin(); it != m_signals.cend(); ++it) {
        if (it.key() == name || dbusMemberToQml(it.key()) == folded) {
            const QString sig = it.value().toString();
            QStringList types;
            int pos = 0;
            bool ok = true;
            while (pos < sig.size()) {
                const QString t = firstCompleteType(sig, pos);
                if (t.isEmpty()) {
                    ok = false;
                    break;
                }
                types << t;
            }
            if (ok)
                return types;
            return {};
        }
    }
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        for (const auto &sig : spec->signals_) {
            if (sig.name == name || dbusMemberToQml(sig.name) == folded)
                return sig.argTypes;
        }
    }
    return {};
}

// Resolve the declared out-arg signatures for a method reply, in precedence
// order: explicit _signatures override → catalog declaration. Returns empty
// when no declaration exists (caller falls back to stable inference).
QStringList DBusAdaptor::declaredOutTypes(const QString &member, bool *found) const {
    if (found)
        *found = false;
    const QString qmlMember = dbusMemberToQml(member);

    // 1. Explicit override — a concatenated signature string, split per-arg.
    auto it = m_signatures.constFind(member);
    if (it == m_signatures.constEnd())
        it = m_signatures.constFind(qmlMember);
    if (it == m_signatures.constEnd()) {
        // A13: the matched ALIAS name is tried too (a _signatures key
        // carrying the aliased QML name was silently ignored).
        const QString aliased = m_members.value(member).toString();
        if (!aliased.isEmpty())
            it = m_signatures.constFind(aliased);
    }
    if (it != m_signatures.constEnd()) {
        if (found)
            *found = true; // A7: declared (possibly empty = declared-void)
        QStringList out;
        const QString sig = it.value().toString();
        int pos = 0;
        while (pos < sig.size()) {
            const QString argSig = firstCompleteType(sig, pos);
            if (argSig.isEmpty()) {
                // B10: a declared signature that breaks mid-parse is LOUD
                // (the proxy's twin already is); the parsed prefix stands.
                qWarning("dbusqml: declared signature '%s' for %s breaks mid-parse — partial "
                         "out-args used",
                         qPrintable(sig), qPrintable(member));
                break;
            }
            out << argSig;
        }
        return out;
    }

    // 2. Catalog declaration (bundled or user XML). A8: the keys are wire
    // names — the caller may present the wire name directly.
    if (auto spec = DBusCatalog::instance().lookup(m_iface)) {
        auto mit = spec->methods.constFind(member);
        if (mit == spec->methods.constEnd())
            mit = spec->methods.constFind(qmlMember);
        if (mit != spec->methods.constEnd()) {
            if (found)
                *found = true; // A7: declared (possibly empty = declared-void)
            return mit->outTypes;
        }
    }
    return {};
}

bool DBusAdaptor::handleMessage(const QDBusMessage &msg, const QDBusConnection &) {
    QDBusConnection conn = bus();
    const QString interface = msg.interface();
    const QString member = msg.member();
    const QVariantList args = msg.arguments();
    const QMetaObject *meta = metaObject();
    const bool replyRequired = msg.isReplyRequired();
    // B4: honor NO_REPLY_EXPECTED — the handler runs, but no reply (success
    // or error) is ever constructed or sent when the caller didn't ask for
    // one. All Properties-interface sends below go through this guard.
    auto sendReply = [&](const QDBusMessage &reply) {
        if (replyRequired)
            conn.send(reply);
    };

    // P10b: sender authorization (xdp-request.c:121-139) — methods AND
    // Properties surfaces, BEFORE any handler runs. Empty = open.
    // Composes with Phase 4 (consumer sets allowedSender from
    // callerService() at Request creation — the xdp per-caller-object
    // pattern).
    if (!m_allowedSender.isEmpty() && msg.service() != m_allowedSender) {
        sendReply(
            msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.AccessDenied"),
                                 QStringLiteral("sender %1 is not authorized").arg(msg.service())));
        return true;
    }

    // Properties interface
    if (interface == QStringLiteral("org.freedesktop.DBus.Introspectable"))
        return false;

    if (interface == QStringLiteral("org.freedesktop.DBus.Properties")) {
        // Validate the interface name if provided
        if (!args.isEmpty()) {
            QString reqIface = args[0].toString();
            if (!reqIface.isEmpty() && reqIface != m_iface) {
                sendReply(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such interface: %1").arg(reqIface)));
                return true;
            }
        }

        if (member == QStringLiteral("Get") && args.size() >= 2) {
            QString propName = args[1].toString();
            if (isPrivateProperty(propName)) {
                sendReply(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            // P10a: library config maps are not served properties.
            if (propName == QStringLiteral("_options") ||
                propName == QStringLiteral("_signatures") ||
                propName == QStringLiteral("_signals") || propName == QStringLiteral("_members")) {
                sendReply(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            // A2: dual lookup — exact QML name, then the folded wire name
            // (Get("iface", "Version") must find `property int version`),
            // mirroring the method dispatch's exact→folded order.
            const QStringList propCandidates = candidateQmlNames(propName);
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                const QString name = QString::fromLatin1(prop.name());
                // A14/D5: both folds accepted on lookup — a client-folded
                // ("xmlConfig") and server-folded ("xMLConfig") wire name
                // both resolve to the QML property XMLConfig.
                if (!propCandidates.contains(name) &&
                    dbusFoldName(name, true) != dbusFoldName(propName, true))
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = qjsValueToVariant(val.value<QJSValue>());
                // V-providing slot: the explicit QDBusVariant wrap below is the
                // reply's "v", so gadget values contribute their payload.
                val = toDbusVariantNested(val);
                if (!wireMarshalable(val)) {
                    qWarning("dbusqml: property %s on %s is not marshalable (type %s) — "
                             "replying InvalidArgs",
                             qPrintable(propName), qPrintable(m_iface),
                             QMetaType(val.userType()).name());
                    sendReply(msg.createErrorReply(
                        QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                        QStringLiteral("Property not marshalable: %1").arg(propName)));
                    return true;
                }
                // D-Bus spec: Get returns a variant (signature "v")
                sendReply(msg.createReply(QVariantList{QVariant::fromValue(QDBusVariant(val))}));
                return true;
            }
            // Unknown property
            sendReply(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                           QStringLiteral("No such property: %1").arg(propName)));
            return true;
        }
        if (member == QStringLiteral("GetAll") && args.size() >= 1) {
            QVariantMap props;
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                QString name = QString::fromLatin1(prop.name());
                if (isPrivateProperty(name))
                    continue;
                // The service/path/iface/connection built-ins are FINAL and
                // cannot be shadowed, so no name filter is needed for them;
                // QObject's objectName still needs the manual exclusion.
                if (name == QStringLiteral("objectName"))
                    continue;
                QVariant val = prop.read(this);
                if (val.userType() == qMetaTypeId<QJSValue>())
                    val = qjsValueToVariant(val.value<QJSValue>());
                // V-providing slot: each a{sv} map value carries its own "v",
                // so gadget values contribute their payload (single wrap).
                val = toDbusVariantNested(val);
                // P10a: _options/_signatures/_signals/_members are
                // library config, not served properties (offset
                // discipline already protects the XML; GetAll joins it
                // so busctl never sees a bogus "Options" property).
                if (name == QStringLiteral("_options") || name == QStringLiteral("_signatures") ||
                    name == QStringLiteral("_signals") || name == QStringLiteral("_members"))
                    continue;
                if (!wireMarshalable(val)) {
                    // GetAll cannot represent a per-property error — skip.
                    // Skipping also keeps introspection (busctl populates its
                    // property values via GetAll) from killing the process.
                    qWarning("dbusqml: skipping non-marshalable property reply on %s in GetAll "
                             "(type %s)",
                             qPrintable(m_iface), QMetaType(val.userType()).name());
                    continue;
                }
                props.insert(advertisedName(name), val);
            }
            sendReply(msg.createReply(QVariantList{props}));
            return true;
        }
        if (member == QStringLiteral("Set") && args.size() >= 3) {
            QString propName = args[1].toString();
            if (isPrivateProperty(propName)) {
                sendReply(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            // P10a: library config maps are not served properties.
            if (propName == QStringLiteral("_options") ||
                propName == QStringLiteral("_signatures") ||
                propName == QStringLiteral("_signals") || propName == QStringLiteral("_members")) {
                sendReply(
                    msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                         QStringLiteral("No such property: %1").arg(propName)));
                return true;
            }
            QVariant value = unwrapDbus(args[2]);
            // A2: same dual lookup as Get — exact QML name, then folded wire name.
            // A14/D5: both folds accepted on lookup (client vs server fold).
            const QStringList propCandidates = candidateQmlNames(propName);
            for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
                QMetaProperty prop = meta->property(i);
                const QString name = QString::fromLatin1(prop.name());
                if ((!propCandidates.contains(name) &&
                     dbusFoldName(name, true) != dbusFoldName(propName, true)) ||
                    !prop.isWritable())
                    continue;
                // C1: a failed write is a caller error — reply InvalidArgs with
                // the property unchanged, not a silent empty success (the
                // 0.5.2 Get/GetAll guard precedent, on the last unguarded
                // Properties path).
                if (!prop.write(this, value)) {
                    sendReply(msg.createErrorReply(
                        QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                        QStringLiteral("Cannot convert value for property %1").arg(propName)));
                    return true;
                }
                // D-Bus spec: Set must send an empty reply
                sendReply(msg.createReply());
                return true;
            }
            // Unknown property
            sendReply(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"),
                                           QStringLiteral("No such property: %1").arg(propName)));
            return true;
        }
        return false;
    }

    // Method dispatch
    QVariantList dbusArgs = msg.arguments();
    // Unwrap D-Bus containers so QML/JS receives traversable data
    // (arrays, dicts, structs) instead of opaque QDBusArgument objects.
    for (QVariant &a : dbusArgs)
        a = unwrapDbus(a);

    // P10a: served option-whitelist. When the method declares an
    // _options entry AND the last in-arg is a{sv}, filter the dict
    // BEFORE dispatch: unknown keys silently dropped, mistyped keys →
    // InvalidArgs error reply (xdp semantics). The FILTERED dict is
    // what the handler receives.
    {
        const QVariantMap whitelist = optionWhitelist(member);
        if (!whitelist.isEmpty() && !dbusArgs.isEmpty()) {
            const QVariant &last = dbusArgs.last();
            QVariantMap opts;
            bool isOptionsDict = false;
            if (last.userType() == qMetaTypeId<QVariantMap>()) {
                opts = last.toMap();
                isOptionsDict = true;
            }
            if (isOptionsDict) {
                QString filterError;
                const QVariantMap filtered = filterOptions(whitelist, opts, &filterError);
                if (!filterError.isEmpty()) {
                    sendReply(msg.createErrorReply(
                        QStringLiteral("org.freedesktop.DBus.Error.InvalidArgs"), filterError));
                    return true;
                }
                dbusArgs.last() = filtered;
            }
        }
    }

    // D-Bus members are PascalCase; QML methods are camelCase (QML
    // forbids uppercase-initial names). Resolution order (the naming
    // ladder): explicit _members alias → exact match (C++ Q_INVOKABLEs can
    // be PascalCase) → the folded name.
    const QString qmlMember = dbusMemberToQml(member);
    const QStringList memberCandidates = candidateQmlNames(member);
    QString matchedName; // the QML method name that matched

    for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
        QMetaMethod method = meta->method(i);
        if (method.methodType() != QMetaMethod::Method && method.methodType() != QMetaMethod::Slot)
            continue;
        const QString methodName = QString::fromLatin1(method.name());
        // holdReply()/unregister()/emitSignal() are library mechanisms for
        // the handler, not D-Bus methods — never dispatch to them over the
        // wire (A10/D1: the load warning for emitSignal becomes true).
        if (methodName == QStringLiteral("holdReply") ||
            methodName == QStringLiteral("unregister") ||
            methodName == QStringLiteral("emitSignal") ||
            methodName == QStringLiteral("callerService"))
            continue;
        if (!memberCandidates.contains(methodName)) {
            continue;
        }
        matchedName = methodName;

        if (method.parameterCount() != dbusArgs.size()) {
            continue;
        }

        QVariant retVal;
        bool invoked = false;
        // P0 tail state: captured INSIDE the DispatchScope lifetime (the
        // scope object below), read AFTER it ends. The scope restores the
        // outer context on destruction, so the tail must not read
        // m_currentCall afterwards.
        bool heldForThisCall = false;

        // P0 (for-all-times Phase 0): RAII dispatch scope — pushes a fresh
        // context so holdReply() works synchronously inside the handler;
        // destruction restores the outer context (nested dispatches must
        // not clobber it).
        {
            DispatchScope scope(this, msg, conn, member);
            QQmlEngine *engine = qmlEngine(this);
            if (engine) {
                // Ownership preservation (0.8.0): the only hazard is
                // newQObject()'s side effect — it re-marks the wrapped QObject as
                // JavaScriptOwnership. Record the current ownership, wrap, and
                // restore immediately: the adaptor keeps its pre-dispatch
                // ownership through the call. Declarative adaptors stay
                // CppOwnership (no-op); dynamically created adaptors stay
                // JavaScriptOwnership — so QML destroy() works anywhere,
                // including inside the dispatched handler (deletion is deferred
                // until after the current script block, i.e. after the dispatch
                // returns), and GC collects an abandoned adaptor even while a
                // reply it can no longer answer is pending (the destructor errors
                // pending callers). Spike-verified: the thisObj QJSValue is a GC
                // root for the duration of the dispatch (plans/
                // ownership-preserve-v0.8.0/spike-findings.md).
                //
                // The adaptor is wrapped as a QJSValue and invoked through
                // callWithInstance. This avoids building a JS source string
                // (which mishandles arrays/dicts and stringifies numeric args
                // without escaping) and works cleanly for multiple adaptor
                // instances sharing one engine.
                const QQmlEngine::ObjectOwnership priorOwnership =
                    QQmlEngine::objectOwnership(this);
                QJSValue thisObj = engine->newQObject(this);
                QQmlEngine::setObjectOwnership(this, priorOwnership);
                // A1: the matched QML name (the _members alias) goes FIRST —
                // aliased handlers must run through the JS path (named error
                // replies + precision-safe 64-bit delivery), never fall to the
                // C++ invoke fallback, which swallows thrown errors into an
                // empty success reply and loses int64 precision to a JS Number.
                QJSValue fn = thisObj.property(matchedName);
                if (!fn.isCallable() && member != matchedName)
                    fn = thisObj.property(member);
                if (!fn.isCallable() && qmlMember != member && qmlMember != matchedName)
                    fn = thisObj.property(qmlMember);
                if (fn.isCallable()) {
                    QJSValueList jsArgs;
                    jsArgs.reserve(dbusArgs.size());
                    for (const QVariant &arg : std::as_const(dbusArgs))
                        jsArgs << precisionSafeToScriptValue(engine, arg);
                    QJSValue result = fn.callWithInstance(thisObj, jsArgs);
                    // P0 (for-all-times Phase 0): thrown-vs-returned
                    // discrimination. Empirically (VM probe, QJSEngine):
                    // callWithInstance reports isError()==false for thrown
                    // primitives AND hasError()==false afterwards — a thrown
                    // 'x' is IDENTICAL to a returned 'x' at this layer
                    // (string QJSValue either way; thrown undefined identical
                    // to returned undefined). So pure classification is
                    // impossible here — instead the SCOPE of the throw
                    // contract is the dispatch shape: a handler that called
                    // holdReply() has no legitimate bare-primitive return
                    // (held replies settle via the held object, and a bare
                    // return with held==true is already warned+ignored).
                    // Treat string/number/bool/null results as THROWS
                    // exactly when held==true (P0: the silence was always
                    // and only on the held path); otherwise preserve the
                    // legacy return-value behavior. A bare `undefined`
                    // result is NOT a throw — it is the normal void return
                    // (a handler that only calls holdReply() completes with
                    // undefined); thrown-undefined is undetectable at this
                    // layer and remains a documented limitation. Arrays
                    // (isArray) and plain objects are legitimate multi-out
                    // / map returns even when held — only the scalar bare
                    // kinds route to the error path.
                    const bool heldNow = m_currentCall.held;
                    const bool thrownPrimitive = heldNow && !result.isError() &&
                                                 !result.isArray() && !result.isObject() &&
                                                 (result.isString() || result.isNumber() ||
                                                  result.isBool() || result.isNull());
                    QJSValue thrownValue;
                    const bool callThrew = thrownPrimitive;
                    if (callThrew)
                        thrownValue = result;
                    const bool thrownErrorShape = [&result] {
                        if (!result.isObject() && !result.isVariant())
                            return false;
                        const QJSValue marker = result.property(QStringLiteral("dbusError"));
                        if (marker.isUndefined() || marker.isNull() || !marker.toBool())
                            return false;
                        // A name that fails the grammar still shapes as an error
                        // throw — B11's validation falls back to Failed for it.
                        const QJSValue n = result.property(QStringLiteral("name"));
                        return n.isString();
                    }();
                    if (result.isError() || thrownErrorShape || thrownPrimitive) {
                        // S2: named error replies from handlers. A thrown value
                        // with a D-Bus error shape (DBusQML.DBusUtils.error() —
                        // a map carrying a dotted `name` and a `message`) becomes
                        // that exact error reply; any other JS exception becomes
                        // org.freedesktop.DBus.Error.Failed with the exception
                        // message. Never a silent empty reply, and the handler is
                        // never re-run through the C++ invoke path.
                        //
                        // P0 (for-all-times Phase 0): when the handler deferred
                        // via holdReply(), the error settles the HELD reply via
                        // sendError — never a direct reply on the serial (which
                        // would leave the held object unsettled for a later
                        // double-reply). Exactly-one-reply-per-serial becomes
                        // structural: sendError marks the held object settled.
                        QString errorName = QStringLiteral("org.freedesktop.DBus.Error.Failed");
                        // P0: a thrown primitive carries its string form as
                        // the message (verified: 'primitive-boom' reaches
                        // the caller).
                        QString errorMessage;
                        if (thrownPrimitive)
                            errorMessage = thrownValue.toString();
                        else
                            errorMessage = result.toString();
                        const QJSValue nameVal = result.property(QStringLiteral("name"));
                        const QJSValue msgVal = result.property(QStringLiteral("message"));
                        if (nameVal.isString()) {
                            const QString n = nameVal.toString();
                            // B11: the name must satisfy the D-Bus error-name
                            // grammar (dot-separated identifiers, each starting
                            // with a letter or underscore). An invalid name would
                            // fail at reply marshal — the caller would just time
                            // out. Fall back to Failed with a warning.
                            const bool validName = [n]() {
                                const QStringList parts = n.split(QLatin1Char('.'));
                                if (parts.size() < 2)
                                    return false;
                                for (const QString &p : parts) {
                                    if (p.isEmpty() || !p.at(0).isLetter())
                                        return false;
                                    for (const QChar &c : p) {
                                        if (!(c.isLetterOrNumber() || c == QLatin1Char('_')))
                                            return false;
                                    }
                                }
                                return true;
                            }();
                            if (validName) {
                                errorName = n;
                                if (msgVal.isString())
                                    errorMessage = msgVal.toString();
                            } else if (n.contains(QLatin1Char('.')) || thrownErrorShape) {
                                // B11: a THROWER-declared named error with bad
                                // grammar warns + falls back; a plain JS Error
                                // ("Error") is not a named error — silent Failed.
                                qWarning("dbusqml: invalid error name '%s' — falling back to "
                                         "org.freedesktop.DBus.Error.Failed",
                                         qPrintable(n));
                                if (msgVal.isString())
                                    errorMessage = msgVal.toString();
                            }
                        }
                        qWarning("dbusqml: handler for %s threw %s: %s", qPrintable(member),
                                 qPrintable(errorName), qPrintable(errorMessage));
                        if (m_currentCall.held && !m_currentCall.reply.isNull()) {
                            // P0: settle the HELD reply, not the serial. B4
                            // honored: sendError sends nothing when the caller
                            // set NO_REPLY_EXPECTED.
                            m_currentCall.reply->sendError(errorName, errorMessage);
                        } else if (msg.isReplyRequired()) {
                            if (!conn.send(msg.createErrorReply(errorName, errorMessage)))
                                qWarning("dbusqml: error reply for %s failed to send: %s",
                                         qPrintable(member),
                                         qPrintable(conn.lastError().message()));
                        }
                        return true;
                    }
                    if (!result.isError() && !thrownErrorShape && !callThrew) {
                        retVal = result.isUndefined() ? QVariant() : qjsValueToVariant(result);
                        invoked = true;
                    }
                }
            } // end if (fn.isCallable())
            // Capture THIS call's held flag before the scope ends (the
            // destructor restores the outer context; the tail below must
            // not read m_currentCall afterwards).
            heldForThisCall = m_currentCall.held;
        } // end DispatchScope
        if (!invoked) {
            QByteArray methodName = matchedName.toLatin1();
            // C++ Q_INVOKABLEs take QVariant args (matching the Q_ARG dispatch
            // below). ANY default-constructible return type is captured via a
            // generic return argument and round-trips into the reply — an
            // uncapturable typed return used to silently send an EMPTY reply
            // (0.5.0–0.5.2 captured QVariant only).
            const QMetaType rt = method.returnMetaType();
            bool captureReturn = false;
            // QMetaMethod::invoke is the type-erased entry point (runtime
            // return types cannot use the Q_RETURN_ARG macro). For QVariant
            // returns the argument addresses retVal itself — a data()
            // -addressed return writes a nested QVariant slot and leaves
            // retVal QVariant-typed, which the reply tail rejects.
            QGenericReturnArgument retArg(nullptr, nullptr);
            if (rt.id() == QMetaType::QVariant) {
                captureReturn = true;
                retArg = QGenericReturnArgument(rt.name(), &retVal);
            } else if (rt.id() != QMetaType::UnknownType && rt.id() != QMetaType::Void &&
                       rt.isDefaultConstructible()) {
                retVal = QVariant(rt);
                retArg = QGenericReturnArgument(rt.name(), retVal.data());
                captureReturn = true;
            } else if (rt.id() != QMetaType::UnknownType && rt.id() != QMetaType::Void) {
                qWarning("dbusqml: method %s returns non-default-constructible %s — declare a "
                         "QVariant return for wire-callable C++ methods",
                         qPrintable(matchedName), rt.name());
            }
            switch (dbusArgs.size()) {
            case 0:
                if (captureReturn)
                    invoked = method.invoke(this, Qt::DirectConnection, retArg);
                else
                    invoked = method.invoke(this, Qt::DirectConnection);
                break;
            case 1:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))));
                break;
            case 2:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))));
                break;
            case 3:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))));
                break;
            case 4:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))));
                break;
            case 5:
                if (captureReturn)
                    invoked = method.invoke(
                        this, Qt::DirectConnection, retArg,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(4))));
                else
                    invoked = method.invoke(
                        this, Qt::DirectConnection,
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(0))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(1))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(2))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(3))),
                        QGenericArgument("QVariant", const_cast<QVariant *>(&dbusArgs.at(4))));
                break;
            default:
                qWarning("dbusqml: method %s takes %d arguments — the C++ dispatch path supports "
                         "at most 5; declare fewer parameters",
                         qPrintable(matchedName), int(dbusArgs.size()));
                return false;
            }
        }

        // If the handler deferred the reply via holdReply(), the held reply
        // will settle it later — skip the synchronous tail. The handler's
        // return value (if any) is ignored in that case. Reads the
        // captured flag (NOT m_currentCall — the scope restored the outer
        // context on exit). Loud when the handler ALSO returned a value
        // (fable): a held-path skip with a value is a caller-observable
        // decision, never silent.
        if (heldForThisCall) {
            if (retVal.isValid()) {
                qWarning("dbusqml: handler return value ignored when holdReply() was called "
                         "(member %s — the held reply settles the caller)",
                         qPrintable(member));
            }
            return true;
        }

        // B7: a failed QMetaMethod::invoke is a LOUD Failed error reply —
        // an unchecked invoke used to send a silent EMPTY SUCCESS reply
        // (invalid retVal) for C++-typed handlers whose conversion failed.
        if (!invoked) {
            qWarning("dbusqml: handler invocation failed for %s (conversion error)",
                     qPrintable(member));
            sendReply(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                           QStringLiteral("Handler invocation failed")));
            return true;
        }

        sendMethodReply(conn, msg, member, retVal);
        return true;
    } // end for (candidate methods)

    return false;
} // end handleMessage

DBusHeldReply *DBusAdaptor::holdReply() {
    if (!m_inDispatch) {
        qWarning("dbusqml: holdReply() called outside method dispatch - ignored");
        return nullptr;
    }
    auto *reply = new DBusHeldReply(this);
    reply->setContext(this, m_currentCall.msg, m_currentCall.conn, m_currentCall.member);
    // Ownership audit (0.7.0): CppOwnership while pending — the held reply is
    // the only handle that can answer the caller and must not be GC-collected
    // mid-flight. After settle() it is handed to the JS GC (0.5.0 design,
    // intended); it stays parented to this adaptor, so a parented JS-owned
    // object is never collected and it is destroyed with the adaptor.
    QQmlEngine::setObjectOwnership(reply, QQmlEngine::CppOwnership);
    m_currentCall.held = true;
    m_currentCall.reply = reply;
    // Phase 9: TTL opt-in — arm the expiry timer (single-shot). Settle
    // (send/sendError/expire/destructor-tail) cancels it; expiry settles
    // with Failed. The timer is parented to the REPLY (dies with it);
    // the timeout value is read at hold time (changing the property
    // mid-hold does not re-arm — documented).
    if (m_heldReplyTimeout > 0) {
        QTimer::singleShot(m_heldReplyTimeout, reply, [reply] { reply->expire(); });
    }
    return reply;
}

QString DBusAdaptor::callerService() const {
    // Phase 4: the delivery message's sender, valid only synchronously
    // during dispatch (the DispatchScope owns the message lifetime).
    if (!m_inDispatch) {
        qWarning("dbusqml: callerService() called outside method dispatch - returning empty");
        return {};
    }
    return m_currentCall.msg.service();
}

void DBusAdaptor::unregister() {
    if (!m_attached) {
        qWarning("dbusqml: unregister() on an already detached adaptor (path %s) — ignored",
                 qPrintable(m_path));
        return;
    }

    // Same tail as the destructor: a deferred reply can never be answered
    // once the path is gone — error it rather than leaving the caller to
    // time out.
    const auto heldReplies = findChildren<DBusHeldReply *>();
    for (DBusHeldReply *reply : heldReplies) {
        if (!reply->isSettled()) {
            reply->sendError(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                             QStringLiteral("adaptor unregistered with reply pending"));
        }
    }

    // Detach the path + service reference via the dispatcher registry
    // (idempotent tail). One-way: re-registration is not supported. The
    // QObject stays alive for QML to drop whenever.
    DBusPathDispatcher::detach(bus(), m_path, m_service, this);
    m_attached = false;
}

void DBusAdaptor::sendMethodReply(const QDBusConnection &conn, const QDBusMessage &msg,
                                  const QString &member, const QVariant &retVal) {
    // B4: NO_REPLY_EXPECTED — never construct or send a reply (success or
    // error) when the caller didn't ask for one.
    if (!msg.isReplyRequired())
        return;
    // P0 (v): every send-site result is checked (C0 did this for signals;
    // the reply tail never did). A failed send is loud, never a silent
    // caller timeout.
    auto checkedSend = [&](const QDBusMessage &reply, const char *what) {
        if (!conn.send(reply))
            qWarning("dbusqml: %s for %s failed to send: %s", what, qPrintable(member),
                     qPrintable(conn.lastError().message()));
    };
    const QVariant value = toDbusVariant(retVal);
    if (value.isValid()) {
        // Robustness guard: an unmarshalable payload (e.g. a returned JS
        // function) would otherwise make QtDBus drop the bus connection.
        // Degrade to an error reply — one failed call, never the service.
        if (!wireMarshalable(value)) {
            qWarning("dbusqml: reply for %s is not marshalable (type %s) — sending error reply",
                     qPrintable(member), QMetaType(value.userType()).name());
            checkedSend(msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                             QStringLiteral("Reply value is not marshalable")),
                        "error reply");
            return;
        }

        // Honor a declared reply signature: explicit _signatures override
        // → catalog declaration → stable inference (unchanged). Struct
        // replies now arrive here already in writable-QDBusArgument form
        // (toDbusVariant), so no top-level special case is needed.
        // D6: a DECLARED-VOID method drops the handler's return value with
        // one warning — the declaration is the contract (never a phantom
        // inference out-arg, never a silent wrong-shaped reply).
        bool declared = false;
        const QStringList outTypes = declaredOutTypes(member, &declared);
        if (declared && outTypes.isEmpty()) {
            qWarning("dbusqml: %s is declared void — dropping return value", qPrintable(member));
            checkedSend(msg.createReply(), "declared-void reply");
            return;
        }
        if (outTypes.size() == 1) {
            checkedSend(msg.createReply({marshalBySignature(outTypes.first(), value)}),
                        "method reply");
            return;
        }
        if (outTypes.size() > 1) {
            // Multi-out: the method returned a list of out values.
            const QVariantList values = value.toList();
            QVariantList reply;
            for (int i = 0; i < outTypes.size(); ++i)
                reply << marshalBySignature(outTypes.at(i),
                                            i < values.size() ? values.at(i) : QVariant());
            checkedSend(msg.createReply(reply), "multi-out reply");
            return;
        }
        checkedSend(msg.createReply({value}), "method reply"); // stable inference
    } else {
        checkedSend(msg.createReply(), "void reply"); // void return — no reply args
    }
}

#include "dbusadaptor.moc"

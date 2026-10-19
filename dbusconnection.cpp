#include "dbusconnection.h"
#include "dbuspathdispatcher.h"
#include "dbusutils.h"
#include "dbussignatureslots.h"
#include "dbustypes.h"

#include <QDBusMetaType>

#include <array>
#include <utility>

#include <QAtomicInt>
#include <QDBusArgument>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>
#include <QJSValue>
#include <QJSValueList>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QQmlEngine>
#include <QTimer>

bool wireMarshalable(const QVariant &v) {
    if (!v.isValid())
        return false;
    const int t = v.userType();

    // Containers recurse — a single unmarshalable element poisons the whole.
    if (t == qMetaTypeId<QVariantMap>()) {
        const QVariantMap map = v.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (!wireMarshalable(it.value()))
                return false;
        }
        return true;
    }
    if (t == qMetaTypeId<QVariantList>()) {
        const QVariantList list = v.toList();
        for (const QVariant &e : list) {
            if (!wireMarshalable(e))
                return false;
        }
        return true;
    }
    if (t == qMetaTypeId<QDBusVariant>())
        return wireMarshalable(v.value<QDBusVariant>().variant());
    // QDBusArgument carries its own signature — always marshalable.
    if (t == qMetaTypeId<QDBusArgument>())
        return true;
    // Object paths and signatures must be STRUCTURALLY valid — an invalid
    // path content passes QtDBus's type lookup and fails only at marshal
    // time (the caller would time out with zero diagnostics).
    if (t == qMetaTypeId<QDBusObjectPath>()) {
        const QString p = v.value<QDBusObjectPath>().path();
        if (p.isEmpty() || (!p.startsWith(QLatin1Char('/'))))
            return false;
        for (const QChar &c : p) {
            if (!(c.isLetterOrNumber() || c == QLatin1Char('/') || c == QLatin1Char('_')))
                return false;
        }
        return true;
    }
    if (t == qMetaTypeId<QDBusSignature>()) {
        // A 'g' value is a message signature — validated with the full
        // spec rules (the 255-byte cap in particular: an over-long g value
        // reaches the same libdbus assert as an over-long message
        // signature). Empty stays rejected (the existing contract).
        const QString g = v.value<QDBusSignature>().signature();
        if (g.isEmpty())
            return false;
        return validateSignature(g, SignatureKind::Message);
    }
    // Unix fds are marshalable as their descriptor — an INVALID one is not:
    // it would fail at marshal time with the caller left timing out (B4).
    // A DBusQML.unixFd is converted by toDbusVariant before any send; an
    // unconverted one (or a DBusFdFailure) has no wire form here.
    if (t == qMetaTypeId<QDBusUnixFileDescriptor>())
        return v.value<QDBusUnixFileDescriptor>().isValid();
    if (t == qMetaTypeId<DBus::UnixFd>() || t == qMetaTypeId<DBusFdFailure>())
        return false;
    if (t == QMetaType::Int)
        return true;

    // Anything QtDBus knows a wire signature for (basics, QString, QByteArray,
    // QStringList, QDBusObjectPath, QDBusSignature, registered types). QJSValue,
    // QObject*, and unregistered gadgets return null → false.
    return QDBusMetaType::typeToSignature(QMetaType(t)) != nullptr;
}

namespace {
// The argument's contribution to the outgoing message signature, in encoded
// bytes. A QDBusArgument carries its own signature (built by
// writeBySignature, already validator-gated); a variant contributes "v";
// everything else resolves through QtDBus's type table.
int wireSignatureLength(const QVariant &v) {
    if (v.userType() == qMetaTypeId<QDBusArgument>())
        return v.value<QDBusArgument>().currentSignature().toUtf8().size();
    if (v.userType() == qMetaTypeId<QDBusVariant>())
        return 1;
    const char *sig = QDBusMetaType::typeToSignature(QMetaType(v.userType()));
    return sig ? static_cast<int>(qstrlen(sig)) : 0;
}
} // namespace

QString fdFailureIn(const QVariant &v) {
    const int t = v.userType();
    if (t == qMetaTypeId<DBusFdFailure>())
        return v.value<DBusFdFailure>().reason;
    if (t == qMetaTypeId<QDBusVariant>())
        return fdFailureIn(v.value<QDBusVariant>().variant());
    if (t == qMetaTypeId<QVariantList>()) {
        for (const QVariant &e : v.toList()) {
            const QString r = fdFailureIn(e);
            if (!r.isEmpty())
                return r;
        }
    } else if (t == qMetaTypeId<QVariantMap>()) {
        const QVariantMap map = v.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            const QString r = fdFailureIn(it.value());
            if (!r.isEmpty())
                return r;
        }
    }
    return {};
}

bool checkSendable(const QVariantList &args, QString *err) {
    int aggregate = 0;
    for (int i = 0; i < args.size(); ++i) {
        const QVariant &a = args.at(i);
        if (!wireMarshalable(a)) {
            if (err) {
                const QString fd = fdFailureIn(a);
                *err = fd.isEmpty() ? QStringLiteral("argument %1 is not marshalable").arg(i)
                                    : QStringLiteral("argument %1: %2").arg(i).arg(fd);
            }
            return false;
        }
        aggregate += wireSignatureLength(a);
        if (aggregate > 255) { // DBUS_MAXIMUM_SIGNATURE_LENGTH
            if (err)
                *err = QStringLiteral("message signature exceeds the 255-byte D-Bus limit "
                                      "(argument %1)")
                           .arg(i);
            return false;
        }
    }
    return true;
}

// Convert a QVariant into a native JS value, recursively unwrapping lists
// and maps so the JS side receives real Array / Object instances (with a
// working Array.isArray and iterable/spread semantics), not the array-like
// QVariantList wrappers QQmlEngine::toScriptValue produces by default.
// Precision-safe QVariant → QJSValue: 64-bit ints that don't round-trip
// through a double (|v| above 2^53) are delivered as full-precision decimal
// strings — the C2 contract (QML's JS engine has no BigInt). variantToJs is
// the reply path; this variant is used for dispatch args. CF-20: recursive
// — the old top-level-only form lost precision on nested int64 > 2^53
// (a nested x/t inside a{sv}/av/struct went through the default double
// path). Containers recurse through variantToJs (which applies the scalar
// guard at its leaves); QDBusArgument/QDBusVariant unwrap first so nested
// payloads are visible.
QJSValue precisionSafeToScriptValue(QQmlEngine *engine, const QVariant &v) {
    if (v.userType() == qMetaTypeId<DBus::UnixFd>()) {
        // Delivery into JS: arms the GC-fallback warning, registers the
        // engine for the pressure valve. JS copies share the one state.
        DBus::UnixFd::noteDelivered(v, engine);
        return engine->toScriptValue(v);
    }
    if (v.userType() == QMetaType::LongLong) {
        const qint64 value = v.toLongLong();
        if (static_cast<qint64>(static_cast<double>(value)) != value)
            return engine->toScriptValue(QVariant(QString::number(value)));
    }
    if (v.userType() == QMetaType::ULongLong) {
        const quint64 value = v.toULongLong();
        if (static_cast<quint64>(static_cast<double>(value)) != value)
            return engine->toScriptValue(QVariant(QString::number(value)));
    }
    if (v.userType() == qMetaTypeId<QVariantList>() || v.userType() == qMetaTypeId<QStringList>()) {
        const QVariantList list = v.toList();
        QJSValue arr = engine->newArray(static_cast<quint32>(list.size()));
        for (int i = 0; i < list.size(); ++i)
            arr.setProperty(static_cast<quint32>(i),
                            precisionSafeToScriptValue(engine, list.at(i)));
        return arr;
    }
    if (v.userType() == qMetaTypeId<QVariantMap>()) {
        const QVariantMap map = v.toMap();
        QJSValue obj = engine->newObject();
        for (auto it = map.begin(); it != map.end(); ++it)
            obj.setProperty(it.key(), precisionSafeToScriptValue(engine, it.value()));
        return obj;
    }
    if (v.userType() == qMetaTypeId<QDBusArgument>() || v.userType() == qMetaTypeId<QDBusVariant>())
        return precisionSafeToScriptValue(engine, unwrapDbus(v));
    return engine->toScriptValue(v);
}

QJSValue variantToJs(QQmlEngine *engine, const QVariant &v) {
    const int t = v.userType();
    if (t == qMetaTypeId<QVariantList>() || t == qMetaTypeId<QStringList>()) {
        const QVariantList list = v.toList();
        QJSValue arr = engine->newArray(static_cast<quint32>(list.size()));
        for (int i = 0; i < list.size(); ++i)
            arr.setProperty(static_cast<quint32>(i), variantToJs(engine, list.at(i)));
        return arr;
    }
    if (t == qMetaTypeId<QVariantMap>()) {
        const QVariantMap map = v.toMap();
        QJSValue obj = engine->newObject();
        for (auto it = map.begin(); it != map.end(); ++it)
            obj.setProperty(it.key(), variantToJs(engine, it.value()));
        return obj;
    }
    // C2 — lossless 64-bit delivery (see precisionSafeToScriptValue above).
    return precisionSafeToScriptValue(engine, v);
}

// Recursively unwrap QDBusVariant / QDBusArgument values into plain QVariant
// containers so QML can traverse them as JavaScript objects. Handles nested
// a{sv}, a{ss}, av, as, ao, etc.
QVariant unwrapDbus(const QVariant &v) {
    DecodeState state;
    const QVariant result = unwrapDbus(v, state);
    if (!state.ok)
        qWarning("dbusqml: value could not be decoded");
    return result;
}

// Normalize DBus.* gadgets to marshalable values, recursing into containers.
//
// `nested` describes the enclosing slot: true when that slot already provides
// the D-Bus variant wrapper (an a{sv} dict value, an av list element, a
// DBus::Dict value), false when it does not (a top-level arg, a struct member,
// another variant's payload). A DBus::Variant in a nested slot contributes its
// payload directly (the enclosing "v" wraps it — a nested QDBusVariant would
// otherwise double-wrap to v(v(x))); in a free slot it carries the QDBusVariant
// wrapper itself (so a top-level `variant(x)` is a real "v").
static QVariant toDbusVariantImpl(const QVariant &v, bool nested) {
    int type = v.userType();

    if (type == qMetaTypeId<DBus::Bool>())
        return QVariant::fromValue(v.value<DBus::Bool>().value);
    if (type == qMetaTypeId<DBus::Int16>())
        return QVariant::fromValue(v.value<DBus::Int16>().value);
    if (type == qMetaTypeId<DBus::Int32>())
        return QVariant::fromValue(v.value<DBus::Int32>().value);
    if (type == qMetaTypeId<DBus::Int64>())
        return QVariant::fromValue(v.value<DBus::Int64>().value);
    if (type == qMetaTypeId<DBus::Uint16>())
        return QVariant::fromValue(v.value<DBus::Uint16>().value);
    if (type == qMetaTypeId<DBus::Uint32>())
        return QVariant::fromValue(v.value<DBus::Uint32>().value);
    if (type == qMetaTypeId<DBus::Uint64>())
        return QVariant::fromValue(v.value<DBus::Uint64>().value);
    if (type == qMetaTypeId<DBus::Double>())
        return QVariant::fromValue(v.value<DBus::Double>().value);
    if (type == qMetaTypeId<DBus::Byte>())
        return QVariant::fromValue(v.value<DBus::Byte>().value);
    if (type == qMetaTypeId<DBus::String>())
        return QVariant::fromValue(v.value<DBus::String>().value);
    if (type == qMetaTypeId<DBus::ObjectPath>())
        return QVariant::fromValue(v.value<DBus::ObjectPath>().value);
    if (type == qMetaTypeId<DBus::Signature>())
        return QVariant::fromValue(v.value<DBus::Signature>().value);
    if (type == qMetaTypeId<DBus::Bytes>())
        return QVariant::fromValue(v.value<DBus::Bytes>().value);
    if (type == qMetaTypeId<DBus::UnixFd>()) {
        // Marshals as `h`. The message gets its OWN dup: QtDBus keeps a sent
        // message until its pending call dies, and the sender's close()
        // must close the wrapper's descriptor now, not when the call ends.
        // A closed one cannot be sent.
        const DBus::UnixFd fd = v.value<DBus::UnixFd>();
        if (!fd.isValid())
            return QVariant::fromValue(
                DBusFdFailure{QStringLiteral("a closed DBusQML.unixFd cannot be sent")});
        return QVariant::fromValue(QDBusUnixFileDescriptor(fd.fileDescriptor()));
    }
    if (type == qMetaTypeId<DBus::Struct>()) {
        // A raw DBus::Struct gadget has no fixed D-Bus signature (variable
        // members), so handing it to QtDBus marshals an empty struct `()` and
        // corrupts the connection. Emit the writable-QDBusArgument form instead
        // — that cross-marshals in EVERY position (variant payloads, map/list
        // values, signal args, call args), not just top-level replies.
        QVariantList members = v.value<DBus::Struct>().value;
        // Struct members are variant-free slots: a Variant member must carry
        // its own "v" (so (ssv) stays (ssv), not (ssx)).
        for (auto &m : members) {
            if (m.userType() != qMetaTypeId<DBus::Struct>())
                m = toDbusVariantImpl(m, false);
            if (!fdFailureIn(m).isEmpty())
                return m; // the struct cannot be sent; the boundary reports why
        }
        QDBusArgument arg;
        arg << DBus::Struct(members);
        return QVariant::fromValue(arg);
    }
    // Containers provide the "v" for their values: recurse as nested.
    if (type == qMetaTypeId<QVariantMap>()) {
        QVariantMap m = v.toMap();
        for (auto it = m.begin(); it != m.end(); ++it)
            it.value() = toDbusVariantImpl(it.value(), true);
        return QVariant::fromValue(m);
    }
    if (type == qMetaTypeId<QVariantList>()) {
        QVariantList list = v.toList();
        for (auto &e : list)
            e = toDbusVariantImpl(e, true);
        return QVariant::fromValue(list);
    }
    if (type == qMetaTypeId<DBus::Dict>()) {
        // Unwrap recursively: a Dict's QVariantMap may itself hold Dict /
        // Variant values (e.g. NetworkManager connection dicts a{sa{sv}}).
        QVariantMap m = v.value<DBus::Dict>().value;
        for (auto it = m.begin(); it != m.end(); ++it)
            it.value() = toDbusVariantImpl(it.value(), true);
        return QVariant::fromValue(m);
    }
    if (type == qMetaTypeId<DBus::Variant>()) {
        const DBus::Variant var = v.value<DBus::Variant>();
        // Optional payload signature drives the payload through marshalBySignature
        // (which loud-fails and falls back to inference for unproducible shapes);
        // empty signature keeps inference. The payload is itself a variant-free
        // slot, so recurse as free.
        const QVariant payload = var.sig.isEmpty() ? toDbusVariantImpl(var.propValue(), false)
                                                   : marshalBySignature(var.sig, var.propValue());
        if (nested)
            return payload; // the enclosing container provides the "v"
        return QVariant::fromValue(QDBusVariant(payload));
    }

    return v;
}

QVariant toDbusVariant(const QVariant &v) {
    return toDbusVariantImpl(v, false);
}

QVariant toDbusVariantNested(const QVariant &v) {
    return toDbusVariantImpl(v, true);
}

// ==================== Signature-driven marshaller ====================

// Parse one complete D-Bus type from `sig` starting at `pos`.
// Returns the type's signature substring and advances pos past it.
// Returns empty on parse failure. CF-3: the 'a' branch recursed
// uncapped — peer introspection XML flows verbatim into this walker,
// so a 200k-deep "a…a" crashed the process (SIGSEGV, fork-VM-proven).
// Depth-capped at 32 like every other walker (loud-fail, same message
// shape); validateSignature (the full spec validator) is the second gate
// at slot-mint time, and writeBySignature re-checks validity per
// the PARITY §2 plan note.
QString firstCompleteType(const QString &sig, int &pos, int depth) {
    if (pos >= sig.size())
        return {};
    if (depth > 32) {
        // Same message shape as the other walkers (B1/readBySignature,
        // writeValueBySignature) so one ignoreMessage pattern covers all.
        qWarning("dbusqml: recursion depth cap (32) exceeded — failing loud");
        return {};
    }
    int start = pos;
    QChar c = sig.at(pos);

    // Basic single-char types
    if (QStringLiteral("ybnqiuxtdhsogv").contains(c)) {
        ++pos;
        return sig.mid(start, 1);
    }

    if (c == QLatin1Char('a')) {
        // Array — 'a' followed by one complete type
        ++pos;
        QString elem = firstCompleteType(sig, pos, depth + 1);
        if (elem.isEmpty())
            return {};
        // Dict entry shorthand: a{KV} — the {KV} is one element type
        return sig.mid(start, pos - start);
    }

    if (c == QLatin1Char('(')) {
        // Struct — (...) with member types
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('('))
                ++depth;
            else if (sig.at(pos) == QLatin1Char(')'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }

    if (c == QLatin1Char('{')) {
        // Dict entry — {KV} (only valid inside an array, but parse it anyway)
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('{'))
                ++depth;
            else if (sig.at(pos) == QLatin1Char('}'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }

    return {};
}

// Signature-slot pool state (F1/B9 — see dbussignatureslots.h for the four
// invariants). File-static like the walkers: one instance per process, shared
// by every connection in it. Assignment takes the mutex for lookup +
// registerCustomType as a single critical section and releases it before
// returning — never held while streaming, so nested assignment re-takes it
// without deadlock. Qt's own registry lock nests inside ours; Qt never calls
// back into us under its lock (no marshall operators are ever registered),
// so the ordering is deadlock-free.
namespace {
template <int... Is>
std::array<QMetaType, sizeof...(Is)> makeSignatureSlotTypes(std::integer_sequence<int, Is...>) {
    std::array<QMetaType, sizeof...(Is)> types{};
    ((types[Is] = QMetaType(qRegisterMetaType<DbusSignatureSlot<Is>>())), ...);
    return types;
}
const std::array<QMetaType, SignatureSlotPoolSize + 1> &signatureSlotTypes() {
    static const auto types =
        makeSignatureSlotTypes(std::make_integer_sequence<int, SignatureSlotPoolSize + 1>());
    return types;
}
} // namespace

// D-Bus signature validation (spec, "Valid Signatures"; libdbus
// _dbus_validate_signature_with_reason). Recursive descent over one
// complete type with PER-KIND nesting heights — array and struct nesting
// are capped separately at DBUS_MAXIMUM_TYPE_RECURSION_DEPTH (32), like
// the decode side's DecodeDepth (dbusutils.cpp): a dict entry nests like
// a struct, so '{' counts as struct height. Replaces isStrictSignature,
// which missed the 255-byte length cap, the dict-entry placement and
// key-basicness rules, and the per-kind depth split (findings #1/#4).
namespace {
constexpr int kMaxSignatureLength = 255; // DBUS_MAXIMUM_SIGNATURE_LENGTH
constexpr int kMaxSignatureDepth = 32;   // DBUS_MAXIMUM_TYPE_RECURSION_DEPTH

bool validateTypeAt(const QString &sig, int &pos, int arrays, int structs) {
    if (pos >= sig.size() || arrays > kMaxSignatureDepth || structs > kMaxSignatureDepth)
        return false;
    const QChar c = sig.at(pos++);
    if (QStringLiteral("ybnqiuxtdhsogv").contains(c))
        return true;
    if (c == QLatin1Char('a')) {
        if (pos < sig.size() && sig.at(pos) == QLatin1Char('{')) {
            // Dict entry: valid only as an array's element type, with
            // exactly two members, the key a BASIC type (a variant is a
            // container, not a basic type).
            ++pos;
            if (pos >= sig.size() || !QStringLiteral("ybnqiuxtdhsog").contains(sig.at(pos)))
                return false;
            ++pos;
            if (!validateTypeAt(sig, pos, arrays + 1, structs + 1))
                return false;
            if (pos >= sig.size() || sig.at(pos) != QLatin1Char('}'))
                return false;
            ++pos;
            return true;
        }
        return validateTypeAt(sig, pos, arrays + 1, structs);
    }
    if (c == QLatin1Char('(')) {
        // "()" is not a valid D-Bus type — libdbus aborts on it.
        bool any = false;
        while (pos < sig.size() && sig.at(pos) != QLatin1Char(')')) {
            if (!validateTypeAt(sig, pos, arrays, structs + 1))
                return false;
            any = true;
        }
        if (!any || pos >= sig.size())
            return false;
        ++pos; // ')'
        return true;
    }
    return false;
}
} // namespace

bool validateSignature(const QString &sig, SignatureKind kind) {
    // The wire cap applies to the whole signature string (message
    // signature or standalone single type alike) — libdbus rejects longer
    // ones, and its container-open check aborts the process.
    if (sig.toUtf8().size() > kMaxSignatureLength)
        return false;
    int pos = 0;
    if (kind == SignatureKind::Single)
        return validateTypeAt(sig, pos, 0, 0) && pos == sig.size();
    while (pos < sig.size()) {
        if (!validateTypeAt(sig, pos, 0, 0))
            return false;
    }
    return true;
}

QMetaType signatureSlotForSignature(const QString &sig) {
    // Full spec validation BEFORE touching Qt's registry: a bad header
    // aborts inside libdbus — and element signatures can arrive from a
    // peer's live introspection XML, so garbage here is remotely
    // triggerable (finding #1: a 256-byte element signature registered
    // here and aborted the process at beginArray). Loud-fail (invalid
    // metatype) on anything the validator rejects.
    if (!validateSignature(sig, SignatureKind::Single))
        return QMetaType();
    static QMutex mutex;
    static QHash<QString, int> assigned;
    static int nextFree = 0;
    QMutexLocker locker(&mutex);
    auto it = assigned.constFind(sig);
    if (it != assigned.cend())
        return signatureSlotTypes().at(it.value());
    if (nextFree >= SignatureSlotPoolSize) {
        qWarning("dbusqml: signature-slot pool exhausted (%d distinct element signatures) — "
                 "failing loud",
                 SignatureSlotPoolSize);
        return QMetaType();
    }
    const int index = nextFree++;
    assigned.insert(sig, index);
    // Assign-once: this id is never registered again (invariant 1). The
    // signature is live in the registry before the metatype escapes
    // (invariant 2).
    QDBusMetaType::registerCustomType(signatureSlotTypes().at(index), sig.toUtf8());
    return signatureSlotTypes().at(index);
}

// Map a D-Bus signature to the QMetaType used for beginArray/beginMap
// element arguments. QDBusMetaType::signatureToMetaType() only covers basic
// types on Qt 6.11 (container signatures return an invalid QMetaType even
// after registerCustomType), so container shapes are mapped explicitly — and
// anything left over mints a signature-slot pool assignment (F1/B9), so every
// well-formed element signature is producible.
QMetaType metaTypeForSignature(const QString &sig) {
    QMetaType mt = QDBusMetaType::signatureToMetaType(sig.toUtf8().constData());
    if (mt.isValid())
        return mt;
    if (sig == QLatin1String("a{sv}"))
        return QMetaType::fromType<QVariantMap>();
    if (sig == QLatin1String("a{sa{sv}}"))
        return QMetaType::fromType<QMap<QString, QVariantMap>>();
    if (sig == QLatin1String("aa{sv}"))
        return QMetaType::fromType<QList<QVariantMap>>();
    if (sig == QLatin1String("h"))
        return QMetaType::fromType<QDBusUnixFileDescriptor>();
    return signatureSlotForSignature(sig);
}

// Forward declaration — mutual recursion between writeProbe/appendStaged
// and metaTypeForSignature (CF-9 staging helpers below).
QMetaType metaTypeForSignature(const QString &sig);

// CF-9: close-on-fail staging helpers. A bare `return false` between
// beginMap/beginArray and endMap/endArray leaves the QDBusArgument
// half-open (the L2 leak class LSan proved for structs). Container
// branches therefore PROBE first — writeProbe is pure (never touches
// `arg`) and returns the elements in final streaming form, or an
// invalid QVariant when unproducible — and only begin the container
// when every element probed valid. appendStaged then streams
// pre-validated input and cannot fail. Basic-type probes mirror the
// streaming expressions in writeValueBySignature exactly.
// The one fd send rule (finding #4): an `h` position carries only an explicit
// fd — a DBusQML.unixFd (toDbusVariant gives the message its own dup) or a
// C++ QDBusUnixFileDescriptor. A number is refused at every `h` site, whatever
// declared the `h`; `fdError` receives the first reason.
static bool wireFd(const QVariant &v, QDBusUnixFileDescriptor *out, QString *fdError) {
    QString why;
    if (v.userType() == qMetaTypeId<QDBusUnixFileDescriptor>()) {
        *out = v.value<QDBusUnixFileDescriptor>();
        if (out->isValid())
            return true;
        why = QStringLiteral("an invalid fd cannot be sent");
    } else if (v.userType() == qMetaTypeId<DBus::UnixFd>()) {
        const QVariant wire = toDbusVariant(v); // the one unixFd → wire conversion
        return wireFd(wire, out, fdError);
    } else if (v.userType() == qMetaTypeId<DBusFdFailure>()) {
        why = v.value<DBusFdFailure>().reason;
    } else {
        why = QStringLiteral("an fd (h) value must be a DBusQML.unixFd, got %1")
                  .arg(QString::fromLatin1(v.isValid() ? v.metaType().name() : "undefined"));
    }
    if (fdError && fdError->isEmpty())
        *fdError = why;
    return false;
}

static QVariant writeProbe(const QString &sig, const QVariant &value, int depth,
                           QString *fdError = nullptr) {
    if (depth > 32)
        return {};
    const QVariant v = toDbusVariant(value);
    if (sig == QLatin1String("y"))
        return QVariant::fromValue(static_cast<uchar>(v.toUInt()));
    if (sig == QLatin1String("b"))
        return QVariant::fromValue(v.toBool());
    if (sig == QLatin1String("n"))
        return QVariant::fromValue(static_cast<short>(v.toInt()));
    if (sig == QLatin1String("q"))
        return QVariant::fromValue(static_cast<ushort>(v.toUInt()));
    if (sig == QLatin1String("i"))
        return QVariant::fromValue(v.toInt());
    if (sig == QLatin1String("u"))
        return QVariant::fromValue(v.toUInt());
    if (sig == QLatin1String("x"))
        return QVariant::fromValue(static_cast<qint64>(v.toLongLong()));
    if (sig == QLatin1String("t"))
        return QVariant::fromValue(static_cast<quint64>(v.toULongLong()));
    if (sig == QLatin1String("d"))
        return QVariant::fromValue(v.toDouble());
    if (sig == QLatin1String("s"))
        return QVariant::fromValue(v.toString());
    if (sig == QLatin1String("o"))
        return QVariant::fromValue(QDBusObjectPath(v.toString()));
    if (sig == QLatin1String("g"))
        return QVariant::fromValue(QDBusSignature(v.toString()));
    if (sig == QLatin1String("h")) {
        QDBusUnixFileDescriptor fd;
        if (!wireFd(v, &fd, fdError))
            return {};
        return QVariant::fromValue(fd);
    }
    if (sig == QLatin1String("v")) {
        if (v.userType() == qMetaTypeId<QDBusVariant>())
            return v;
        return QVariant::fromValue(QDBusVariant(v));
    }
    if (sig == QLatin1String("ay")) {
        if (v.userType() == QMetaType::QString)
            return QVariant::fromValue(v.toString().toUtf8());
        if (v.userType() == qMetaTypeId<QVariantList>()) {
            QByteArray bytes;
            const QVariantList list = v.toList();
            bytes.reserve(list.size());
            for (const QVariant &b : list)
                bytes.append(static_cast<char>(b.toInt()));
            return QVariant::fromValue(bytes);
        }
        return QVariant::fromValue(v.toByteArray());
    }
    if (sig == QLatin1String("as"))
        return QVariant::fromValue(v.toStringList());
    if (sig.startsWith(QLatin1Char('('))) {
        const QString inner = sig.mid(1, sig.size() - 2);
        if (inner.isEmpty())
            return {};
        const QVariantList members = v.toList();
        QVariantList staged;
        int pos = 0;
        int mi = 0;
        while (pos < inner.size()) {
            const QString memberSig = firstCompleteType(inner, pos);
            if (memberSig.isEmpty())
                return {};
            const QVariant mv = mi < members.size() ? members.at(mi) : QVariant();
            const QVariant p = writeProbe(memberSig, mv, depth + 1, fdError);
            if (!p.isValid())
                return {};
            staged << p;
            ++mi;
        }
        return QVariant::fromValue(staged);
    }
    if (sig.startsWith(QLatin1Char('a'))) {
        const QString elemSig = sig.mid(1);
        if (elemSig.startsWith(QLatin1Char('{'))) {
            int pos = 1;
            const QString keySig = firstCompleteType(elemSig, pos);
            const QString valSig = firstCompleteType(elemSig, pos);
            if (keySig.isEmpty() || valSig.isEmpty())
                return {};
            if (!metaTypeForSignature(keySig).isValid() || !metaTypeForSignature(valSig).isValid())
                return {};
            const QVariantMap map = v.toMap();
            QVariantList staged;
            for (auto it = map.begin(); it != map.end(); ++it) {
                const QVariant kp = writeProbe(keySig, QVariant(it.key()), depth + 1, fdError);
                const QVariant vp = writeProbe(valSig, it.value(), depth + 1, fdError);
                if (!kp.isValid() || !vp.isValid())
                    return {};
                staged << QVariant::fromValue(QVariantList{kp, vp});
            }
            return QVariant::fromValue(staged);
        }
        if (!metaTypeForSignature(elemSig).isValid())
            return {};
        const QVariantList list = v.toList();
        QVariantList staged;
        for (const QVariant &e : list) {
            const QVariant p = writeProbe(elemSig, e, depth + 1, fdError);
            if (!p.isValid())
                return {};
            staged << p;
        }
        return QVariant::fromValue(staged);
    }
    return {};
}

static void appendStaged(QDBusArgument &arg, const QString &sig, const QVariant &staged) {
    if (sig == QLatin1String("y")) {
        arg << static_cast<uchar>(staged.toUInt());
        return;
    }
    if (sig == QLatin1String("b")) {
        arg << staged.toBool();
        return;
    }
    if (sig == QLatin1String("n")) {
        arg << static_cast<short>(staged.toInt());
        return;
    }
    if (sig == QLatin1String("q")) {
        arg << static_cast<ushort>(staged.toUInt());
        return;
    }
    if (sig == QLatin1String("i")) {
        arg << staged.toInt();
        return;
    }
    if (sig == QLatin1String("u")) {
        arg << staged.toUInt();
        return;
    }
    if (sig == QLatin1String("x")) {
        arg << static_cast<qint64>(staged.toLongLong());
        return;
    }
    if (sig == QLatin1String("t")) {
        arg << static_cast<quint64>(staged.toULongLong());
        return;
    }
    if (sig == QLatin1String("d")) {
        arg << staged.toDouble();
        return;
    }
    if (sig == QLatin1String("s")) {
        arg << staged.toString();
        return;
    }
    if (sig == QLatin1String("o")) {
        arg << QDBusObjectPath(staged.toString());
        return;
    }
    if (sig == QLatin1String("g")) {
        arg << QDBusSignature(staged.toString());
        return;
    }
    if (sig == QLatin1String("h")) {
        arg << staged.value<QDBusUnixFileDescriptor>(); // wireFd-checked by the probe
        return;
    }
    if (sig == QLatin1String("v")) {
        arg << staged.value<QDBusVariant>();
        return;
    }
    if (sig == QLatin1String("ay")) {
        arg << staged.toByteArray();
        return;
    }
    if (sig == QLatin1String("as")) {
        arg << staged.toStringList();
        return;
    }
    if (sig.startsWith(QLatin1Char('('))) {
        const QString inner = sig.mid(1, sig.size() - 2);
        const QVariantList members = staged.toList();
        int pos = 0;
        int mi = 0;
        arg.beginStructure();
        while (pos < inner.size()) {
            const QString memberSig = firstCompleteType(inner, pos);
            appendStaged(arg, memberSig, members.at(mi));
            ++mi;
        }
        arg.endStructure();
        return;
    }
    if (sig.startsWith(QLatin1Char('a'))) {
        const QString elemSig = sig.mid(1);
        if (elemSig.startsWith(QLatin1Char('{'))) {
            int pos = 1;
            const QString keySig = firstCompleteType(elemSig, pos);
            const QString valSig = firstCompleteType(elemSig, pos);
            arg.beginMap(metaTypeForSignature(keySig), metaTypeForSignature(valSig));
            const QVariantList staged_entries = staged.toList();
            for (const QVariant &e : staged_entries) {
                const QVariantList kv = e.toList();
                arg.beginMapEntry();
                appendStaged(arg, keySig, kv.at(0));
                appendStaged(arg, valSig, kv.at(1));
                arg.endMapEntry();
            }
            arg.endMap();
            return;
        }
        arg.beginArray(metaTypeForSignature(elemSig));
        const QVariantList staged_elems = staged.toList();
        for (const QVariant &e : staged_elems)
            appendStaged(arg, elemSig, e);
        arg.endArray();
        return;
    }
}

// Recursive signature walker: append `value` marshaled as `sig` into a
// writable QDBusArgument — the write-side mirror of readBySignature.
// Returns false (loud, at the caller) for malformed signatures, invalid
// values, and past the depth cap. Every well-formed element signature is
// producible: unregistered container shapes mint signature-slot pool
// assignments (F1/B9) instead of failing.
static bool writeValueBySignature(QDBusArgument &arg, const QString &sig, const QVariant &value,
                                  int depth = 0, QString *fdError = nullptr) {
    if (depth > 32) {
        // Same message shape as the other walkers (B1 plan note).
        qWarning("dbusqml: recursion depth cap (32) exceeded — failing loud");
        return false;
    }
    // Unwrap DBus.* gadgets nested in the value before walking. toDbusVariant
    // recurses QVariantMap and DBus::Struct payloads; list/array elements are
    // unwrapped as we recurse per element below.
    const QVariant v = toDbusVariant(value);

    if (sig == QLatin1String("y")) {
        arg << static_cast<uchar>(v.toUInt());
        return true;
    }
    if (sig == QLatin1String("b")) {
        arg << v.toBool();
        return true;
    }
    if (sig == QLatin1String("n")) {
        arg << static_cast<short>(v.toInt());
        return true;
    }
    if (sig == QLatin1String("q")) {
        arg << static_cast<ushort>(v.toUInt());
        return true;
    }
    if (sig == QLatin1String("i")) {
        arg << v.toInt();
        return true;
    }
    if (sig == QLatin1String("u")) {
        arg << v.toUInt();
        return true;
    }
    if (sig == QLatin1String("x")) {
        arg << static_cast<qint64>(v.toLongLong());
        return true;
    }
    if (sig == QLatin1String("h")) {
        // Unix fd: only an explicit fd value (wireFd); the sender's unixFd
        // stays open — the message carries its own dup.
        QDBusUnixFileDescriptor fd;
        if (!wireFd(v, &fd, fdError))
            return false;
        arg << fd;
        return true;
    }
    if (sig == QLatin1String("t")) {
        arg << static_cast<quint64>(v.toULongLong());
        return true;
    }
    if (sig == QLatin1String("d")) {
        arg << v.toDouble();
        return true;
    }
    if (sig == QLatin1String("s")) {
        arg << v.toString();
        return true;
    }
    if (sig == QLatin1String("o")) {
        arg << QDBusObjectPath(v.toString());
        return true;
    }
    if (sig == QLatin1String("g")) {
        arg << QDBusSignature(v.toString());
        return true;
    }
    if (sig == QLatin1String("v")) {
        if (v.userType() == qMetaTypeId<QDBusVariant>())
            arg << v.value<QDBusVariant>();
        else
            arg << QDBusVariant(v);
        return true;
    }
    if (sig == QLatin1String("ay")) {
        // B3: mirror W4's coercion — a number array (QVariantList of ints)
        // converts per-element; a bare toByteArray() on a list silently
        // produced EMPTY bytes at nested positions.
        if (v.userType() == QMetaType::QString)
            arg << v.toString().toUtf8();
        else if (v.userType() == qMetaTypeId<QVariantList>()) {
            QByteArray bytes;
            const QVariantList list = v.toList();
            bytes.reserve(list.size());
            for (const QVariant &b : list)
                bytes.append(static_cast<char>(b.toInt()));
            arg << bytes;
        } else
            arg << v.toByteArray();
        return true;
    }
    if (sig == QLatin1String("as")) {
        arg << v.toStringList();
        return true;
    }

    if (sig.startsWith(QLatin1Char('('))) {
        const QString inner = sig.mid(1, sig.size() - 2);
        // "()" / "(": an empty struct is not a valid D-Bus type — writing one
        // aborts inside libdbus. Loud-fail instead.
        if (inner.isEmpty())
            return false;
        // L2 (ledger-zero, LSan-proven): the old code wrote members
        // directly into `arg` after beginStructure() — any member failure
        // (`return false`) skipped endStructure(), leaving the container
        // open and leaking the half-built libdbus message inside QtDBus
        // (152B beginStructure leak, fork-VM-proven). Validate-then-write
        // per member instead: each member is attempted, and on failure
        // the (complete, balanced) prefix is still closed — `arg` is
        // never left half-open. A failed member still fails the call
        // loud (the caller sends InvalidArgs on false).
        arg.beginStructure();
        const QVariantList members = v.toList();
        int pos = 0;
        int mi = 0;
        bool ok = true;
        while (pos < inner.size()) {
            const QString memberSig = firstCompleteType(inner, pos);
            if (memberSig.isEmpty()) {
                ok = false;
                break;
            }
            const QVariant mv = mi < members.size() ? members.at(mi) : QVariant();
            if (!writeValueBySignature(arg, memberSig, mv, depth + 1, fdError)) {
                ok = false;
                break;
            }
            ++mi;
        }
        arg.endStructure();
        return ok;
    }

    if (sig.startsWith(QLatin1Char('a'))) {
        const QString elemSig = sig.mid(1);
        if (elemSig.startsWith(QLatin1Char('{'))) {
            int pos = 1;
            const QString keySig = firstCompleteType(elemSig, pos);
            const QString valSig = firstCompleteType(elemSig, pos);
            if (keySig.isEmpty() || valSig.isEmpty())
                return false;
            const QMetaType kMt = metaTypeForSignature(keySig);
            const QMetaType vMt = metaTypeForSignature(valSig);
            if (!kMt.isValid() || !vMt.isValid())
                return false;
            // CF-9: close-on-fail like the struct branch above — a bare
            // `return false` between beginMap and endMap leaves the
            // QDBusArgument half-open (same L2 leak class LSan proved for
            // structs). Validate-then-commit per entry instead: element
            // failures accumulate into `ok` and the complete entries are
            // still closed — `arg` is never left half-open.
            const QVariantMap map = v.toMap();
            QVector<QPair<QVariant, QVariant>> staged;
            staged.reserve(map.size());
            bool ok = true;
            for (auto it = map.begin(); it != map.end(); ++it) {
                const QVariant kv = writeProbe(keySig, QVariant(it.key()), depth + 1, fdError);
                const QVariant vv = writeProbe(valSig, it.value(), depth + 1, fdError);
                if (!kv.isValid() || !vv.isValid()) {
                    ok = false;
                    break;
                }
                staged.append({kv, vv});
            }
            if (!ok)
                return false;
            arg.beginMap(kMt, vMt);
            for (const auto &e : staged) {
                arg.beginMapEntry();
                appendStaged(arg, keySig, e.first);
                appendStaged(arg, valSig, e.second);
                arg.endMapEntry();
            }
            arg.endMap();
            return true;
        }
        const QMetaType eMt = metaTypeForSignature(elemSig);
        if (!eMt.isValid())
            return false;
        // CF-9: same close-on-fail discipline for arrays — stage every
        // element first; only beginArray when all are producible.
        QVector<QVariant> staged;
        {
            const QVariantList list = v.toList();
            staged.reserve(list.size());
            for (const QVariant &e : list) {
                const QVariant se = writeProbe(elemSig, e, depth + 1, fdError);
                if (!se.isValid())
                    return false;
                staged.append(se);
            }
        }
        arg.beginArray(eMt);
        for (const QVariant &se : staged)
            appendStaged(arg, elemSig, se);
        arg.endArray();
        return true;
    }
    return false;
}

// Forward declaration — mutual recursion between marshalBySignature and
// marshalContainerBySignature.
static QVariant marshalContainerBySignature(const QString &sig, const QVariant &value);

// Marshal a JS-supplied QVariant against a known D-Bus signature.
// Produces a QVariant with the correct C++ type for QtDBus to marshal
// to the wire format matching `sig`. Falls back to toDbusVariant for
// inference when the signature is empty, "v", or unrecognized.
QVariant marshalBySignature(const QString &sig, const QVariant &value) {
    // CF-4: a declared top-level `v` wraps the inferred value in a real
    // QDBusVariant (unless it already is one) — the XML advertises `v`
    // while a bare toDbusVariant return would marshal as the value's own
    // type. The old early-return (`sig=="v"` → toDbusVariant) made the
    // correct wrap below unreachable. Nested-`v` behavior (writer
    // :859-865) is unchanged.
    if (sig == QLatin1String("v")) {
        const QVariant inferred = toDbusVariant(value);
        if (inferred.userType() == qMetaTypeId<QDBusVariant>())
            return inferred;
        return QVariant::fromValue(QDBusVariant(inferred));
    }
    if (sig.isEmpty())
        return toDbusVariant(value);

    // Explicit DBus.* wrapper types always win — the caller chose the type.
    // D4: the short-circuit applies ONLY when the value's own wire signature
    // EQUALS the declared one; otherwise fall through to the declared
    // coercion. Position-independent: declared `u` + Int32 coerces to u at
    // every position, while declared `i` + Int32 passes through untouched.
    if (value.userType() != qMetaTypeId<QVariantList>() &&
        value.userType() != qMetaTypeId<QVariantMap>() && value.userType() != QMetaType::QString &&
        value.userType() != QMetaType::Bool && value.userType() != QMetaType::Int &&
        value.userType() != QMetaType::Double && value.userType() != QMetaType::UInt &&
        value.userType() != QMetaType::LongLong && value.userType() != QMetaType::ULongLong &&
        value.userType() != QMetaType::QByteArray) {
        const QVariant converted = toDbusVariant(value);
        const char *own = QDBusMetaType::typeToSignature(QMetaType(converted.userType()));
        if (own && sig == QLatin1String(own))
            return converted;
    }

    // Basic types — coerce the QVariant to the exact C++ type.
    if (sig == QLatin1String("y"))
        return QVariant::fromValue(static_cast<uchar>(value.toUInt()));
    if (sig == QLatin1String("b"))
        return QVariant::fromValue(value.toBool());
    if (sig == QLatin1String("n"))
        return QVariant::fromValue(static_cast<short>(value.toInt()));
    if (sig == QLatin1String("q"))
        return QVariant::fromValue(static_cast<ushort>(value.toUInt()));
    if (sig == QLatin1String("i"))
        return QVariant::fromValue(value.toInt());
    if (sig == QLatin1String("u"))
        return QVariant::fromValue(value.toUInt());
    if (sig == QLatin1String("h")) {
        // Unix fd: only an explicit fd value (wireFd) — a number is never
        // turned into a descriptor (finding #4). B4: anything else fails
        // LOUD at the send boundary (DBusFdFailure carries the reason)
        // instead of carrying a dead fd that leaves the caller timing out.
        QDBusUnixFileDescriptor fd;
        QString why;
        if (wireFd(toDbusVariant(value), &fd, &why))
            return QVariant::fromValue(fd);
        return QVariant::fromValue(DBusFdFailure{why});
    }
    if (sig == QLatin1String("x"))
        return QVariant::fromValue(static_cast<qint64>(value.toLongLong()));
    if (sig == QLatin1String("t"))
        return QVariant::fromValue(static_cast<quint64>(value.toULongLong()));
    if (sig == QLatin1String("d"))
        return QVariant::fromValue(value.toDouble());
    if (sig == QLatin1String("s"))
        return QVariant::fromValue(value.toString());
    if (sig == QLatin1String("o"))
        return QVariant::fromValue(QDBusObjectPath(value.toString()));
    if (sig == QLatin1String("g"))
        return QVariant::fromValue(QDBusSignature(value.toString()));

    // Byte array — ay. Accept string (UTF-8), number array, or QByteArray.
    if (sig == QLatin1String("ay")) {
        if (value.userType() == QMetaType::QByteArray)
            return value;
        if (value.userType() == QMetaType::QString)
            return QVariant::fromValue(value.toString().toUtf8());
        if (value.userType() == qMetaTypeId<QVariantList>()) {
            QByteArray bytes;
            const QVariantList list = value.toList();
            bytes.reserve(list.size());
            for (const QVariant &b : list)
                bytes.append(static_cast<char>(b.toInt()));
            return QVariant::fromValue(bytes);
        }
        // Fallback: try string conversion
        return QVariant::fromValue(value.toString().toUtf8());
    }

    // String array — as. Use DBusAsArray to force the correct marshaling
    // (QStringList alone may marshal as av).
    if (sig == QLatin1String("as")) {
        DBusAsArray arr;
        const QVariantList list = value.toList();
        for (const QVariant &item : list)
            arr.value << item.toString();
        return QVariant::fromValue(arr);
    }

    // Variant — nested positions wrap after unwrapping any DBus.* types.
    // (Top-level `v` is handled at the function head — CF-4.)
    if (sig == QLatin1String("v"))
        return QVariant::fromValue(QDBusVariant(toDbusVariant(value)));

    // Container types — delegate to the recursive container marshaller.
    if (sig.startsWith(QLatin1Char('a')) || sig.startsWith(QLatin1Char('(')) ||
        sig.startsWith(QLatin1Char('{')))
        return marshalContainerBySignature(sig, value);

    // Unrecognized — fall back to inference. B10: LOUD (the container twin
    // already warns) — a declared signature is never silently ignored.
    qWarning("dbusqml: marshalBySignature: unrecognized signature '%s' — falling back to "
             "inference",
             qPrintable(sig));
    return toDbusVariant(value);
}

// Container marshaling — handles a{...}, a<complex>, (...), etc.
// Dicts use known container types; any other container shape walks the
// generic signature-driven writer below (unregistered element signatures
// mint signature-slot pool assignments — F1/B9).
static QVariant marshalContainerBySignature(const QString &sig, const QVariant &value) {
    // a{sv} — dict with string keys and variant values.
    // QVariantMap is exactly a{sv} in QtDBus.
    if (sig == QLatin1String("a{sv}")) {
        QVariantMap map = value.toMap();
        // Recurse into values — nested Dict/Variant payloads must be unwrapped.
        for (auto it = map.begin(); it != map.end(); ++it)
            it.value() = toDbusVariant(it.value());
        return QVariant::fromValue(map);
    }

    // a{sa{sv}} — dict of dicts. NM connection settings shape.
    // Needs QMap<QString,QVariantMap> — registered in dbusplugin.cpp.
    if (sig == QLatin1String("a{sa{sv}}")) {
        QVariantMap outer = value.toMap();
        QMap<QString, QVariantMap> typed;
        for (auto it = outer.begin(); it != outer.end(); ++it) {
            QVariantMap inner = it.value().toMap();
            for (auto jt = inner.begin(); jt != inner.end(); ++jt)
                jt.value() = toDbusVariant(jt.value());
            typed.insert(it.key(), inner);
        }
        return QVariant::fromValue(typed);
    }

    // a(sss...) — array of structs with homogeneous members, and the general
    // path for any other container: signature-walking writer. Produces a
    // QDBusArgument-wrapped QVariant cross-marshaled by QtDBus. Unproducible
    // inputs (malformed signatures, invalid values) fail loudly and fall back
    // to inference — a declared signature is never silently ignored.
    {
        // For now, handle aay (array of byte arrays) explicitly.
        if (sig == QLatin1String("aay")) {
            QList<QByteArray> list;
            const QVariantList items = value.toList();
            for (const QVariant &item : items) {
                if (item.userType() == QMetaType::QByteArray)
                    list << item.toByteArray();
                else if (item.userType() == QMetaType::QString)
                    list << item.toString().toUtf8();
                else
                    list << item.toByteArray();
            }
            return QVariant::fromValue(list);
        }

        // Generic: signature-walking writer. Produces a QDBusArgument-wrapped
        // QVariant cross-marshaled by QtDBus for any producible signature.
        // Unproducible shapes fail loudly and fall back to inference — a
        // declared signature is never silently ignored.
        QString fdError;
        QVariant walked = writeBySignature(sig, value, &fdError);
        if (walked.isValid())
            return walked;
        // An `h` position refused its value: never fall back to inference
        // (that would send the number as a non-fd type) — the send boundary
        // fails the call with the reason.
        if (!fdError.isEmpty())
            return QVariant::fromValue(DBusFdFailure{fdError});
        qWarning("dbusqml: cannot produce declared signature %s for value of type %s — "
                 "falling back to inference",
                 qPrintable(sig), QMetaType(value.userType()).name());
        return toDbusVariant(value);
    }
}

QVariant writeBySignature(const QString &sig, const QVariant &value, QString *fdError) {
    // Reject invalid signatures BEFORE building anything: an unbalanced,
    // over-long, or over-deep signature produces a QDBusArgument whose
    // contained signature libdbus rejects with an assertion abort
    // (remotely triggerable via variant(x, sig) or a peer's introspected
    // in-arg types — the 0.5.2-era crash class on the reply path, and
    // finding #1's peer-XML shape). Loud-fail to the caller instead.
    // The writer's own depth-32 walker cap still applies after this gate.
    if (sig.isEmpty())
        return QVariant();
    if (!validateSignature(sig, SignatureKind::Single))
        return QVariant();
    QDBusArgument arg;
    if (!writeValueBySignature(arg, sig, value, 0, fdError))
        return QVariant();
    return QVariant::fromValue(arg);
}

static QDBusMessage toQDBusMessage(const DBusMessage &msg) {
    auto qmsg =
        QDBusMessage::createMethodCall(msg.service(), msg.path(), msg.iface(), msg.member());

    if (!msg.arguments().isEmpty()) {
        QVariantList args = msg.arguments();

        // If the message carries an explicit signature, use it to drive
        // per-argument marshaling. The signature is a concatenation of
        // per-arg complete types, e.g. "sa{sa{sv}}ay" for three args.
        if (!msg.signature().isEmpty()) {
            QString sig = msg.signature();
            int pos = 0;
            for (int i = 0; i < args.size() && pos < sig.size(); ++i) {
                QString argSig = firstCompleteType(sig, pos);
                if (argSig.isEmpty()) {
                    // B10: a signature that breaks mid-parse is LOUD — the
                    // remaining args fall back to inference visibly.
                    qWarning("dbusqml: message signature '%s' breaks mid-parse at arg %d — "
                             "remaining args fall back to inference",
                             qPrintable(sig), i);
                    break;
                }
                args[i] = marshalBySignature(argSig, args[i]);
            }
        } else {
            for (int i = 0; i < args.size(); ++i)
                args[i] = toDbusVariant(args[i]);
        }
        qmsg.setArguments(args);
    }

    // Call options carried on the gadget (0.9.0).
    qmsg.setInteractiveAuthorizationAllowed(msg.interactiveAuthorization());
    qmsg.setAutoStartService(msg.autoStart());

    return qmsg;
}

DBusConnection::DBusConnection(const QDBusConnection &conn, const QString &name, QObject *parent)
    : QObject(parent), m_connection(conn), m_connectionName(name) {
    armLossProbe();
}

DBusConnection::~DBusConnection() {
    delete m_lossProbe;
}

void DBusConnection::armLossProbe() {
    // P5 loss detector (Nemo connection.cpp:89-109, QtDBus-adapted — see
    // the header note on Local.Disconnected): one daemon-facing
    // NameHasOwner ping per connection-moment. Answered by the daemon
    // itself, so it fails if and only if the connection is dead. The
    // completion re-arms through a single-shot gate (CF-5): the old code
    // re-armed synchronously inside its own completion, and a µs-fast
    // daemon answer turned one ping into a perpetual idle ping storm per
    // instance. No polling: at most one ping is ever in flight, and it
    // completes on its own as soon as the bus answers or dies.
    if (!m_connected || m_lossProbe || m_lossRearm)
        return;
    QDBusMessage ping = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
        QStringLiteral("org.freedesktop.DBus"), QStringLiteral("NameHasOwner"));
    ping.setArguments({m_connection.baseService()});
    QDBusPendingCall call = m_connection.asyncCall(ping);
    m_lossProbe = new QDBusPendingCallWatcher(call, this);
    connect(m_lossProbe, &QDBusPendingCallWatcher::finished, this,
            &DBusConnection::onPendingCallFinished);
}

void DBusConnection::onPendingCallFinished(QDBusPendingCallWatcher *w) {
    if (w != m_lossProbe) {
        w->deleteLater();
        return;
    }
    m_lossProbe = nullptr;
    QDBusPendingReply<bool> reply = *w;
    w->deleteLater();
    if (reply.isError() && reply.error().type() == QDBusError::Disconnected) {
        // The bus is dead. Flip once; no resubscribe/reconnect (FD5).
        if (!m_connected)
            return;
        m_connected = false;
        emit connectedChanged();
        emit disconnected();
        // Fan out to served claims on this connection: holders get
        // nameLost through the relay (main-thread delivery, T1
        // discipline).
        DBusPathDispatcher::handleConnectionLost(m_connection.name());
        return;
    }
    // Alive (or a non-fatal error — the daemon answered, which is itself
    // proof of life): re-arm for the next connection-moment through the
    // single-shot gate — never synchronously (CF-5 ping storm).
    m_lossRearm = true;
    QTimer::singleShot(kLossProbeRearmMs, this, [this] {
        m_lossRearm = false;
        armLossProbe();
    });
}

DBusConnection *DBusConnection::connectToBus(const QString &address) {
    // A fixed connection name causes QtDBus to return the FIRST connection
    // for every subsequent call, silently reusing it regardless of address.
    // Use a per-call counter so callers get distinct connections.
    static QAtomicInt counter{0};
    QString name = QStringLiteral("dbusqml-custom-%1").arg(counter.fetchAndAddOrdered(1) + 1);
    auto conn = QDBusConnection::connectToBus(address, name);
    if (!conn.isConnected())
        return nullptr;
    return new DBusConnection(conn, name);
}

DBusPendingReply *DBusConnection::asyncCall(const DBusMessage &message) {
    return startCall(message, /*gcHandoff=*/true);
}

DBusPendingReply *DBusConnection::startCall(const DBusMessage &message, bool gcHandoff) {
    auto qmsg = toQDBusMessage(message);
    // Client-exit guard: an argument with no wire representation — or an
    // aggregate message signature over the 255-byte cap — would abort
    // inside QtDBus/libdbus marshaling — the caller-side analogue of the
    // 0.5.2 property crash. Fail the call LOCALLY instead; never send
    // garbage.
    QString sendErr;
    if (!checkSendable(qmsg.arguments(), &sendErr)) {
        qWarning("dbusqml: %s in call to %s — failing call locally", qPrintable(sendErr),
                 qPrintable(message.member()));
        auto *fail = new DBusPendingReply(this);
        fail->setEngine(qmlEngine(this));
        fail->setGcHandoffEnabled(gcHandoff);
        fail->completeLocalError(QStringLiteral("org.freedesktop.DBus.Error.Failed"), sendErr);
        return fail;
    }
    // Per-call timeout: the gadget's timeout (ms) drives QtDBus's asyncCall
    // timeout (−1 = Qt default, 25 s).
    auto pending = m_connection.asyncCall(qmsg, message.timeout());
    auto watcher = new QDBusPendingCallWatcher(pending, this);
    auto reply = new DBusPendingReply(this);
    reply->setEngine(qmlEngine(this));
    reply->setGcHandoffEnabled(gcHandoff);
    reply->setWatcher(watcher);
    return reply;
}

void DBusConnection::send(const DBusMessage &message) {
    auto qmsg = toQDBusMessage(message);
    // Fire-and-forget: NO_REPLY_EXPECTED is implied by send(); the message
    // goes out and nothing comes back. The client-exit send boundary still
    // applies — a bad argument or an over-long aggregate signature warns
    // and drops (there is no reply channel to fail locally into).
    QString sendErr;
    if (!checkSendable(qmsg.arguments(), &sendErr)) {
        qWarning("dbusqml: %s in call to %s — dropping send", qPrintable(sendErr),
                 qPrintable(message.member()));
        return;
    }
    m_connection.send(qmsg);
}

void DBusConnection::asyncCall(const DBusMessage &message, const QJSValue &resolve,
                               const QJSValue &reject) {
    // Promise-style overload: (resolve, reject) callbacks.
    //   resolve is called with the reply value converted natively to a JS
    //     value (numbers, booleans, arrays, and dicts survive; nested D-Bus
    //     containers are unwrapped via unwrapDbus).
    //   reject is called with a single error object { name, message }.
    // R4: the promise path manages this reply C++-side (settled →
    // deleteLater below) and never returns it to JS — opted out of the
    // completion GC handoff before it can settle (unparenting it with no JS
    // wrapper would leak it). With or without callbacks, the reply is
    // deleted once it settles.
    auto reply = startCall(message, /*gcHandoff=*/false);
    QPointer<QQmlEngine> engine = qmlEngine(this);
    connect(reply, &DBusPendingReply::finished, this,
            [reply, resolve = QJSValue(resolve), reject = QJSValue(reject), engine]() mutable {
                // CF-11: the promise settled — the reply's job is done.
                // deleteLater (not delete: we are inside its signal).
                // The returned-pointer overload below keeps
                // caller-managed ownership (D1, owner-vetoable at V1).
                reply->deleteLater();
                // P6: user callbacks run in the JS world — a thrown
                // exception from one is a QJSValue result, not a crash, but
                // dropping it silently violates the loud contract.
                if (reply->isError()) {
                    if (reject.isCallable()) {
                        QJSValue errObj;
                        if (engine) {
                            errObj = engine->newObject();
                            errObj.setProperty(QStringLiteral("name"),
                                               QJSValue(reply->error().name()));
                            errObj.setProperty(QStringLiteral("message"),
                                               QJSValue(reply->error().message()));
                        } else {
                            errObj = QJSValue(reply->error().message());
                        }
                        QJSValue thrown = reject.call({errObj});
                        if (thrown.isError())
                            qWarning("dbusqml: asyncCall reject callback threw: %s",
                                     qPrintable(thrown.toString()));
                    }
                } else if (resolve.isCallable()) {
                    QJSValue val = engine ? variantToJs(engine.data(), reply->value())
                                          : QJSValue(reply->value().toString());
                    QJSValue thrown = resolve.call({val});
                    if (thrown.isError())
                        qWarning("dbusqml: asyncCall resolve callback threw: %s",
                                 qPrintable(thrown.toString()));
                }
            });
}

SessionBusConnection::SessionBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::sessionBus(), QString(), parent) {}

SystemBusConnection::SystemBusConnection(QObject *parent)
    : DBusConnection(QDBusConnection::systemBus(), QString(), parent) {}

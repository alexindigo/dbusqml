#pragma once

#include <QDBusObjectPath>
#include <QDBusSignature>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QExplicitlySharedDataPointer>
#include <QJSValue>
#include <QVariantMap>
#include <qqmlregistration.h>

class QDBusArgument;
class QJSEngine;

#define DBUS_QML_TYPE(TYPE, NAME, CTOR, STORE, SIG)                                                \
    class TYPE {                                                                                   \
        Q_GADGET                                                                                   \
        QML_VALUE_TYPE(NAME)                                                                       \
        QML_CONSTRUCTIBLE_VALUE                                                                    \
        Q_PROPERTY(STORE value MEMBER value)                                                       \
    public:                                                                                        \
        explicit TYPE() {}                                                                         \
        Q_INVOKABLE explicit TYPE(CTOR v) : value(v) {}                                            \
        operator STORE() const { return value; }                                                   \
        Q_INVOKABLE QString toString() const { return QVariant::fromValue(value).toString(); }     \
        operator QVariant() const { return QVariant::fromValue(*this); }                           \
        STORE value;                                                                               \
    };

namespace DBus {

DBUS_QML_TYPE(Uint32, uint32, uint, uint, "u")
DBUS_QML_TYPE(Int32, int32, int, int, "i")
DBUS_QML_TYPE(Uint16, uint16, uint, ushort, "q")
DBUS_QML_TYPE(Int16, int16, int, short, "n")
DBUS_QML_TYPE(Uint64, uint64, quint64, quint64, "t")
DBUS_QML_TYPE(Int64, int64, qint64, qint64, "x")
DBUS_QML_TYPE(Bool, boolean, bool, bool, "b")
DBUS_QML_TYPE(Double, double, double, double, "d")
DBUS_QML_TYPE(Byte, byte, uint, uchar, "y")
DBUS_QML_TYPE(String, string, QString, QString, "s")

class ObjectPath {
    Q_GADGET
    QML_VALUE_TYPE(objectPath)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(QDBusObjectPath value MEMBER value)
public:
    explicit ObjectPath() {}
    Q_INVOKABLE explicit ObjectPath(const QString &v) : value(v) {}
    operator QDBusObjectPath() const { return value; }
    Q_INVOKABLE QString toString() const { return value.path(); }
    operator QVariant() const { return QVariant::fromValue(*this); }
    QDBusObjectPath value;
};

class Signature {
    Q_GADGET
    QML_VALUE_TYPE(signature)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(QDBusSignature value MEMBER value)
public:
    explicit Signature() {}
    Q_INVOKABLE explicit Signature(const QString &v) : value(v) {}
    Q_INVOKABLE QString toString() const { return value.signature(); }
    operator QVariant() const { return QVariant::fromValue(*this); }
    QDBusSignature value;
};

class Dict {
    Q_GADGET
    QML_VALUE_TYPE(dict)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(QVariantMap value MEMBER value)
public:
    explicit Dict() {}
    Q_INVOKABLE explicit Dict(const QVariantMap &v) : value(v) {}
    Q_INVOKABLE QString toString() const { return QVariant(value).toString(); }
    operator QVariant() const { return QVariant::fromValue(*this); }
    QVariantMap value;
};

class Variant {
    Q_GADGET
    QML_VALUE_TYPE(variant)
    QML_CONSTRUCTIBLE_VALUE
    // Expose the payload as a plain QVariant. Using QDBusVariant here with
    // MEMBER makes moc emit an inequality comparison the type doesn't
    // support on Qt < 6.7, breaking the build on the declared 6.8 minimum.
    Q_PROPERTY(QVariant value READ propValue WRITE setPropValue)
public:
    explicit Variant() {}
    Q_INVOKABLE explicit Variant(const QJSValue &v) : value(v.toVariant()) {}
    // Optional payload signature: new DBusQML.variant(value, "as") drives the
    // variant payload's wire signature (empty = inference, unchanged).
    Q_INVOKABLE Variant(const QJSValue &v, const QString &signature)
        : value(v.toVariant()), sig(signature) {}
    Q_INVOKABLE QString toString() const { return value.variant().toString(); }
    operator QVariant() const { return value.variant(); }
    QVariant propValue() const { return value.variant(); }
    void setPropValue(const QVariant &v) { value = QDBusVariant(v); }
    QDBusVariant value;
    QString sig;
};

class Bytes {
    Q_GADGET
    QML_VALUE_TYPE(bytes)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(QByteArray value MEMBER value)
public:
    explicit Bytes() {}
    Q_INVOKABLE explicit Bytes(const QByteArray &v) : value(v) {}
    Q_INVOKABLE QString toString() const { return QString::fromUtf8(value); }
    operator QVariant() const { return QVariant::fromValue(*this); }
    QByteArray value;
};

class Struct {
    Q_GADGET
    QML_VALUE_TYPE(struct_)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(QVariantList value MEMBER value)
public:
    explicit Struct() {}
    Q_INVOKABLE explicit Struct(const QVariantList &v) : value(v) {}
    Q_INVOKABLE QString toString() const { return QVariant::fromValue(value).toString(); }
    operator QVariant() const { return QVariant::fromValue(*this); }
    QVariantList value;
};

class UnixFdPrivate;

// Shared by the per-message decode budget and the live-fd pressure valve.
int fdDecodeBudget();

// DBusQML.unixFd — the only fd value. A unixFd OWNS its descriptor (prior
// art: Node FileHandle, Go os.File, Qt QDBusUnixFileDescriptor, dbus-python
// UnixFd). Sending an `h` requires one (a plain number is a local error on
// every path); every received `h` arrives as one, holding its own dup() —
// two listeners never share a descriptor. Copies (JS copies, QVariant
// copies) share ONE state: close() on any copy closes it for all.
//   valid    — false after close(), and for a received invalid fd
//   close()  — closes now (the contract; idempotent)
//   take()   — a caller-owned DUPLICATE number (closeFd() it); the unixFd
//              itself stays open
// The last copy's destruction closes a still-open fd; for one that reached
// JS this is the garbage-collector fallback and warns. A process-wide
// pressure valve queues a JS collection when wrapper-owned fds pile up.
// Not for concurrent use from several threads.
class UnixFd {
    Q_GADGET
    QML_VALUE_TYPE(unixFd)
    QML_CONSTRUCTIBLE_VALUE
    Q_PROPERTY(bool valid READ isValid)
public:
    UnixFd();
    // JS: new DBusQML.unixFd(n) — duplicates n; the caller keeps (and
    // closes) its own number.
    Q_INVOKABLE explicit UnixFd(const QJSValue &fd);
    // C++: duplicates fd (the caller keeps its own).
    explicit UnixFd(int fd);
    // C++ / receive path: duplicates the descriptor (never shares it); an
    // invalid descriptor gives an invalid unixFd.
    explicit UnixFd(const QDBusUnixFileDescriptor &fd);
    // Internal receive boundary only: fd MUST be the sole holder of a
    // freshly demarshalled (or freshly duplicated) descriptor. Qt's take
    // does not detach shared holders. All ordinary constructors still dup.
    static UnixFd adoptReceivedDescriptor(QDBusUnixFileDescriptor &&fd);
    UnixFd(const UnixFd &other);
    UnixFd &operator=(const UnixFd &other);
    ~UnixFd();

    bool isValid() const;
    Q_INVOKABLE void close();
    Q_INVOKABLE int take() const;
    Q_INVOKABLE QString toString() const;

    // Borrowed number (-1 when invalid); valid only while this unixFd stays
    // open. For I/O inside the library and C++ consumers.
    int fileDescriptor() const;
    // A QDBusUnixFileDescriptor sharing this descriptor (P6: what C++
    // consumers and the wire get). Invalid when this unixFd is invalid.
    QDBusUnixFileDescriptor descriptor() const;
    operator QVariant() const { return QVariant::fromValue(*this); }

    // Delivery into JS: marks every unixFd inside v (lists/maps recurse) as
    // escaped (GC-fallback warning armed) and registers engine for the
    // pressure valve; no-op without an engine. A missed site only loses the
    // warning.
    static void noteDelivered(const QVariant &v, QJSEngine *engine);
    void noteDelivered(QJSEngine *engine) const;

    static int liveCount();         // wrapper-owned open fds, process-wide
    static int pressureThreshold(); // clamp(RLIMIT_NOFILE soft / 4, 64, 1024)

private:
    void init(const QDBusUnixFileDescriptor &owned);
    QExplicitlySharedDataPointer<UnixFdPrivate> d;
};

} // namespace DBus

Q_DECLARE_METATYPE(DBus::Struct)

// D-Bus marshaling for DBus::Struct — beginStructure/endStructure with
// per-member QVariant marshaling.
class QDBusArgument;
QDBusArgument &operator<<(QDBusArgument &arg, const DBus::Struct &s);
const QDBusArgument &operator>>(const QDBusArgument &arg, DBus::Struct &s);

// Distinct type to force D-Bus marshaling fallthrough to appendRegisteredType for "as".
// QStringList is handled by Qt's internal marshaling but may produce "av";
// a distinct type avoids that code path entirely.
struct DBusAsArray {
    QStringList value;
};
Q_DECLARE_METATYPE(DBusAsArray)
QDBusArgument &operator<<(QDBusArgument &arg, const DBusAsArray &a);
const QDBusArgument &operator>>(const QDBusArgument &arg, DBusAsArray &a);

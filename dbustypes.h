#pragma once

#include <QDBusObjectPath>
#include <QDBusSignature>
#include <QDBusVariant>
#include <QJSValue>
#include <QVariantMap>
#include <qqmlregistration.h>

class QDBusArgument;

#define DBUS_QML_TYPE(TYPE, NAME, CTOR, STORE, SIG)                  \
    class TYPE {                                                      \
        Q_GADGET                                                      \
        QML_VALUE_TYPE(NAME)                                          \
        QML_CONSTRUCTIBLE_VALUE                                       \
        Q_PROPERTY(STORE value MEMBER value)                          \
    public:                                                           \
        explicit TYPE() {}                                            \
        Q_INVOKABLE explicit TYPE(CTOR v) : value(v) {}               \
        operator STORE() const { return value; }                      \
        Q_INVOKABLE QString toString() const {                        \
            return QVariant::fromValue(value).toString();             \
        }                                                             \
        operator QVariant() const {                                   \
            return QVariant::fromValue(*this);                        \
        }                                                             \
        STORE value;                                                  \
    };

namespace DBus {

DBUS_QML_TYPE(Uint32, uint32, uint, uint, "u")
DBUS_QML_TYPE(Int32,  int32,  int,   int,  "i")
DBUS_QML_TYPE(Uint16, uint16, uint, ushort, "q")
DBUS_QML_TYPE(Int16,  int16,  int,   short, "n")
DBUS_QML_TYPE(Uint64, uint64, quint64, quint64, "t")
DBUS_QML_TYPE(Int64,  int64,  qint64, qint64,  "x")
DBUS_QML_TYPE(Bool, boolean, bool, bool, "b")
DBUS_QML_TYPE(Double, double, double, double,  "d")
DBUS_QML_TYPE(Byte,   byte,   uint,   uchar,   "y")
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
    // support on Qt < 6.7, breaking the build on the declared 6.5 minimum.
    Q_PROPERTY(QVariant value READ propValue WRITE setPropValue)
public:
    explicit Variant() {}
    Q_INVOKABLE explicit Variant(const QJSValue &v) : value(v.toVariant()) {}
    Q_INVOKABLE QString toString() const { return value.variant().toString(); }
    operator QVariant() const { return value.variant(); }
    QVariant propValue() const { return value.variant(); }
    void setPropValue(const QVariant &v) { value = QDBusVariant(v); }
    QDBusVariant value;
};

} // namespace DBus

// Distinct type to force D-Bus marshaling fallthrough to appendRegisteredType for "as".
// QStringList is handled by Qt's internal marshaling but may produce "av";
// a distinct type avoids that code path entirely.
struct DBusAsArray {
    QStringList value;
};
Q_DECLARE_METATYPE(DBusAsArray)
QDBusArgument &operator<<(QDBusArgument &arg, const DBusAsArray &a);
const QDBusArgument &operator>>(const QDBusArgument &arg, DBusAsArray &a);

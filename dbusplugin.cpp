#include <QDBusMetaType>

#include "dbustypes.h"

// Type converter registrations that must run when the library is loaded.
static void registerTypeConverters() {
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    qDBusRegisterMetaType<QStringList>();
    qDBusRegisterMetaType<DBusAsArray>();
    qDBusRegisterMetaType<DBus::Struct>();
    // a{sa{sv}} — dict of dicts, NM connection-settings shape.
    // Registered so the signature-driven marshaller can produce it from
    // plain JS objects.
    {
        typedef QMap<QString, QVariantMap> StringVariantMapMap;
        qDBusRegisterMetaType<StringVariantMapMap>();
        auto mt = QMetaType::fromType<StringVariantMapMap>();
        if (mt.isValid())
            QDBusMetaType::registerCustomType(mt, QByteArray("a{sa{sv}}"));
    }
    // aa{sv} — array of dicts, NM Ip4Config.AddressData shape. Registered so
    // the signature-driven marshaller can produce it from plain JS arrays.
    {
        typedef QList<QVariantMap> VariantMapList;
        qDBusRegisterMetaType<VariantMapList>();
        auto mt = QMetaType::fromType<VariantMapList>();
        if (mt.isValid())
            QDBusMetaType::registerCustomType(mt, QByteArray("aa{sv}"));
    }
    // NOTE: These registrations are process-global. The QStringList → "as"
    // and DBusAsArray → "as" mappings affect the host app's QtDBus marshaling
    // for ALL D-Bus traffic, not just dbusqml's. This is intentional — the
    // library is a QML plugin loaded into a host process.
    {
        auto mt = QMetaType::fromType<QStringList>();
        if (mt.isValid())
            QDBusMetaType::registerCustomType(mt, QByteArray("as"));
    }
    {
        auto mt = QMetaType::fromType<DBusAsArray>();
        if (mt.isValid())
            QDBusMetaType::registerCustomType(mt, QByteArray("as"));
    }

    QMetaType::registerConverter<DBus::Bool, bool>();
    QMetaType::registerConverter<DBus::Int16, short>();
    QMetaType::registerConverter<DBus::Int32, int>();
    QMetaType::registerConverter<DBus::Int64, qint64>();
    QMetaType::registerConverter<DBus::Uint16, ushort>();
    QMetaType::registerConverter<DBus::Uint32, uint>();
    QMetaType::registerConverter<DBus::Uint64, quint64>();
    QMetaType::registerConverter<DBus::Double, double>();
    QMetaType::registerConverter<DBus::Byte, uchar>();
    QMetaType::registerConverter<DBus::String, QString>();
    QMetaType::registerConverter<DBus::ObjectPath, QDBusObjectPath>(
        [](const DBus::ObjectPath &p) { return p.value; });
    QMetaType::registerConverter<QDBusObjectPath, QString>(
        [](const QDBusObjectPath &p) { return p.path(); });
    QMetaType::registerConverter<DBus::Signature, QDBusSignature>(
        [](const DBus::Signature &s) { return s.value; });
    QMetaType::registerConverter<DBus::Dict, QVariantMap>(
        [](const DBus::Dict &d) { return d.value; });
    QMetaType::registerConverter<DBus::Variant, QDBusVariant>(
        [](const DBus::Variant &v) { return v.value; });
    QMetaType::registerConverter<DBus::Variant, QVariant>(
        [](const DBus::Variant &v) { return v.value.variant(); });
    QMetaType::registerConverter<DBus::Bytes, QByteArray>(
        [](const DBus::Bytes &b) { return b.value; });
    QMetaType::registerConverter<DBus::Struct, QVariantList>(
        [](const DBus::Struct &s) { return s.value; });
}

// Static initializer — runs when the shared library is loaded
namespace {
struct Init {
    Init() { registerTypeConverters(); }
} _init;
} // namespace

// There is deliberately no QML plugin class in this file: qt_add_qml_module
// generates the real one into libdbusqmlplugin.so. The hand-written class
// that used to live here compiled into libdbusqml.so, was never instantiated,
// and its duplicate staticMetaObject tripped ASan's ODR check whenever a
// process loaded both DSOs (quiet-hours report §4).

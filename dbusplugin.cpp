#include <QDBusMetaType>
#include <QQmlEngineExtensionPlugin>

#include "dbustypes.h"

void qml_register_types_DBus();

// Type converter registrations that must run when the library is loaded.
// Called both from the static initializer below and from the plugin constructor.
static void registerTypeConverters()
{
    static bool registered = false;
    if (registered) return;
    registered = true;

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
}

// Static initializer — runs when the shared library is loaded
namespace { struct Init { Init() { registerTypeConverters(); } } _init; }

class DBusPlugin : public QQmlEngineExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)
    Q_DISABLE_COPY_MOVE(DBusPlugin)

public:
    DBusPlugin(QObject *parent = nullptr)
        : QQmlEngineExtensionPlugin(parent)
    {
        registerTypeConverters();
        volatile auto registration = &qml_register_types_DBus;
        Q_UNUSED(registration);
    }
};

#include "dbusplugin.moc"

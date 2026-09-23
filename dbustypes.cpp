#include "dbustypes.h"
#include "dbusutils.h"

#include <QDBusArgument>

QDBusArgument &operator<<(QDBusArgument &arg, const DBusAsArray &a) {
    arg << a.value;
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBusAsArray &a) {
    const QVariant got = readBySignature(arg);
    if (got.userType() == QMetaType::QStringList)
        a.value = got.toStringList();
    else {
        a.value.clear();
        for (const QVariant &item : got.toList())
            a.value.append(item.toString());
    }
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const DBus::Struct &s) {
    arg.beginStructure();
    for (const QVariant &m : s.value) {
        // Marshal each member by its concrete type. QVariant doesn't have
        // a direct operator<< — dispatch on the metatype.
        switch (m.userType()) {
        case QMetaType::Bool:
            arg << m.toBool();
            break;
        case QMetaType::Int:
            arg << m.toInt();
            break;
        case QMetaType::UInt:
            arg << m.toUInt();
            break;
        case QMetaType::Short:
            arg << static_cast<short>(m.toInt());
            break;
        case QMetaType::UShort:
            arg << static_cast<ushort>(m.toUInt());
            break;
        case QMetaType::LongLong:
            arg << m.toLongLong();
            break;
        case QMetaType::ULongLong:
            arg << m.toULongLong();
            break;
        case QMetaType::Double:
            arg << m.toDouble();
            break;
        case QMetaType::QString:
            arg << m.toString();
            break;
        case QMetaType::QByteArray:
            arg << m.toByteArray();
            break;
        case QMetaType::QStringList:
            arg << m.toStringList();
            break;
        case QMetaType::QVariantList:
            arg << m.toList();
            break;
        case QMetaType::QVariantMap:
            arg << m.toMap();
            break;
        default:
            if (m.userType() == qMetaTypeId<QDBusVariant>())
                arg << m.value<QDBusVariant>();
            else if (m.userType() == qMetaTypeId<QDBusObjectPath>())
                arg << m.value<QDBusObjectPath>();
            else if (m.userType() == qMetaTypeId<QDBusSignature>())
                arg << m.value<QDBusSignature>();
            else if (m.userType() == qMetaTypeId<DBus::Struct>())
                arg << m.value<DBus::Struct>(); // nested struct
            else
                qWarning("DBus::Struct: unsupported member type %d", m.userType());
            break;
        }
    }
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBus::Struct &s) {
    arg.beginStructure();
    s.value.clear();
    while (!arg.atEnd())
        s.value.append(readBySignature(arg));
    arg.endStructure();
    return arg;
}

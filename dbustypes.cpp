#include "dbustypes.h"

#include <QDBusArgument>

QDBusArgument &operator<<(QDBusArgument &arg, const DBusAsArray &a)
{
    arg << a.value;
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBusAsArray &a)
{
    arg >> a.value;
    return arg;
}

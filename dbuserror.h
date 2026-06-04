#pragma once

#include <QDBusError>
#include <qqmlregistration.h>

class DBusError {
    Q_GADGET
    QML_VALUE_TYPE(dbusError)
    QML_STRUCTURED_VALUE

    Q_PROPERTY(bool isValid READ isValid)
    Q_PROPERTY(QString name READ name)
    Q_PROPERTY(QString message READ message)

public:
    explicit DBusError() = default;
    explicit DBusError(const QDBusError &err)
        : m_isValid(err.isValid())
        , m_name(err.name())
        , m_message(err.message()) {}

    bool isValid() const { return m_isValid; }
    QString name() const { return m_name; }
    QString message() const { return m_message; }

private:
    bool m_isValid = false;
    QString m_name;
    QString m_message;
};

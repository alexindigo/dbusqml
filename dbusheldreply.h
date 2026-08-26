#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <qqmlregistration.h>

class DBusAdaptor;

// A deferred method reply. Created only by DBusAdaptor::holdReply() during
// method dispatch; the QML handler holds it and settles it later with send()
// or sendError(). While pending it is the only handle that can answer the
// caller, so it is parented to the adaptor with CppOwnership; after settle it
// is handed to the JS garbage collector (JavaScriptOwnership).
class DBusHeldReply : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DBusHeldReply)
    QML_UNCREATABLE("Created by holdReply()")

public:
    explicit DBusHeldReply(QObject *parent = nullptr);

    void setContext(DBusAdaptor *adaptor, const QDBusMessage &msg, const QDBusConnection &conn,
                    const QString &member);
    bool isSettled() const { return m_settled; }

    Q_INVOKABLE void send(const QJSValue &value = QJSValue::UndefinedValue);
    Q_INVOKABLE void sendError(const QString &name, const QString &message = QString());

private:
    void settle();

    QPointer<DBusAdaptor> m_adaptor;
    QDBusMessage m_msg;
    QDBusConnection m_conn;
    QString m_member;
    bool m_settled = false;
};

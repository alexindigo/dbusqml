#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QTimer>
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

    // Caller identification (features train, Phase 4): the unique bus
    // name captured at hold time. Survives settle-later (the delivery
    // message is copied into the held reply at holdReply()).
    Q_INVOKABLE QString callerService() const { return m_caller; }

    Q_INVOKABLE void send(const QJSValue &value = QJSValue::UndefinedValue);
    Q_INVOKABLE void sendError(const QString &name, const QString &message = QString());

private:
    void settle();
    // Phase 9: TTL expiry entry point (called by the adaptor's timer).
    void expire();

    friend class DBusAdaptor;

    QPointer<DBusAdaptor> m_adaptor;
    QDBusMessage m_msg;
    QDBusConnection m_conn;
    QString m_member;
    QString m_caller;
    bool m_settled = false;
    // CF-27: the TTL single-shot, owned by the reply — settle() stops it
    // so the timer does not outlive its purpose (expire() early-outs on
    // m_settled either way, so this is hygiene + doc-truth, not a
    // behavior change on the reply count).
    QTimer *m_ttlTimer = nullptr;
};

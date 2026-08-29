#include "dbusheldreply.h"

#include "dbusadaptor.h"

#include <QQmlEngine>

DBusHeldReply::DBusHeldReply(QObject *parent)
    : QObject(parent), m_conn(QDBusConnection::sessionBus()) {}

void DBusHeldReply::setContext(DBusAdaptor *adaptor, const QDBusMessage &msg,
                               const QDBusConnection &conn, const QString &member) {
    m_adaptor = adaptor;
    m_msg = msg;
    m_conn = conn;
    m_member = member;
}

void DBusHeldReply::send(const QJSValue &value) {
    if (m_settled) {
        qWarning("dbusqml: DBusHeldReply already settled - ignoring send");
        return;
    }
    const QVariant v = value.isUndefined() ? QVariant() : qjsValueToVariant(value);
    if (m_adaptor)
        m_adaptor->sendMethodReply(m_conn, m_msg, m_member, v);
    settle();
}

void DBusHeldReply::sendError(const QString &name, const QString &message) {
    if (m_settled) {
        qWarning("dbusqml: DBusHeldReply already settled - ignoring sendError");
        return;
    }
    // Single-string convenience: a name without a '.' is the message, using
    // the generic failure error name.
    QString errorName = name;
    QString errorMessage = message;
    if (!name.contains(QLatin1Char('.'))) {
        errorName = QStringLiteral("org.freedesktop.DBus.Error.Failed");
        errorMessage = name;
    }
    m_conn.send(m_msg.createErrorReply(errorName, errorMessage));
    settle();
}

void DBusHeldReply::settle() {
    m_settled = true;
    // Ownership audit (0.7.0): CppOwnership while pending → JavaScriptOwnership
    // after settle (0.5.0 design — intended). The adaptor defers its
    // post-dispatch ownership restore while any held reply is outstanding;
    // notify it that one just settled (the last settle triggers the restore).
    QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
    if (m_adaptor)
        m_adaptor->heldReplySettled();
}

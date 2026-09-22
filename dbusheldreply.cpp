#include "dbusheldreply.h"

#include "dbusadaptor.h"
#include "dbusconnection.h" // CF-1: wireMarshalable — shared marshal-boundary guard
#include "dbusutils.h"

#include <QDBusError>
#include <QQmlEngine>

DBusHeldReply::DBusHeldReply(QObject *parent)
    : QObject(parent), m_conn(QDBusConnection::sessionBus()) {}

void DBusHeldReply::setContext(DBusAdaptor *adaptor, const QDBusMessage &msg,
                               const QDBusConnection &conn, const QString &member) {
    m_adaptor = adaptor;
    m_msg = msg;
    m_conn = conn;
    m_member = member;
    // Phase 4: capture the caller at hold time (the delivery message's
    // sender). Queryable via callerService() even after settle-later.
    m_caller = msg.service();
}

void DBusHeldReply::send(const QJSValue &value) {
    if (m_settled) {
        qWarning("dbusqml: DBusHeldReply already settled - ignoring send");
        return;
    }
    if (!m_adaptor) {
        qWarning("dbusqml: held reply for %s: adaptor destroyed before send — replying Failed",
                 qPrintable(m_member));
        if (m_msg.isReplyRequired())
            checkedSend(m_conn,
                        m_msg.createErrorReply(QDBusError::Failed,
                                               QStringLiteral("adaptor destroyed before reply")),
                        "held Failed reply", m_member);
        settle();
        return;
    }
    const QVariant v = value.isUndefined() ? QVariant() : qjsValueToVariant(value);
    if (m_msg.isReplyRequired()) // B4: NO_REPLY_EXPECTED — send nothing
        m_adaptor->sendMethodReply(m_conn, m_msg, m_member, v);
    settle();
}

void DBusHeldReply::sendError(const QString &name, const QString &message) {
    if (m_settled) {
        qWarning("dbusqml: DBusHeldReply already settled - ignoring sendError");
        return;
    }
    // CF-1: same B11 grammar validation + Failed fallback as the
    // throw path (dbusadaptor.cpp normalizeErrorName) — an invalid
    // dotted name used to produce zero replies for the serial. The
    // send result is checked and loud (member named); settle is
    // unconditional so exactly-one-reply holds either way.
    const auto norm = DBusAdaptor::normalizeErrorName(name, message, false);
    if (m_msg.isReplyRequired()) { // B4: NO_REPLY_EXPECTED — send nothing
        checkedSend(m_conn, m_msg.createErrorReply(norm.first, norm.second), "held error reply",
                    m_member);
    }
    settle();
}

void DBusHeldReply::settle() {
    m_settled = true;
    // CF-27: actually stop the TTL timer (the comment at the hold site
    // promises settle cancels it). expire() early-outs regardless, so
    // the reply count is unchanged — the timer just no longer outlives
    // its purpose.
    if (m_ttlTimer) {
        m_ttlTimer->stop();
        m_ttlTimer = nullptr;
    }
    // Ownership audit (0.7.0): CppOwnership while pending → JavaScriptOwnership
    // after settle (0.5.0 design — intended). The adaptor keeps its own
    // ownership through dispatch (0.8.0 preservation) — no notification needed.
    QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
}

void DBusHeldReply::expire() {
    // Phase 9: held-reply TTL expiry. Settles with Failed ("reply timed
    // out") + warn; exactly-one-reply preserved (settle marks settled;
    // a later consumer send() is ignored as already-settled).
    if (m_settled)
        return;
    qWarning("dbusqml: held reply for %s timed out — replying Failed", qPrintable(m_member));
    if (m_msg.isReplyRequired()) // B4: NO_REPLY_EXPECTED — send nothing
        checkedSend(m_conn,
                    m_msg.createErrorReply(QStringLiteral("org.freedesktop.DBus.Error.Failed"),
                                           QStringLiteral("reply timed out")),
                    "held expire", m_member);
    settle();
}

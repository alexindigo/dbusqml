#include "dbuspendingreply.h"
#include "dbusconnection.h"
#include "dbusutils.h"

#include <QDBusMessage>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>
#include <QQmlEngine>

DBusPendingReply::DBusPendingReply(QObject *parent) : QObject(parent) {}

// Ownership: DBusPendingReply is created C++-side (by the proxy's
// call/getProperty and DbusMethodHelper::callMethod, and by
// DBusConnection::asyncCall) parented to its creator — in flight it is never
// JS-GC collected. settle() applies the one lifetime policy at completion:
// with an engine (and the handoff enabled) the reply is handed to the JS GC;
// without one it keeps its parent and dies with its owner. The reply data
// survives past finished() via caching, so consumers read it at leisure.

DBusPendingReply::~DBusPendingReply() {}

void DBusPendingReply::setWatcher(QDBusPendingCallWatcher *watcher) {
    m_watcher = watcher;
    // SingleShotConnection: auto-disconnects after firing once, so the
    // callback doesn't hold a reference that would prevent GC. The watcher
    // is deleted immediately after the callback completes — safe because
    // this is the only slot (SingleShotConnection).
    connect(watcher, &QDBusPendingCallWatcher::finished, this, &DBusPendingReply::onFinished,
            Qt::SingleShotConnection);
}

bool DBusPendingReply::isError() const {
    if (m_finished)
        return m_isError;
    return m_watcher ? m_watcher->isError() : true;
}

bool DBusPendingReply::isValid() const {
    if (m_finished)
        return m_isValid;
    return m_watcher && m_watcher->isValid() && !m_watcher->isError();
}

DBusError DBusPendingReply::error() const {
    if (m_finished)
        return m_error;
    if (m_watcher)
        return DBusError(m_watcher->error());
    return DBusError(QDBusError(QDBusError::InternalError, "No pending call"));
}

QJSValue DBusPendingReply::valueJs() {
    // Convert on demand — no caching, no engine-lifetime coupling.
    // The engine is alive when QML reads this property (QML evaluation
    // context). If the engine is gone, return an invalid QJSValue.
    QQmlEngine *engine = m_engine ? m_engine.data() : nullptr;
    if (!engine)
        engine = qmlEngine(parent());
    if (!engine)
        return {};
    return variantToJs(engine, value());
}

QJSValue DBusPendingReply::valuesJs() {
    QQmlEngine *engine = m_engine ? m_engine.data() : nullptr;
    if (!engine)
        engine = qmlEngine(parent());
    if (!engine)
        return {};
    return variantToJs(engine, QVariant::fromValue(values()));
}

QVariant DBusPendingReply::value() const {
    if (m_finished)
        return m_value;
    if (!m_watcher || m_watcher->isError())
        return {};

    const QVariantList decoded = values();
    return decoded.isEmpty() ? QVariant() : decoded.first();
}

QVariantList DBusPendingReply::values() const {
    if (m_finished)
        return m_values;
    if (!m_watcher || m_watcher->isError())
        return {};

    QVariantList args;
    decodeMessage(m_watcher->reply(), args);
    return args;
}

void DBusPendingReply::completeLocalError(const QString &name, const QString &message) {
    Outcome outcome;
    outcome.isError = true;
    outcome.error = DBusError(name, message);
    // Always queued: a local failure settles inside the call that created
    // the reply, before any caller (JS, the promise overload, or a C++
    // consumer — with or without an engine) could connect to finished.
    settle(std::move(outcome), Delivery::Queued);
}

void DBusPendingReply::onFinished(QDBusPendingCallWatcher *watcher) {
    Outcome outcome;
    outcome.isError = watcher->isError();
    outcome.isValid = watcher->isValid() && !watcher->isError();
    if (outcome.isError)
        outcome.error = DBusError(watcher->error());
    else {
        QDBusError error;
        if (!decodeMessage(watcher->reply(), outcome.values, &error)) {
            outcome.isError = true;
            outcome.isValid = false;
            outcome.error = DBusError(error);
        }
    }
    // Clean up the watcher. SingleShotConnection means this is the only
    // slot, so the signal emission completes before the destructor runs.
    // Immediate delete (not deleteLater) is safe here.
    delete watcher;
    m_watcher = nullptr;
    settle(std::move(outcome), Delivery::QueuedWhenEngine);
}

void DBusPendingReply::completeFromReply(const QDBusMessage &reply) {
    // P9: share one wire reply across coalesced waiters (same error vs
    // value split + unwrap as onFinished).
    // DELIVERY NOTE: onFinished queues finished() when an engine is
    // present (queued delivery needs the loop turning); coalesced
    // waiters complete SYNCHRONOUSLY here (direct emit) because the
    // owner already pumped the loop to observe its own completion —
    // the waiters' data is cached and finished() observers fire
    // without another loop turn (probe-verified: queued-emit waiters
    // stall at 1/8 on one connection, direct-emit completes 8/8).
    Outcome outcome;
    if (reply.type() == QDBusMessage::ErrorMessage) {
        outcome.isError = true;
        outcome.error = DBusError(reply.errorName(), reply.errorMessage());
    } else {
        outcome.isValid = true;
        QDBusError error;
        if (!decodeMessage(reply, outcome.values, &error)) {
            outcome.isError = true;
            outcome.isValid = false;
            outcome.error = DBusError(error);
        }
    }
    settle(std::move(outcome), Delivery::Direct);
}

void DBusPendingReply::settle(Outcome outcome, Delivery delivery) {
    if (m_finished)
        return; // one-shot: a settled reply never changes again
    m_isError = outcome.isError;
    m_isValid = outcome.isValid;
    m_error = outcome.error;
    m_value = outcome.values.isEmpty() ? QVariant() : outcome.values.first();
    m_values = std::move(outcome.values);
    m_finished = true;

    // Queued delivery lets a synchronous reply.finished.connect() right
    // after the call (or after a local failure) land first. The disconnect
    // lives in deliverFinished, so it follows the emission, never the post.
    QPointer<DBusPendingReply> self(this);
    if (delivery == Delivery::Queued || (m_engine && delivery == Delivery::QueuedWhenEngine))
        QMetaObject::invokeMethod(this, &DBusPendingReply::deliverFinished, Qt::QueuedConnection);
    else
        deliverFinished();
    // A re-entrant handler in the direct delivery may have destroyed this
    // reply — nothing left to hand off.
    if (!self)
        return;

    // R4 (road-to-one, owner Call 4 = safe-A): hand completed replies to
    // the JS garbage collector — SYNCHRONOUSLY, in this same stack frame.
    // The 27a5f72 attempt posted the handoff as a queued event and died in
    // ccaade6 (the posted lambda ran during engine teardown, touching
    // half-destroyed engine state). A synchronous handoff runs while the
    // engine is verifiably alive (m_engine is a QPointer), so teardown
    // never observes a reparent. The queued finished posted above is
    // delivered before any deferred delete a later collection posts.
    // Settling DURING engine destruction is reachable only through JS run
    // by Component.onDestruction (a call that fails locally): Qt runs those
    // handlers first in ~QQmlEngine, before any cleanup step, so the handoff
    // still meets an intact engine and the final sweep deletes the reply
    // (pinned: testReplySettleDuringEngineTeardown). Wire completions
    // arrive as queued watcher signals, never inside the destructor.
    if (m_engine && m_gcHandoff) {
        setParent(nullptr);
        QQmlEngine::setObjectOwnership(this, QQmlEngine::JavaScriptOwnership);
    }
    // No engine (pure C++ consumer): keep the parent.
}

void DBusPendingReply::deliverFinished() {
    QPointer<DBusPendingReply> self(this);
    emit finished();
    // A handler may have destroyed the reply.
    if (!self)
        return;
    // finished is one-shot: drop every receiver so a JS handler that
    // captures this reply (r.finished.connect(() => r.value)) no longer
    // roots it — the connection held the closure strongly, the closure held
    // the reply's wrapper, and the cycle was never collected.
    disconnect(this, &DBusPendingReply::finished, nullptr, nullptr);
}

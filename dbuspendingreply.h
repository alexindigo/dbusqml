#pragma once

#include <QDBusPendingCallWatcher>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QVariant>

class QQmlEngine;
#include <qqmlregistration.h>

#include "dbuserror.h"

class DBusPendingReply : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DBusPendingReply)
    QML_UNCREATABLE("Created by asyncCall")

    Q_PROPERTY(bool isFinished READ isFinished NOTIFY finished)
    Q_PROPERTY(bool isError READ isError NOTIFY finished)
    Q_PROPERTY(bool isValid READ isValid NOTIFY finished)
    Q_PROPERTY(DBusError error READ error NOTIFY finished)
    Q_PROPERTY(QJSValue value READ valueJs NOTIFY finished)
    Q_PROPERTY(QJSValue values READ valuesJs NOTIFY finished)

public:
    explicit DBusPendingReply(QObject *parent = nullptr);
    ~DBusPendingReply() override;

    void setWatcher(QDBusPendingCallWatcher *watcher);
    void setEngine(QQmlEngine *engine) { m_engine = engine; }

    // Lifetime policy (applied once, by settle()): a completed reply with an
    // engine is handed to the JS garbage collector (R4). A reply whose
    // lifetime the LIBRARY manages C++-side opts out BEFORE it can complete:
    // the promise-style asyncCall overload settles its reply itself
    // (deleteLater, CF-11) and never returns it to JS — handing it off would
    // unparent it with no JS wrapper ever created (nothing to collect it).
    void setGcHandoffEnabled(bool on) { m_gcHandoff = on; }

    // Complete LOCALLY with a D-Bus error — no wire round-trip. The
    // client-exit guard path: a call whose arguments cannot be marshaled
    // fails here (with org.freedesktop.DBus.Error.Failed, "argument N is not
    // marshalable") instead of sending garbage or aborting the caller.
    void completeLocalError(const QString &name, const QString &message);

    // P9 (features train, Phase 8): complete from an already-received
    // wire reply (coalesced-Get waiters share one reply).
    void completeFromReply(const QDBusMessage &reply);

    bool isFinished() const { return m_finished; }
    bool isError() const;
    bool isValid() const;
    DBusError error() const;
    QJSValue valueJs();
    QJSValue valuesJs();

    // C++ accessors — raw QVariant data
    QVariant value() const;
    QVariantList values() const;

signals:
    // Delivered exactly once; every receiver is disconnected right after
    // the emission returns (a settled reply never changes again, and a
    // handler that captures its reply must not keep it alive). A
    // connection made after delivery never fires.
    void finished();

private:
    struct Outcome {
        bool isError = false;
        bool isValid = false;
        DBusError error;
        QVariantList values; // unwrapped
    };
    enum class Delivery {
        Queued,           // always on a later loop turn
        QueuedWhenEngine, // queued if an engine may be listening, else direct
        Direct,           // always in this stack frame
    };

    void onFinished(QDBusPendingCallWatcher *watcher);
    // The ONLY completion path: cache → mark finished → deliver finished
    // once (deliverFinished) → lifetime policy. Every entry point calls it.
    void settle(Outcome outcome, Delivery delivery);
    // Emit finished, then disconnect all its receivers.
    void deliverFinished();

    QPointer<QDBusPendingCallWatcher> m_watcher;
    // Set by settle(); from then on the accessors answer from the cache —
    // the reply is self-contained after the watcher is deleted.
    bool m_finished = false;
    bool m_isError = false;
    bool m_isValid = false;
    DBusError m_error;
    QVariant m_value;
    QVariantList m_values;
    QPointer<QQmlEngine> m_engine;
    bool m_gcHandoff = true;
};

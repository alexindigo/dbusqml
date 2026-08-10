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
    Q_PROPERTY(QJSValue value READ value NOTIFY finished)
    Q_PROPERTY(QJSValue values READ values NOTIFY finished)

public:
    explicit DBusPendingReply(QObject *parent = nullptr);
    ~DBusPendingReply() override;

    void setWatcher(QDBusPendingCallWatcher *watcher);
    void setEngine(QQmlEngine *engine) { m_engine = engine; }

    bool isFinished() const { return m_finished; }
    bool isError() const;
    bool isValid() const;
    DBusError error() const;
    QJSValue value() const;
    QJSValue values() const;

    // C++ accessors — raw QVariant data (unchanged)
    QVariant valueVariant() const;
    QVariantList valuesVariant() const;

signals:
    void finished();

private:
    void onFinished(QDBusPendingCallWatcher *watcher);

    QPointer<QDBusPendingCallWatcher> m_watcher;
    bool m_finished = false;

    // Cached after watcher finishes — makes the reply self-contained
    // so it can be safely accessed after the watcher is deleted.
    bool m_isError = false;
    bool m_isValid = false;
    DBusError m_error;
    QVariant m_value;
    QVariantList m_values;
    bool m_cached = false;

    // JS-native versions of m_value/m_values — real Array/Object for QML
    QJSValue m_jsValue;
    QJSValue m_jsValues;
    bool m_jsCached = false;
    QQmlEngine *m_engine = nullptr;
};

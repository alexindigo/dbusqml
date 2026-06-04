#pragma once

#include <QDBusPendingCallWatcher>
#include <QObject>
#include <QVariant>
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
    Q_PROPERTY(QVariant value READ value NOTIFY finished)
    Q_PROPERTY(QVariantList values READ values NOTIFY finished)

public:
    explicit DBusPendingReply(QObject *parent = nullptr);
    ~DBusPendingReply() override;

    void setWatcher(QDBusPendingCallWatcher *watcher);

    bool isFinished() const { return m_finished; }
    bool isError() const;
    bool isValid() const;
    DBusError error() const;
    QVariant value() const;
    QVariantList values() const;

signals:
    void finished();

private:
    void onFinished(QDBusPendingCallWatcher *watcher);

    QDBusPendingCallWatcher *m_watcher = nullptr;
    bool m_finished = false;
};

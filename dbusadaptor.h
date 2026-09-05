#pragma once

#include <QDBusVirtualObject>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <qqmlregistration.h>

#include "dbusconnection.h"
#include "dbusheldreply.h"

class DBusAdaptor : public QDBusVirtualObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_NAMED_ELEMENT(DBusAdaptor)

    // FINAL: shadowing a built-in from QML is a load-time error, not a
    // silently broken adaptor (the shadow used to swallow the registration
    // config AND leak through Properties.Get).
    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged FINAL)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged FINAL)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged FINAL)
    Q_PROPERTY(DBusConnection *connection READ connection WRITE setConnection NOTIFY
                   connectionChanged FINAL)
    Q_PROPERTY(QVariantMap _signatures READ signatures WRITE setSignatures NOTIFY signaturesChanged)
    // Explicit wire-surface declarations (the explicit tier of the naming
    // ladder): _signals maps wire signal name → concatenated arg signature;
    // _members maps wire member name → QML function/property name.
    Q_PROPERTY(QVariantMap _signals READ signalSpecs WRITE setSignalSpecs NOTIFY _signalsChanged)
    Q_PROPERTY(
        QVariantMap _members READ memberAliases WRITE setMemberAliases NOTIFY _membersChanged)
    // Service-name acquisition options (all default false — today's behavior):
    // allow others to take the name from us, take it from a current owner, or
    // queue until it becomes available.
    Q_PROPERTY(bool allowReplacement READ allowReplacement WRITE setAllowReplacement NOTIFY
                   allowReplacementChanged)
    Q_PROPERTY(bool replaceExisting READ replaceExisting WRITE setReplaceExisting NOTIFY
                   replaceExistingChanged)
    Q_PROPERTY(bool queueOnBusy READ queueOnBusy WRITE setQueueOnBusy NOTIFY queueOnBusyChanged)

public:
    explicit DBusAdaptor(QObject *parent = nullptr);
    ~DBusAdaptor() override;

    QString service() const { return m_service; }
    void setService(const QString &v);

    QString path() const { return m_path; }
    void setPath(const QString &v);

    QString iface() const { return m_iface; }
    void setIface(const QString &v);

    DBusConnection *connection() const { return m_conn.data(); }
    void setConnection(DBusConnection *v);

    QVariantMap signatures() const { return m_signatures; }
    void setSignatures(const QVariantMap &v);

    QVariantMap signalSpecs() const { return m_signals; }

    // A6: declared per-arg types for a signal (_signals → catalog), for
    // emission-side marshaling (relay + emitSignal). Empty = undeclared.
    Q_INVOKABLE QStringList declaredSignalTypes(const QString &name) const;
    void setSignalSpecs(const QVariantMap &v);

    QVariantMap memberAliases() const { return m_members; }
    void setMemberAliases(const QVariantMap &v);

    bool allowReplacement() const { return m_allowReplacement; }
    void setAllowReplacement(bool v);

    bool replaceExisting() const { return m_replaceExisting; }
    void setReplaceExisting(bool v);

    bool queueOnBusy() const { return m_queueOnBusy; }
    void setQueueOnBusy(bool v);

    // QQmlParserStatus
    void classBegin() override {}
    void componentComplete() override;

    // QDBusVirtualObject
    QString introspect(const QString &path) const override;
    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override;

    Q_INVOKABLE void emitSignal(const QString &name,
                                const QJSValue &arguments = QJSValue::UndefinedValue);

    // Deferred replies. Valid only synchronously during a method handler;
    // returns nullptr (with a warning) outside dispatch. The returned
    // DBusHeldReply is the only handle that can answer the caller.
    Q_INVOKABLE DBusHeldReply *holdReply();

    // Deterministic retirement of a dynamically created adaptor: runs the
    // destructor's detach tail (path + service reference via the dispatcher
    // registry — idempotent) and errors out outstanding held replies, leaving
    // the QObject alive for QML to drop whenever. One-way: re-registration
    // after unregister() is NOT supported.
    Q_INVOKABLE void unregister();

    // Shared reply tail: marshalability guard -> declared out-signatures ->
    // marshalBySignature -> multi-out split -> send. Used by the synchronous
    // dispatch path and by DBusHeldReply::send().
    void sendMethodReply(const QDBusConnection &conn, const QDBusMessage &msg,
                         const QString &member, const QVariant &retVal);

Q_SIGNALS:
    void serviceChanged();
    void pathChanged();
    void ifaceChanged();
    void connectionChanged();
    void signaturesChanged();
    void _signalsChanged();
    void _membersChanged();
    void allowReplacementChanged();
    void replaceExistingChanged();
    void queueOnBusyChanged();
    // Fired when the service name acquisition state changes (claimed, queued,
    // lost to another owner).
    void nameAcquired();
    void nameLost();

private:
    friend class PropertiesChangedRelay;
    friend class DBusPathDispatcher;
    // Service-name ownership notifications (invoked by the dispatcher's
    // owner-change watch).
    void nameAcquiredInternal();
    void nameLostInternal();
    QString generateXml() const;
    QDBusConnection bus() const;
    QStringList declaredOutTypes(const QString &member) const;

    // The naming ladder (explicit _members → catalog → first-char-upper fold):
    // the wire name advertised for a QML member name (methods and properties).
    QString advertisedName(const QString &qmlName) const;
    // The incoming-wire-name → QML-name resolution used by dispatch: explicit
    // _members alias → exact → first-char-lower fold.
    QStringList candidateQmlNames(const QString &wireName) const;
    // Emits org.freedesktop.DBus.Properties.PropertiesChanged for the
    // property at propIndex (connected to its notify signal at completion).
    void emitPropertiesChanged(int propIndex);

    QString m_service;
    QString m_path;
    QString m_iface;
    QPointer<DBusConnection> m_conn;
    QVariantMap m_signatures;
    QVariantMap m_signals;
    QVariantMap m_members;
    bool m_allowReplacement = false;
    bool m_replaceExisting = false;
    bool m_queueOnBusy = false;
    bool m_attached = false;

    // Dispatch context for holdReply(): the in-flight call's message,
    // connection, and member name. Set around the handler invocation, cleared
    // after; `held` records that the handler deferred the reply.
    struct PendingCall {
        QDBusMessage msg;
        QDBusConnection conn;
        QString member;
        bool held = false;
    };
    PendingCall m_currentCall;
    bool m_inDispatch = false;
};

// Convert a QJSValue to QVariant for D-Bus marshaling (preserves DBus.*
// gadget types). Shared between the sync dispatch path and DBusHeldReply.
QVariant qjsValueToVariant(const QJSValue &jsval);

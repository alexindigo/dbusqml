#pragma once

#include <QDBusVirtualObject>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QStack>
#include <QVarLengthArray>
#include <optional>
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
    // P10a (features train, Phase 5): per-method served option
    // whitelist — { "MethodName": { "key": "sig", … } }, applied to
    // the method's last a{sv} in-arg (xdp xdp_filter_options shape).
    Q_PROPERTY(QVariantMap _options READ optionSpecs WRITE setOptionSpecs NOTIFY _optionsChanged)
    // P10b (features train, Phase 6): sender authorization — the unique
    // bus name allowed to call this adaptor (empty = open). Mismatch →
    // AccessDenied before any handler runs (xdp-request.c:121-139).
    Q_PROPERTY(
        QString allowedSender READ allowedSender WRITE setAllowedSender NOTIFY allowedSenderChanged)
    // Phase 9 (features train): held-reply TTL — max lifetime of a held
    // reply in ms (0 = disabled, the default: zero behavior change
    // unless opted in). On expiry the held reply settles with Failed
    // ("reply timed out") + warn; settle cancels the timer.
    Q_PROPERTY(int heldReplyTimeout READ heldReplyTimeout WRITE setHeldReplyTimeout NOTIFY
                   heldReplyTimeoutChanged)
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

    QVariantMap optionSpecs() const { return m_options; }
    void setOptionSpecs(const QVariantMap &v);

    QString allowedSender() const { return m_allowedSender; }
    void setAllowedSender(const QString &v);

    int heldReplyTimeout() const { return m_heldReplyTimeout; }
    void setHeldReplyTimeout(int v);
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

    // Caller identification (features train, Phase 4): the unique bus
    // name of the caller of the currently-dispatched method. Valid only
    // synchronously during dispatch (outside: warn + empty). The
    // delivery message is already held in the RAII DispatchScope — no
    // QDBusContext inheritance needed. Held-reply path: the caller is
    // captured at hold time and remains queryable on the DBusHeldReply.
    Q_INVOKABLE QString callerService() const;

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

    // CF-1: shared error-name normalization (B11 grammar + Failed
    // fallback). Used by the throw path and DBusHeldReply::sendError so
    // both get identical validation. Declared here (not file-static) so
    // the held-reply TU can share it without duplication.
    static QPair<QString, QString> normalizeErrorName(const QString &name, const QString &message,
                                                      bool declaredShape);

Q_SIGNALS:
    void serviceChanged();
    void pathChanged();
    void ifaceChanged();
    void connectionChanged();
    void signaturesChanged();
    void _signalsChanged();
    void _membersChanged();
    void _optionsChanged();
    void allowedSenderChanged();
    void heldReplyTimeoutChanged();
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
    friend class OwnerChangeRelay;
    // Service-name ownership notifications (invoked by the dispatcher's
    // owner-change watch).
    void nameAcquiredInternal();
    void nameLostInternal();
    QString generateXml() const;
    QDBusConnection bus() const;
    QStringList declaredOutTypes(const QString &member, bool *found = nullptr) const;
    // P10a: validate _options entries against the method's in-arg shape
    // (FD4); warn + drop entries without a trailing a{sv}. Also resolves
    // the entry's method-name key through the naming ladder.
    void validateOptionSpecs();
    // P10a: the whitelist for a wire method name ({ key: sig }), or
    // nullopt when the method has no entry (filter OFF). R2: an engaged
    // EMPTY map is a real entry — filter ON with zero declared keys
    // (deny-all-keys). Name resolution: exact wire name → folded QML name
    // → alias (same ladder as dispatch).
    std::optional<QVariantMap> optionWhitelist(const QString &wireMember) const;
    // P10a: xdp_filter_options shape — drop unknown keys, type-check the
    // rest against the declared sigs. Returns the filtered dict; sets
    // *error to InvalidArgs detail on mistype (caller sends the reply).
    QVariantMap filterOptions(const QVariantMap &whitelist, const QVariantMap &options,
                              QString *error) const;

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
    // CF-29: the attach-time connection BY VALUE — teardown tails
    // (destructor, engine-teardown, unregister) detach on this, never by
    // re-resolving through the null-able m_conn above.
    QDBusConnection m_teardownConn = QDBusConnection::sessionBus();
    QVariantMap m_signatures;
    QVariantMap m_signals;
    QVariantMap m_members;
    QVariantMap m_options;
    QString m_allowedSender;
    int m_heldReplyTimeout = 0;
    bool m_allowReplacement = false;
    bool m_replaceExisting = false;
    bool m_queueOnBusy = false;
    bool m_attached = false;

    // Dispatch context for holdReply(): the in-flight call's message,
    // connection, member name, and the held-reply object (if any). A stack:
    // nested dispatches (re-entrant handleMessage on the same adaptor)
    // push/restore rather than clobber the outer call. `held` records that
    // the handler deferred the reply.
    struct PendingCall {
        QDBusMessage msg;
        QDBusConnection conn;
        QString member;
        bool held = false;
        QPointer<DBusHeldReply> reply = nullptr;
    };
    PendingCall m_currentCall;
    QStack<PendingCall> m_callStack;
    bool m_inDispatch = false;
    // RAII guard: pushes a fresh PendingCall for the duration of one
    // handler invocation, restoring the outer context on exit (nested
    // dispatches must not clobber the outer call's held flag/message).
    // Also owns the m_inDispatch flag save/restore.
    class DispatchScope;
}; // end DBusAdaptor

// Convert a QJSValue to QVariant for D-Bus marshaling (preserves DBus.*
// gadget types). Shared between the sync dispatch path and DBusHeldReply.
QVariant qjsValueToVariant(const QJSValue &jsval);

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

    Q_PROPERTY(QString service READ service WRITE setService NOTIFY serviceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(QString iface READ iface WRITE setIface NOTIFY ifaceChanged)
    Q_PROPERTY(
        DBusConnection *connection READ connection WRITE setConnection NOTIFY connectionChanged)
    Q_PROPERTY(QVariantMap _signatures READ signatures WRITE setSignatures NOTIFY signaturesChanged)
    // Explicit wire-surface declarations (the explicit tier of the naming
    // ladder): _signals maps wire signal name → concatenated arg signature;
    // _members maps wire member name → QML function/property name.
    Q_PROPERTY(QVariantMap _signals READ signalSpecs WRITE setSignalSpecs NOTIFY _signalsChanged)
    Q_PROPERTY(
        QVariantMap _members READ memberAliases WRITE setMemberAliases NOTIFY _membersChanged)

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
    void setSignalSpecs(const QVariantMap &v);

    QVariantMap memberAliases() const { return m_members; }
    void setMemberAliases(const QVariantMap &v);

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

private:
    friend class PropertiesChangedRelay;
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

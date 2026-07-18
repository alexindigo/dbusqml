#pragma once

#include <QVariantList>
#include <qqmlregistration.h>

class DBusMessage {
    Q_GADGET
    QML_VALUE_TYPE(dbusMessage)
    QML_CONSTRUCTIBLE_VALUE
    QML_STRUCTURED_VALUE

    Q_PROPERTY(QString service READ service WRITE setService)
    Q_PROPERTY(QString path READ path WRITE setPath)
    Q_PROPERTY(QString iface READ iface WRITE setIface)
    Q_PROPERTY(QString member READ member WRITE setMember)
    Q_PROPERTY(QVariantList arguments READ arguments WRITE setArguments)
    Q_PROPERTY(QString signature READ signature WRITE setSignature)

public:
    explicit DBusMessage() = default;
    Q_INVOKABLE explicit DBusMessage(const QVariantMap &data);

    QString service() const { return m_service; }
    void setService(const QString &v) { m_service = v; }

    QString path() const { return m_path; }
    void setPath(const QString &v) { m_path = v; }

    QString iface() const { return m_iface; }
    void setIface(const QString &v) { m_iface = v; }

    QString member() const { return m_member; }
    void setMember(const QString &v) { m_member = v; }

    QVariantList arguments() const { return m_arguments; }
    void setArguments(const QVariantList &v) { m_arguments = v; }

    QString signature() const { return m_signature; }
    void setSignature(const QString &v) { m_signature = v; }

private:
    QString m_service;
    QString m_path;
    QString m_iface;
    QString m_member;
    QVariantList m_arguments;
    QString m_signature;
};

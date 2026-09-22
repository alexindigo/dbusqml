#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <QtGlobal>
#include <QVariant>
#include <qqmlregistration.h>

class QDBusArgument;

// QML singleton providing D-Bus value conversion utilities.
// Primary use: converting ay (byte array, arrives as ArrayBuffer in QML)
// to/from text for common cases like NetworkManager SSIDs.
class DBusUtils : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(DBusUtils)
    QML_SINGLETON

public:
    explicit DBusUtils(QObject *parent = nullptr);

    // Decode an ArrayBuffer (or number-array) as UTF-8 text.
    // Handles the canonical ay representation (ArrayBuffer) and the
    // legacy number-array form for migration.
    Q_INVOKABLE QString textFromBytes(const QJSValue &bytes);

    // Encode a string as UTF-8 bytes, returned as an ArrayBuffer.
    // The result can be passed directly to ay-typed D-Bus arguments
    // (the marshaller also accepts plain strings, but this is explicit).
    Q_INVOKABLE QJSValue bytesFromText(const QString &text);

    // Build a D-Bus error value to THROW from a served method handler: the
    // caller receives an error reply carrying exactly this name and message.
    // Any other thrown value becomes org.freedesktop.DBus.Error.Failed.
    Q_INVOKABLE QVariantMap error(const QString &name, const QString &message);

    // ── fd quartet + fdUrl (0.9.0) ──
    //
    // Unix fd (h) I/O for QML — the ay-codec precedent: ay shipped with
    // textFromBytes/bytesFromText because QML lacked text codecs; h ships
    // with this quartet because QML lacks fd I/O. fds arrive from D-Bus
    // calls as plain ints (the walker's h demarshal) and these members make
    // them USABLE: read/write/close from QML, discharging the
    // receiver-closes contract from the script layer.
    //
    // openFd(path, mode) → int fd (−1 + warning on failure). mode is
    // fopen-style: "r" (read-only), "w" (write-only, create/truncate),
    // "rw" (read-write, create).
    //
    // writeFd(fd, ArrayBuffer|string) → int bytes written (−1 + warning).
    //
    // readFd(fd, maxBytes) → ArrayBuffer (empty on error/EOF).
    //
    // closeFd(fd) — discharges the receiver-closes contract from QML.
    //
    // fdUrl(fd) → "file:///proc/self/fd/N" — lets regular-file-backed fds
    // flow into path-based QML consumers (Image, Quickshell FileView, …).
    // CAVEATS (documented in API.md): works for regular-file-backed fds
    // only — pipes/sockets/anon inodes (the ScreenShot2 pipe and
    // OpenPipeWireRemote socket classes) have no usable path; use
    // readFd/writeFd for streams. The URL is valid only while the fd stays
    // open in this process — lazy/async loaders must not outlive it.
    Q_INVOKABLE int openFd(const QString &path, const QString &mode);
    Q_INVOKABLE int writeFd(int fd, const QJSValue &data);
    Q_INVOKABLE QJSValue readFd(int fd, int maxBytes);
    Q_INVOKABLE void closeFd(int fd);
    Q_INVOKABLE QString fdUrl(int fd);

private:
    QByteArray jsToBytes(const QJSValue &data);
    QJSValue makeArrayBuffer(const QByteArray &bytes);
};

// Library-internal (not QML API). Signature-driven demarshaller; never
// operator>>(QDBusArgument, QVariant) inside a container.
QVariant readBySignature(const QDBusArgument &arg, int depth = 0);

inline void checkedSend(const QDBusConnection &c, const QDBusMessage &m, const char *what,
                        const QString &member = {}) {
    if (!c.send(m))
        qWarning("dbusqml: %s%s%s failed to send: %s", what, member.isEmpty() ? "" : " for ",
                 qPrintable(member), qPrintable(c.lastError().message()));
}

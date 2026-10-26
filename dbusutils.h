#pragma once

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QJSValue>
#include <QObject>
#include <QQmlEngine>
#include <QtGlobal>
#include <QVariant>
#include <qqmlregistration.h>

#include "dbustypes.h"

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

    // ── fd helpers over DBusQML.unixFd ──
    //
    // Unix fd (h) I/O for QML — the ay-codec precedent: QML lacks fd I/O.
    // Every fd these take or return is a DBusQML.unixFd (the owning value
    // D-Bus also sends and delivers); a plain number is refused, never
    // coerced to a descriptor.
    //
    // openFd(path, mode) → unixFd (valid === false + warning on failure).
    // mode is fopen-style: "r" (read-only), "w" (write-only,
    // create/truncate), "rw" (read-write, create).
    //
    // writeFd(unixFd, ArrayBuffer|string) → int bytes written (−1 + warning).
    //
    // readFd(unixFd, maxBytes) → ArrayBuffer (empty on error/EOF).
    //
    // fdUrl(unixFd) → "file:///proc/self/fd/N" — lets regular-file-backed
    // fds flow into path-based QML consumers (Image, Quickshell FileView,
    // …). CAVEATS (documented in API.md): regular-file-backed fds only —
    // pipes/sockets/anon inodes have no usable path; use readFd/writeFd for
    // streams. The URL is valid only while the unixFd stays open.
    //
    // closeFd(number) — closes a caller-owned number from unixFd.take().
    // Refuses 0-2 (stdin/stdout/stderr) and unixFd values (use close()).
    Q_INVOKABLE DBus::UnixFd openFd(const QString &path, const QString &mode);
    Q_INVOKABLE int writeFd(const QVariant &fd, const QJSValue &data);
    Q_INVOKABLE QJSValue readFd(const QVariant &fd, int maxBytes);
    Q_INVOKABLE void closeFd(const QJSValue &fd);
    Q_INVOKABLE QString fdUrl(const QVariant &fd);

private:
    QByteArray jsToBytes(const QJSValue &data);
    QJSValue makeArrayBuffer(const QByteArray &bytes);
};

// Library-internal (not QML API). The same failure state crosses every
// recursive reader; a failed read never yields a partial container.
struct DecodeState {
    bool ok = true;
    const int fdLimit = DBus::fdDecodeBudget();
    int remainingFds = fdLimit;
    QString fdFailure;
    QList<DBus::UnixFd> receivedFds;

    DecodeState() = default;
    DecodeState(const DecodeState &) = delete;
    DecodeState &operator=(const DecodeState &) = delete;
    ~DecodeState() {
        // Also covers standalone unwrap/Struct readers and non-fd failures
        // after an fd was read. Copies already placed in containers close too.
        if (!ok)
            rollbackFds();
    }
    void rollbackFds() {
        for (auto &fd : receivedFds)
            fd.close();
        receivedFds.clear();
    }
};
struct DecodeDepth {
    int arrays = 0;
    int structs = 0;
    int total = 0;
};
QVariant readBySignature(const QDBusArgument &arg, DecodeState &state, DecodeDepth depth = {});
QVariant unwrapDbus(const QVariant &value, DecodeState &state, DecodeDepth depth = {});

// Atomic message boundary shared by replies, signals and adaptor dispatch.
// On failure, args is empty. Signals warn once here; callers of replies/calls
// receive Failed/InvalidArgs respectively, without a recursive warning storm.
bool decodeMessage(const QDBusMessage &message, QVariantList &args, QDBusError *error = nullptr);

// #10: peer-supplied text (a member name from introspection XML, an incoming
// message's member, a reply's error text, a sender name, …) can carry control
// characters — an XML character reference puts a real newline into a name —
// that a text log renders as extra forged lines. Escape every non-printable
// character before printing, plus the Unicode line/paragraph separators
// (Qt's isPrint accepts them, but they render as line breaks); printable
// Unicode passes through unchanged.
inline QString escapeForLog(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (auto it = text.cbegin(); it != text.cend();) {
        char32_t u;
        if (it->isHighSurrogate() && it + 1 != text.cend() && (it + 1)->isLowSurrogate()) {
            u = QChar::surrogateToUcs4(*it, *(it + 1));
            it += 2;
        } else {
            u = it->unicode();
            ++it;
        }
        switch (u) {
        case 0x07:
            out += QStringLiteral("\\a");
            continue;
        case 0x08:
            out += QStringLiteral("\\b");
            continue;
        case 0x09:
            out += QStringLiteral("\\t");
            continue;
        case 0x0A:
            out += QStringLiteral("\\n");
            continue;
        case 0x0B:
            out += QStringLiteral("\\v");
            continue;
        case 0x0C:
            out += QStringLiteral("\\f");
            continue;
        case 0x0D:
            out += QStringLiteral("\\r");
            continue;
        default:
            break;
        }
        const QChar::Category cat = QChar::category(u);
        if (QChar::isPrint(u) && cat != QChar::Separator_Line &&
            cat != QChar::Separator_Paragraph) {
            if (u > 0xFFFF) {
                out += QChar(QChar::highSurrogate(u));
                out += QChar(QChar::lowSurrogate(u));
            } else {
                out += QChar(char16_t(u));
            }
        } else if (u <= 0xFF) {
            out += QStringLiteral("\\x%1").arg(uint(u), 2, 16, QLatin1Char('0'));
        } else {
            out += QStringLiteral("\\u%1").arg(uint(u), 4, 16, QLatin1Char('0'));
        }
    }
    return out;
}

inline void checkedSend(const QDBusConnection &c, const QDBusMessage &m, const char *what,
                        const QString &member = {}) {
    if (!c.send(m))
        qWarning("dbusqml: %s%s%s failed to send: %s", what, member.isEmpty() ? "" : " for ",
                 qPrintable(escapeForLog(member)), qPrintable(c.lastError().message()));
}

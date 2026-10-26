#include "dbusutils.h"
#include "dbusconnection.h"
#include "dbustypes.h"

#include <QJSEngine>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusSignature>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

DBusUtils::DBusUtils(QObject *parent) : QObject(parent) {}

QString DBusUtils::textFromBytes(const QJSValue &bytes) {
    if (bytes.isString()) {
        // Legacy path — some code paths may still surface ay as a string
        // of char codes. Convert back to bytes first.
        QString s = bytes.toString();
        QByteArray ba(s.length(), Qt::Uninitialized);
        for (int i = 0; i < s.length(); ++i)
            ba[i] = static_cast<char>(s.at(i).unicode() & 0xFF);
        return QString::fromUtf8(ba);
    }

    // ArrayBuffer or array-like — extract bytes via JS
    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return {};

    engine->globalObject().setProperty(QStringLiteral("__dbus_bytes"), bytes);
    QJSValue result = engine->evaluate(QStringLiteral(
        "(function() {"
        "  var buf = __dbus_bytes;"
        "  if (buf instanceof ArrayBuffer) {"
        "    return new Uint8Array(buf);"
        "  }"
        "  if (typeof buf.length === 'number') {"
        "    var arr = new Uint8Array(buf.length);"
        "    for (var i = 0; i < buf.length; ++i)"
        "      arr[i] = typeof buf[i] === 'number' ? buf[i] : (buf[i] ? buf[i].charCodeAt(0) : 0);"
        "    return arr;"
        "  }"
        "  return new Uint8Array(0);"
        "})()"));

    if (result.isError()) {
        qWarning("DBusUtils::textFromBytes: JS evaluation failed: %s",
                 qPrintable(result.toString()));
        return {};
    }

    // Convert Uint8Array to QByteArray via JS-side string of char codes
    QJSValue lengthProp = result.property(QStringLiteral("length"));
    if (!lengthProp.isNumber())
        return {};

    int len = lengthProp.toInt();
    QByteArray ba(len, Qt::Uninitialized);
    for (int i = 0; i < len; ++i)
        ba[i] = static_cast<char>(result.property(static_cast<quint32>(i)).toInt());

    engine->globalObject().deleteProperty(QStringLiteral("__dbus_bytes"));
    return QString::fromUtf8(ba);
}

QJSValue DBusUtils::bytesFromText(const QString &text) {
    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return {};

    QByteArray utf8 = text.toUtf8();
    QJSValue buf = engine->evaluate(QStringLiteral("new ArrayBuffer(%1)").arg(utf8.size()));
    QJSValue view = engine->evaluate(QStringLiteral("new Uint8Array(__dbus_target)"));

    // Store the buffer so the view can reference it
    engine->globalObject().setProperty(QStringLiteral("__dbus_target"), buf);
    view = engine->evaluate(QStringLiteral("new Uint8Array(__dbus_target)"));

    for (int i = 0; i < utf8.size(); ++i)
        view.setProperty(static_cast<quint32>(i), static_cast<int>(static_cast<uchar>(utf8[i])));

    engine->globalObject().deleteProperty(QStringLiteral("__dbus_target"));
    return buf;
}

QVariantMap DBusUtils::error(const QString &name, const QString &message) {
    return QVariantMap{{QStringLiteral("dbusError"), true},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("message"), message}};
}

// Extract raw bytes from a QJSValue: string → UTF-8; ArrayBuffer → its
// bytes; array-like → per-element (the textFromBytes idiom, but lossless).
QByteArray DBusUtils::jsToBytes(const QJSValue &data) {
    if (data.isString())
        return data.toString().toUtf8();

    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return {};

    engine->globalObject().setProperty(QStringLiteral("__dbus_fd_data"), data);
    QJSValue result = engine->evaluate(QStringLiteral(
        "(function() {"
        "  var buf = __dbus_fd_data;"
        "  if (buf instanceof ArrayBuffer) return new Uint8Array(buf);"
        "  if (typeof buf.length === 'number') {"
        "    var arr = new Uint8Array(buf.length);"
        "    for (var i = 0; i < buf.length; ++i)"
        "      arr[i] = typeof buf[i] === 'number' ? buf[i] : (buf[i] ? buf[i].charCodeAt(0) : 0);"
        "    return arr;"
        "  }"
        "  return new Uint8Array(0);"
        "})()"));
    engine->globalObject().deleteProperty(QStringLiteral("__dbus_fd_data"));

    if (result.isError()) {
        qWarning("DBusUtils::writeFd: JS evaluation failed: %s", qPrintable(result.toString()));
        return {};
    }

    const QJSValue lengthProp = result.property(QStringLiteral("length"));
    if (!lengthProp.isNumber())
        return {};

    const int len = lengthProp.toInt();
    QByteArray ba(len, Qt::Uninitialized);
    for (int i = 0; i < len; ++i)
        ba[i] = static_cast<char>(result.property(static_cast<quint32>(i)).toInt());
    return ba;
}

QJSValue DBusUtils::makeArrayBuffer(const QByteArray &bytes) {
    QJSEngine *engine = qjsEngine(this);
    if (!engine)
        return {};

    QJSValue buf = engine->evaluate(QStringLiteral("new ArrayBuffer(%1)").arg(bytes.size()));
    engine->globalObject().setProperty(QStringLiteral("__dbus_fd_buf"), buf);
    QJSValue view = engine->evaluate(QStringLiteral("new Uint8Array(__dbus_fd_buf)"));
    for (int i = 0; i < bytes.size(); ++i)
        view.setProperty(static_cast<quint32>(i), static_cast<int>(static_cast<uchar>(bytes[i])));
    engine->globalObject().deleteProperty(QStringLiteral("__dbus_fd_buf"));
    return buf;
}

// The one rule for the helpers' fd argument: a valid DBusQML.unixFd, never a
// number (a number is not proof of ownership — 3 might be anything, 0 is
// stdin). Returns the borrowed descriptor, or -1 with a warning.
static int helperFd(const char *fn, const QVariant &fd) {
    if (fd.userType() != qMetaTypeId<DBus::UnixFd>()) {
        qWarning("DBusUtils::%s: expects a DBusQML.unixFd, got %s", fn,
                 fd.isValid() ? fd.metaType().name() : "undefined");
        return -1;
    }
    const DBus::UnixFd unixFd = fd.value<DBus::UnixFd>();
    if (!unixFd.isValid()) {
        qWarning("DBusUtils::%s: the DBusQML.unixFd is not valid (closed)", fn);
        return -1;
    }
    return unixFd.fileDescriptor();
}

DBus::UnixFd DBusUtils::openFd(const QString &path, const QString &mode) {
    int flags = -1;
    if (mode == QLatin1String("r"))
        flags = O_RDONLY;
    else if (mode == QLatin1String("w"))
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode == QLatin1String("rw"))
        flags = O_RDWR | O_CREAT;

    if (flags < 0) {
        qWarning("DBusUtils::openFd: unknown mode '%s' for %s — use r, w or rw", qPrintable(mode),
                 qPrintable(path));
        return DBus::UnixFd();
    }

    const int fd = ::open(path.toLocal8Bit().constData(), flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        qWarning("DBusUtils::openFd: failed to open %s (mode %s): %s", qPrintable(path),
                 qPrintable(mode), std::strerror(errno));
        return DBus::UnixFd();
    }
    DBus::UnixFd owned(fd); // dups; the wrapper owns its copy
    ::close(fd);
    if (QJSEngine *engine = qjsEngine(this))
        owned.noteDelivered(engine);
    return owned;
}

int DBusUtils::writeFd(const QVariant &fdValue, const QJSValue &data) {
    const int fd = helperFd("writeFd", fdValue);
    if (fd < 0)
        return -1;
    const QByteArray bytes = jsToBytes(data);
    const ssize_t n = ::write(fd, bytes.constData(), size_t(bytes.size()));
    if (n < 0) {
        qWarning("DBusUtils::writeFd: write failed: %s", std::strerror(errno));
        return -1;
    }
    return int(n);
}

QJSValue DBusUtils::readFd(const QVariant &fdValue, int maxBytes) {
    const int fd = helperFd("readFd", fdValue);
    if (fd < 0 || maxBytes <= 0)
        return makeArrayBuffer({});

    QByteArray bytes(maxBytes, Qt::Uninitialized);
    const ssize_t n = ::read(fd, bytes.data(), size_t(maxBytes));
    if (n < 0) {
        qWarning("DBusUtils::readFd: read failed: %s", std::strerror(errno));
        return makeArrayBuffer({});
    }
    bytes.resize(int(n));
    return makeArrayBuffer(bytes);
}

void DBusUtils::closeFd(const QJSValue &fdValue) {
    // Only for a number unixFd.take() handed out. A unixFd closes itself;
    // 0-2 are the process's stdio, never a take()n number (take() dups to
    // >= 3) — refusing them keeps a stray 0 (e.g. Number(unixFd) → NaN → 0)
    // from closing stdin.
    if (fdValue.toVariant().userType() == qMetaTypeId<DBus::UnixFd>()) {
        qWarning("DBusUtils::closeFd: refusing a DBusQML.unixFd — call its close()");
        return;
    }
    const double n = fdValue.isNumber() ? fdValue.toNumber() : -1.0;
    if (!(n >= 0 && n <= INT_MAX && std::floor(n) == n)) {
        qWarning("DBusUtils::closeFd: expects a number from unixFd.take(), got %s",
                 qPrintable(fdValue.toString()));
        return;
    }
    const int fd = static_cast<int>(n);
    if (fd <= 2) {
        qWarning("DBusUtils::closeFd: refusing to close fd %d (stdin/stdout/stderr)", fd);
        return;
    }
    if (::close(fd) != 0)
        qWarning("DBusUtils::closeFd: close on fd %d failed: %s", fd, std::strerror(errno));
}

QString DBusUtils::fdUrl(const QVariant &fdValue) {
    const int fd = helperFd("fdUrl", fdValue);
    if (fd < 0)
        return {};
    return QStringLiteral("file:///proc/self/fd/%1").arg(fd);
}

// D-Bus specification, "Valid Signatures" and "Container types";
// libdbus dbus-protocol.h: DBUS_MAXIMUM_TYPE_RECURSION_DEPTH == 32.
// dbus-marshal-validate.c validate_body_helper caps dynamic nesting (including
// variants and dict entries) at twice that value. A variant starts a new
// signature, but does not reset the body's total depth.
static bool enterDecodeContainer(DecodeState &state, DecodeDepth &depth, QChar kind) {
    constexpr int maximumTypeRecursionDepth = 32;
    ++depth.total;
    if (kind == QLatin1Char('a'))
        ++depth.arrays;
    else if (kind == QLatin1Char('('))
        ++depth.structs;
    else if (kind == QLatin1Char('v'))
        depth.arrays = depth.structs = 0;
    state.ok = state.ok && depth.arrays <= maximumTypeRecursionDepth &&
               depth.structs <= maximumTypeRecursionDepth &&
               depth.total <= 2 * maximumTypeRecursionDepth;
    return state.ok;
}

static QVariant failReceivedFd(DecodeState &state, const QString &reason) {
    state.ok = false;
    state.fdFailure = reason;
    state.rollbackFds();
    return {};
}

static bool canReceiveFd(DecodeState &state) {
    if (!state.ok)
        return false;
    if (state.remainingFds == 0) {
        failReceivedFd(state, QStringLiteral("fd-reference budget exceeded"));
        return false;
    }
    return true;
}

// The single fd-reference accounting/ownership boundary, including invalid
// references (P2). The argument must have a fresh, independent Qt holder.
static QVariant takeReceivedFd(DecodeState &state, QDBusUnixFileDescriptor &&fd) {
    if (!canReceiveFd(state))
        return {};
    --state.remainingFds;
    auto owned = DBus::UnixFd::adoptReceivedDescriptor(std::move(fd));
    state.receivedFds.append(owned);
    return QVariant::fromValue(owned);
}

static QVariant readReceivedFd(const QDBusArgument &arg, DecodeState &state) {
    // Refuse before asking libdbus to allocate even a temporary descriptor.
    if (!canReceiveFd(state))
        return {};
    QDBusUnixFileDescriptor fd;
    // libdbus get_basic returns -1 both for an invalid index (no syscall)
    // and for a failed _dbus_dup (errno set). Capture it before any other
    // operations, preserving invalid -> valid:false without hiding EMFILE.
    errno = 0;
    arg >> fd;
    const int duplicateError = errno;
    if (!fd.isValid() && duplicateError)
        return failReceivedFd(state, QStringLiteral("received fd duplication failed"));
    // Qt 6.8.2/6.11.2: operator>> assigns toUnixFileDescriptor(), whose
    // local giveFileDescriptor(qIterGet()) owns libdbus's fresh dup. Its
    // temporary is gone on return; neither message nor iterator shares it.
    return takeReceivedFd(state, std::move(fd));
}

QVariant readBySignature(const QDBusArgument &arg, DecodeState &state, DecodeDepth depth) {
    if (!state.ok)
        return {};
    const QString sig = arg.currentSignature();
    if (!sig.isEmpty() &&
        (sig[0] == QLatin1Char('a') || sig[0] == QLatin1Char('(') || sig[0] == QLatin1Char('v')) &&
        !enterDecodeContainer(state, depth, sig[0]))
        return {};

    // Basic types — single char signatures
    if (sig == "y") {
        uchar v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "b") {
        bool v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "n") {
        short v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "q") {
        ushort v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "i") {
        int v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "u") {
        uint v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "x") {
        qint64 v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "t") {
        quint64 v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "d") {
        double v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "s") {
        QString v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "o") {
        QDBusObjectPath v;
        arg >> v;
        return QVariant::fromValue(v.path());
    }
    if (sig == "g") {
        QDBusSignature v;
        arg >> v;
        return QVariant::fromValue(v.signature());
    }
    if (sig == "v") {
        QDBusVariant v;
        errno = 0;
        arg >> v;
        const int duplicateError = errno;
        // Qt eagerly reads scalar variant payloads (including nested v's).
        // A failed libdbus dup here must not become a P2 invalid fd when
        // unwrapDbus subsequently sees the already-demarshalled carrier.
        if (duplicateError) {
            QVariant leaf = v.variant();
            while (leaf.userType() == qMetaTypeId<QDBusVariant>())
                leaf = leaf.value<QDBusVariant>().variant();
            if (leaf.userType() == qMetaTypeId<QDBusUnixFileDescriptor>() &&
                !leaf.value<QDBusUnixFileDescriptor>().isValid())
                return failReceivedFd(state, QStringLiteral("received fd duplication failed"));
        }
        return unwrapDbus(v.variant(), state, depth);
    }
    if (sig == "ay") {
        QByteArray v;
        arg >> v;
        return QVariant::fromValue(v);
    }
    if (sig == "as") {
        QStringList v;
        arg >> v;
        return QVariant::fromValue(v);
    }

    // Arrays of basic types — ai, au, ad, ab, an, aq, at, ax, ag.
    // Concrete-typed arrays can't be element-iterated with an untyped
    // QVariant target (crashes inside libdbus). Read via the concrete
    // QList<T> and flatten to QVariantList.
    if (sig.startsWith(QLatin1Char('a')) && sig.length() == 2) {
        const QChar elemType = sig.at(1);
        if (elemType == QLatin1Char('y')) {
            QList<uchar> l;
            arg >> l;
            QVariantList out;
            for (uchar v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('b')) {
            QList<bool> l;
            arg >> l;
            QVariantList out;
            for (bool v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('n')) {
            QList<short> l;
            arg >> l;
            QVariantList out;
            for (short v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('q')) {
            QList<ushort> l;
            arg >> l;
            QVariantList out;
            for (ushort v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('h')) {
            QVariantList out;
            arg.beginArray();
            while (state.ok && !arg.atEnd()) {
                const QVariant fd = readReceivedFd(arg, state);
                if (state.ok)
                    out.append(fd);
            }
            arg.endArray();
            return state.ok ? QVariant(out) : QVariant();
        }
        if (elemType == QLatin1Char('i')) {
            QList<int> l;
            arg >> l;
            QVariantList out;
            for (int v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('u')) {
            QList<uint> l;
            arg >> l;
            QVariantList out;
            for (uint v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('x')) {
            QList<qint64> l;
            arg >> l;
            QVariantList out;
            for (qint64 v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('t')) {
            QList<quint64> l;
            arg >> l;
            QVariantList out;
            for (quint64 v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('d')) {
            QList<double> l;
            arg >> l;
            QVariantList out;
            for (double v : l)
                out.append(v);
            return out;
        }
        if (elemType == QLatin1Char('g')) {
            QList<QDBusSignature> l;
            arg >> l;
            QVariantList out;
            for (const QDBusSignature &v : l)
                out.append(v.signature());
            return out;
        }
    }

    // Containers — recursive, signature-driven. The caller has already
    // opened the container (beginStructure / beginMap / beginMapEntry /
    // beginArray) and we're positioned at one complete element. For a
    // nested container we open it, recurse per member/element/entry, and
    // close it. Never touches operator>>(QDBusArgument, QVariant&).
    if (sig.startsWith(QStringLiteral("a{"))) {
        // Array of dict entries — a{KV}. Read entries one by one.
        // Every begin* pairs with its end* even on failure: QDBusArgument
        // frees the parser state a begin* allocates only in the matching
        // end* — an early return without it leaks (CI ASan gate).
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            DecodeDepth entryDepth = depth;
            if (!enterDecodeContainer(state, entryDepth, QLatin1Char('{'))) {
                arg.endMap();
                return {};
            }
            arg.beginMapEntry();
            const QVariant key = readBySignature(arg, state, entryDepth);
            if (!state.ok) {
                arg.endMapEntry();
                arg.endMap();
                return {};
            }
            const QVariant value = readBySignature(arg, state, entryDepth);
            if (!state.ok) {
                arg.endMapEntry();
                arg.endMap();
                return {};
            }
            map.insert(key.toString(), value);
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }
    if (sig == QLatin1String("ao")) {
        QList<QDBusObjectPath> paths;
        arg >> paths;
        QVariantList out;
        out.reserve(paths.size());
        for (const auto &p : paths)
            out.append(p.path());
        return out;
    }
    if (sig.startsWith(QStringLiteral("aa")) || sig.startsWith(QStringLiteral("a(")) ||
        sig == QLatin1String("av")) {
        // One recursive array walk for structs, arrays and variants. It is
        // shared by top-level and nested positions, with no depth reset.
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd()) {
            const QVariant value = readBySignature(arg, state, depth);
            if (!state.ok) {
                arg.endArray();
                return {};
            }
            out.append(value);
        }
        arg.endArray();
        return out;
    }
    if (sig.startsWith(QStringLiteral("("))) {
        // Struct / tuple — (...). Members read per-signature.
        QVariantList members;
        arg.beginStructure();
        while (!arg.atEnd()) {
            const QVariant value = readBySignature(arg, state, depth);
            if (!state.ok) {
                arg.endStructure();
                return {};
            }
            members.append(value);
        }
        arg.endStructure();
        return members;
    }

    if (sig == QLatin1String("h"))
        return readReceivedFd(arg, state);

    // Never retry an unconsumed value. The message boundary owns reporting.
    state.ok = false;
    return {};
}

QVariant unwrapDbus(const QVariant &v, DecodeState &state, DecodeDepth depth) {
    if (!state.ok)
        return {};
    const int type = v.userType();
    if (type == qMetaTypeId<QDBusArgument>())
        return readBySignature(v.value<QDBusArgument>(), state, depth);
    if (type == qMetaTypeId<QDBusVariant>()) {
        if (!enterDecodeContainer(state, depth, QLatin1Char('v')))
            return {};
        return unwrapDbus(v.value<QDBusVariant>().variant(), state, depth);
    }
    if (type == qMetaTypeId<QVariantMap>() || type == qMetaTypeId<QVariantList>()) {
        if (!enterDecodeContainer(state, depth, QLatin1Char('a')))
            return {};
        if (type == qMetaTypeId<QVariantMap>()) {
            const auto map = v.toMap();
            QVariantMap result;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                // One dict-entry container per entry; the value (always a
                // variant on the wire) counts itself in the recursive call.
                DecodeDepth child = depth;
                if (!enterDecodeContainer(state, child, QLatin1Char('{')))
                    return {};
                const QVariant value = unwrapDbus(it.value(), state, child);
                if (!state.ok)
                    return {};
                result.insert(it.key(), value);
            }
            return result;
        }
        QVariantList result;
        for (const auto &item : v.toList()) {
            const QVariant value = unwrapDbus(item, state, depth);
            if (!state.ok)
                return {};
            result.append(value);
        }
        return result;
    }
    // Preserve native scalar carriers for C++ consumers, as before. The
    // signature reader still normalizes o/g inside raw container carriers.
    if (type == qMetaTypeId<QDBusUnixFileDescriptor>()) {
        if (!canReceiveFd(state))
            return {};
        // Native message/variant carriers can be shared with another listener.
        // Never take() their descriptor, even from a temporary value<T>().
        const auto source = v.value<QDBusUnixFileDescriptor>();
        QDBusUnixFileDescriptor fd;
        if (source.isValid()) {
            fd.setFileDescriptor(source.fileDescriptor());
            if (!fd.isValid())
                return failReceivedFd(state, QStringLiteral("received fd duplication failed"));
        }
        return takeReceivedFd(state, std::move(fd));
    }
    if ((type == QMetaType::QStringList || type == QMetaType::QByteArray) &&
        !enterDecodeContainer(state, depth, QLatin1Char('a')))
        return {};
    if (!v.isValid())
        state.ok = false;
    return state.ok ? v : QVariant();
}

bool decodeMessage(const QDBusMessage &message, QVariantList &args, QDBusError *error) {
    args.clear();
    if (error)
        *error = QDBusError();
    DecodeState state;
    QVariantList decoded;
    for (const auto &arg : message.arguments()) {
        const QVariant value = unwrapDbus(arg, state);
        if (!state.ok) {
            state.rollbackFds();
            if (!state.fdFailure.isEmpty())
                qWarning("dbusqml: message '%s' could not be decoded: %s (fd-reference limit %d)",
                         qPrintable(escapeForLog(message.member())), qPrintable(state.fdFailure),
                         state.fdLimit);
            else if (message.type() == QDBusMessage::SignalMessage)
                qWarning("dbusqml: signal could not be decoded — dropping message");
            if (message.type() != QDBusMessage::SignalMessage && error)
                *error = QDBusError(message.type() == QDBusMessage::MethodCallMessage
                                        ? QDBusError::InvalidArgs
                                        : QDBusError::Failed,
                                    message.type() == QDBusMessage::MethodCallMessage
                                        ? QStringLiteral("arguments could not be decoded")
                                        : QStringLiteral("reply could not be decoded"));
            return false;
        }
        decoded.append(value);
    }
    args = std::move(decoded);
    return true;
}

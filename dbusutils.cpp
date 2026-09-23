#include "dbusutils.h"
#include "dbusconnection.h"

#include <QJSEngine>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusSignature>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <cerrno>
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

int DBusUtils::openFd(const QString &path, const QString &mode) {
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
        return -1;
    }

    const int fd = ::open(path.toLocal8Bit().constData(), flags, 0644);
    if (fd < 0)
        qWarning("DBusUtils::openFd: failed to open %s (mode %s): %s", qPrintable(path),
                 qPrintable(mode), std::strerror(errno));
    return fd;
}

int DBusUtils::writeFd(int fd, const QJSValue &data) {
    if (fd < 0) {
        qWarning("DBusUtils::writeFd: invalid fd %d", fd);
        return -1;
    }
    const QByteArray bytes = jsToBytes(data);
    const ssize_t n = ::write(fd, bytes.constData(), size_t(bytes.size()));
    if (n < 0) {
        qWarning("DBusUtils::writeFd: write on fd %d failed: %s", fd, std::strerror(errno));
        return -1;
    }
    return int(n);
}

QJSValue DBusUtils::readFd(int fd, int maxBytes) {
    if (fd < 0) {
        qWarning("DBusUtils::readFd: invalid fd %d", fd);
        return makeArrayBuffer({});
    }
    if (maxBytes <= 0)
        return makeArrayBuffer({});

    QByteArray bytes(maxBytes, Qt::Uninitialized);
    const ssize_t n = ::read(fd, bytes.data(), size_t(maxBytes));
    if (n < 0) {
        qWarning("DBusUtils::readFd: read on fd %d failed: %s", fd, std::strerror(errno));
        return makeArrayBuffer({});
    }
    bytes.resize(int(n));
    return makeArrayBuffer(bytes);
}

void DBusUtils::closeFd(int fd) {
    if (fd < 0) {
        qWarning("DBusUtils::closeFd: invalid fd %d", fd);
        return;
    }
    if (::close(fd) != 0)
        qWarning("DBusUtils::closeFd: close on fd %d failed: %s", fd, std::strerror(errno));
}

QString DBusUtils::fdUrl(int fd) {
    if (fd < 0) {
        qWarning("DBusUtils::fdUrl: invalid fd %d", fd);
        return {};
    }
    return QStringLiteral("file:///proc/self/fd/%1").arg(fd);
}

QVariant readBySignature(const QDBusArgument &arg, int depth) {
    // B1: recursion depth cap 32 (Nemo precedent) — remote signature data
    // flows into these walkers verbatim; unbounded recursion would exhaust
    // the stack. Over the cap: warn + loud fail (never silent).
    if (depth > 32) {
        qWarning("dbusqml: readBySignature: recursion depth cap (32) exceeded — failing loud");
        return {};
    }
    const QString sig = arg.currentSignature();

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
        arg >> v;
        return unwrapDbus(v.variant());
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
            // Unix fds — delivered as plain ints (receiver closes).
            QVariantList out;
            arg.beginArray();
            while (!arg.atEnd()) {
                QDBusUnixFileDescriptor fd;
                arg >> fd;
                out.append(fd.isValid() ? fd.takeFileDescriptor() : -1);
            }
            arg.endArray();
            return out;
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
        QVariantMap map;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QVariant key = readBySignature(arg, depth + 1);
            QVariant value = readBySignature(arg, depth + 1);
            map.insert(key.toString(), unwrapDbus(value));
            arg.endMapEntry();
        }
        arg.endMap();
        return map;
    }
    if (sig.startsWith(QStringLiteral("a(")) || sig.startsWith(QStringLiteral("a["))) {
        // Array of structs — a(...). Elements are structs.
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd())
            out.append(readBySignature(arg, depth + 1));
        arg.endArray();
        return out;
    }
    // B2: nested av/ao — the top-level twin (unwrapDbus) handles these; the
    // nested positions ((sav), a{sav}, a{sao}, …) previously fell through to
    // the unsupported-signature warning and DROPPED the element.
    if (sig == QLatin1String("av")) {
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd()) {
            QVariant elem;
            arg >> elem;
            out.append(unwrapDbus(elem));
        }
        arg.endArray();
        return out;
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
    if (sig.startsWith(QStringLiteral("aa"))) {
        // Array of arrays — aa* (including aa{...}). Elements are arrays.
        QVariantList out;
        arg.beginArray();
        while (!arg.atEnd())
            out.append(readBySignature(arg, depth + 1));
        arg.endArray();
        return out;
    }
    if (sig.startsWith(QStringLiteral("("))) {
        // Struct / tuple — (...). Members read per-signature.
        QVariantList members;
        arg.beginStructure();
        while (!arg.atEnd())
            members.append(readBySignature(arg, depth + 1));
        arg.endStructure();
        return members;
    }

    if (sig == QLatin1String("h")) {
        // Unix fd — delivered as a plain int (dbus-next + Nemo double
        // precedent). The RECEIVER closes the fd. CF-25: invalid fds are
        // -1 here AND in the ah-array path (was: invalid QVariant / JS
        // undefined here) — one sentinel, branch-testable in JS either
        // way (D4, owner-vetoable at V1).
        QDBusUnixFileDescriptor fd;
        arg >> fd;
        if (!fd.isValid())
            return QVariant::fromValue(-1);
        return QVariant::fromValue(fd.takeFileDescriptor());
    }

    // Unrecognized signature — make the gap visible in logs.
    qWarning("DBus: readBySignature: unsupported signature %s", qPrintable(sig));
    return {};
}

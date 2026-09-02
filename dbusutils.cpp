#include "dbusutils.h"

#include <QJSEngine>
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

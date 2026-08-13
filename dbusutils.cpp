#include "dbusutils.h"

#include <QJSEngine>

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

// Payload echo service (fix-parity phase 6): replies to Echo(sig, values)
// with a WRITE-built payload of the requested signature (writeBySignature).
// Running as a separate process guarantees the reply crosses the real wire —
// the caller receives a READ-mode QDBusArgument, exactly what the
// nested-position walker tests need (write-built args are write-only).
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusVirtualObject>
#include <fcntl.h>
#include <unistd.h>

#include "dbusconnection.h"
#include "deep_payload.h"

class PayloadEchoObject : public QDBusVirtualObject {
public:
    explicit PayloadEchoObject(QObject *parent = nullptr) : QDBusVirtualObject(parent) {}

    QString introspect(const QString &) const override {
        return QStringLiteral("<node><interface name=\"org.dbusqml.PayloadEcho\">"
                              "<method name=\"Echo\"><arg type=\"s\" direction=\"in\"/>"
                              "<arg type=\"av\" direction=\"in\"/></method>"
                              "</interface></node>");
    }

    bool handleMessage(const QDBusMessage &msg, const QDBusConnection &conn) override {
        if (msg.member() == QLatin1String("RoundTrip")) {
            lastArguments = msg.arguments();
            conn.send(msg.createReply(lastArguments));
            return true;
        }
        if (msg.member() == QLatin1String("LastArguments")) {
            conn.send(msg.createReply(lastArguments));
            return true;
        }
        if (msg.member() == QLatin1String("Deep33")) {
            conn.send(msg.createReply({QVariant::fromValue(DeepPayload<1, 32>{})}));
            return true;
        }
        if (msg.member() == QLatin1String("Deep64")) {
            conn.send(msg.createReply({QVariant::fromValue(DeepPayload<32, 32>{})}));
            return true;
        }
        if (msg.member() == QLatin1String("DeepStruct64")) {
            QDBusArgument payload;
            payload.beginStructure();
            payload << DeepPayload<32, 31>{};
            payload.endStructure();
            conn.send(msg.createReply({QVariant::fromValue(payload)}));
            return true;
        }
        if (msg.member() != QLatin1String("Echo") || msg.arguments().size() < 2)
            return false;
        const QString sig = msg.arguments().at(0).toString();
        const QVariant decoded = unwrapDbus(msg.arguments().at(1));
        if (!decoded.isValid()) {
            conn.send(msg.createErrorReply(QStringLiteral("org.dbusqml.Error.BadPayload"),
                                           QStringLiteral("cannot decode payload")));
            return true;
        }
        const QVariantList values = decoded.toList();
        QVariant payload = writeBySignature(sig, values.size() == 1 ? values.first() : values);
        if (!payload.isValid()) {
            conn.send(msg.createErrorReply(QStringLiteral("org.dbusqml.Error.BadPayload"),
                                           QStringLiteral("cannot produce signature")));
            return true;
        }
        conn.send(msg.createReply({payload}));
        return true;
    }

private:
    QVariantList lastArguments;
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    registerDeepPayload<1, 32>();
    registerDeepPayload<32, 32>();
    registerDeepPayload<32, 31>();
    if (argc < 3)
        return 2;
    const QString service = argv[1];
    const QString owner = argv[2];

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !owner.startsWith(QLatin1Char(':')))
        return 2;
    // Subscribe before checking: the owner may vanish during startup.
    QDBusServiceWatcher ownerWatch(owner, bus, QDBusServiceWatcher::WatchForUnregistration);
    QObject::connect(&ownerWatch, &QDBusServiceWatcher::serviceUnregistered, &app,
                     &QCoreApplication::quit);
    const QDBusReply<bool> alive = bus.interface()->isServiceRegistered(owner);
    if (!alive.isValid())
        return 2;
    if (!alive.value())
        return 0;

    PayloadEchoObject echo;
    if (!bus.registerVirtualObject(QStringLiteral("/PayloadEcho"), &echo))
        return 2;
    if (!bus.registerService(service))
        return 2;

    return QCoreApplication::exec();
}
#include "payload_echo_service.moc"

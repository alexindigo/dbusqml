// Cross-process fd helper service (the "dedicated helper binary" shape from
// v1-followup.md — precedent: Nemo's dbustestd, KDE's dbusservice.py).
//
// Serves <service> with WriteThrough(h) -> h:
//   - writes "fd-xfer-send-ok\n" through the RECEIVED DBusQML.unixFd via the
//     DBusUtils fd helpers, then close()s it (the received value owns it),
//   - replies with its own unixFd (the reply file passed as argv[1], opened
//     and pre-written with "fd-xfer-reply-ok\n" via the helpers).
// Running as a separate process means fd numbers cannot coincide between
// caller and service — no same-process vacuous pass.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QQmlParserStatus>
#include <qqml.h>
#include <fcntl.h>
#include <unistd.h>

#include "dbusadaptor.h"
#include "dbustypes.h"
#include "dbusutils.h"

class FdHelperAdaptor : public DBusAdaptor {
    Q_OBJECT
public:
    explicit FdHelperAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

    // WriteThrough(h) -> h: writes the known sequence through the RECEIVED
    // unixFd and replies with our reply-file unixFd (the VM-gate shape).
    Q_INVOKABLE QVariant writeThrough(const QVariant &fd) {
        if (fd.userType() != qMetaTypeId<DBus::UnixFd>())
            return {};
        // The helpers, exercised from a plain QCoreApplication (no QML
        // engine): the string path of writeFd needs no JS runtime.
        DBusUtils utils;
        if (utils.writeFd(fd, QJSValue(QStringLiteral("fd-xfer-send-ok\n"))) != 16)
            return {};
        fd.value<DBus::UnixFd>().close(); // copies share one state: closed for all
        return QVariant::fromValue(replyFd);
    }

    DBus::UnixFd replyFd;
};
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 4)
        return 2;
    const QString replyFilePath = argv[1];
    const QString service = argv[2];
    const QString owner = argv[3];

    const QDBusConnection bus = QDBusConnection::sessionBus();
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

    // Pre-write the reply payload and keep the fd — via the quartet
    // ("rw" is O_RDWR|O_CREAT|O_TRUNC: the caller reads the payload
    // through the transferred fd).
    DBusUtils utils;
    const DBus::UnixFd replyFd = utils.openFd(replyFilePath, QStringLiteral("rw"));
    if (!replyFd.isValid())
        return 2;
    if (utils.writeFd(QVariant::fromValue(replyFd),
                      QJSValue(QStringLiteral("fd-xfer-reply-ok\n"))) != 17)
        return 2;

    FdHelperAdaptor adaptor;
    adaptor.replyFd = replyFd;
    adaptor.setService(service);
    adaptor.setPath(QStringLiteral("/FdXfer"));
    adaptor.setIface(service);
    adaptor.setSignatures(QVariantMap{{QStringLiteral("WriteThrough"), QStringLiteral("h")}});
    adaptor.classBegin();
    adaptor.componentComplete();

    return QCoreApplication::exec();
}
#include "fd_helper_service.moc"

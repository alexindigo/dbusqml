// Cross-process fd helper service (the "dedicated helper binary" shape from
// v1-followup.md — precedent: Nemo's dbustestd, KDE's dbusservice.py).
//
// Serves <service> with WriteThrough(h) -> h:
//   - writes "fd-xfer-send-ok\n" through the RECEIVED fd via the
//     DBusUtils fd quartet, then discharges receiver-closes via closeFd,
//   - replies with its own fd (the reply file passed as argv[1], opened
//     and pre-written with "fd-xfer-reply-ok\n" via the quartet).
// Running as a separate process means fd numbers cannot coincide between
// caller and service — no same-process vacuous pass.
// Running as a separate process means fd numbers cannot coincide between
// caller and service — no same-process vacuous pass.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QTimer>
#include <QQmlParserStatus>
#include <qqml.h>
#include <fcntl.h>
#include <unistd.h>

#include "dbusadaptor.h"
#include "dbusutils.h"

class FdHelperAdaptor : public DBusAdaptor {
    Q_OBJECT
public:
    explicit FdHelperAdaptor(QObject *parent = nullptr) : DBusAdaptor(parent) {}

    // WriteThrough(h) -> h: writes the known sequence through the RECEIVED
    // fd and replies with our reply-file fd (the VM-gate shape).
    Q_INVOKABLE int writeThrough(const QVariant &fd) {
        const int rawFd = fd.toInt();
        if (rawFd < 0)
            return -1;
        // The quartet, exercised from a plain QCoreApplication (no QML
        // engine): the string path of writeFd needs no JS runtime.
        DBusUtils utils;
        if (utils.writeFd(rawFd, QJSValue(QStringLiteral("fd-xfer-send-ok\n"))) != 16)
            return -1;
        utils.closeFd(rawFd); // received fd: receiver-closes, via the quartet
        return replyFd;
    }

    int replyFd = -1;
};
int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 3)
        return 2;
    const QString replyFilePath = argv[1];
    const QString service = argv[2];

    // Pre-write the reply payload and keep the fd — via the quartet
    // ("rw" is O_RDWR|O_CREAT|O_TRUNC: the caller reads the payload
    // through the transferred fd).
    DBusUtils utils;
    const int replyFd = utils.openFd(replyFilePath, QStringLiteral("rw"));
    if (replyFd < 0)
        return 2;
    if (utils.writeFd(replyFd, QJSValue(QStringLiteral("fd-xfer-reply-ok\n"))) != 17)
        return 2;

    FdHelperAdaptor adaptor;
    adaptor.replyFd = replyFd;
    adaptor.setService(service);
    adaptor.setPath(QStringLiteral("/FdXfer"));
    adaptor.setIface(service);
    adaptor.setSignatures(QVariantMap{{QStringLiteral("WriteThrough"), QStringLiteral("h")}});
    adaptor.classBegin();
    adaptor.componentComplete();

    // Safety: never outlive the test.
    QTimer::singleShot(60000, &app, &QCoreApplication::quit);
    return QCoreApplication::exec();
}
#include "fd_helper_service.moc"

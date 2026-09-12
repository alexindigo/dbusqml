// tsan_canary_foreign.cpp — the FOREIGN canary of the F-amended gate
// battery: the bus-bind worker-spawn race (register id
// qt6-dbus-bus-bind-worker-startup), promoted from the council-punch
// isolate_tsan.cpp connonly mode — a bare QDBusConnection::connectToBus
// on a private daemon, daemon killed, connection torn down. TSan reports
// the Qt-internal race with zero dbusqml participation frames (both
// access stacks are Qt/libtsan-only); the classifier must rule it
// FOREIGN-registered, counted, non-failing.
//
// The canary links NO libdbusqml and calls QtDBus directly (question-1
// ruling, tsan-register-tightening): the previous promotion connected
// through our DBusConnection wrapper, which put our frames into the
// attribution stacks of any bus-bind-family sibling whose main-side
// access executes inside the connect call (the 0x1d5ba1 orphan-cleanup
// variant fired exactly so, correctly classified OURS, and red-lit the
// battery by ambient-timing luck). With a pure-QtDBus caller the
// attribution stacks truncate inside Qt by construction of the unwind —
// whichever family member fires, the fire presents all-foreign.
//
// Its fire rate is Qt's, not ours: on a toolchain where it doesn't fire,
// the battery warns and continues (a silent foreign canary proves nothing
// about the detector — the pure/mixed canaries carry that proof).
#include <QCoreApplication>
#include <QDBusConnection>
#include <QProcess>
#include <QTest>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QProcess daemon;
    daemon.start(QStringLiteral("dbus-daemon"),
                 {QStringLiteral("--session"), QStringLiteral("--print-address"),
                  QStringLiteral("--nofork")});
    if (!daemon.waitForStarted(3000) || !daemon.waitForReadyRead(3000)) {
        fprintf(stderr, "canary_foreign: cannot start dbus-daemon\n");
        return 2;
    }
    const QByteArray addr = QByteArray(daemon.readLine()).trimmed();

    const QString name = QStringLiteral("canary_foreign");
    QDBusConnection conn = QDBusConnection::connectToBus(QString::fromLocal8Bit(addr), name);
    if (!conn.isConnected()) {
        fprintf(stderr, "canary_foreign: connectToBus failed\n");
        return 2;
    }
    daemon.kill();
    daemon.waitForFinished(3000);
    QTest::qWait(500);
    QDBusConnection::disconnectFromBus(name);
    return 0;
}

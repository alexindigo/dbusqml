#include "dbustypes.h"
#include "dbusutils.h"

#include <QDBusArgument>
#include <QHash>
#include <QJSEngine>
#include <QMutex>
#include <QtGlobal>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

// ==================== DBusQML.unixFd ====================

namespace DBus {

namespace {

// Wrapper-owned open descriptors, process-wide (the pressure valve's gauge).
QAtomicInt s_liveFds;

// Engines that received a unixFd — the valve's collection targets. Heap-
// allocated and never destroyed: an engine's destroyed() may run after
// static destruction began.
struct EngineRegistry {
    QMutex mutex;
    QHash<QJSEngine *, bool> engines; // value: a collection is queued
};
EngineRegistry &engineRegistry() {
    static auto *registry = new EngineRegistry;
    return *registry;
}

void registerEngine(QJSEngine *engine) {
    if (!engine)
        return;
    EngineRegistry &r = engineRegistry();
    QMutexLocker locker(&r.mutex);
    if (r.engines.contains(engine))
        return;
    r.engines.insert(engine, false);
    // Direct connection, emitted in ~QObject on the engine's thread: the
    // entry is gone before the pointer can dangle.
    QObject::connect(engine, &QObject::destroyed, [engine] {
        EngineRegistry &reg = engineRegistry();
        QMutexLocker l(&reg.mutex);
        reg.engines.remove(engine);
    });
}

// P4: queue one collectGarbage() per registered engine, on its own thread.
// An engine with a collection already queued is skipped; the flag clears
// when that collection has run.
void queueCollections() {
    EngineRegistry &r = engineRegistry();
    QMutexLocker locker(&r.mutex);
    for (auto it = r.engines.begin(); it != r.engines.end(); ++it) {
        if (it.value())
            continue;
        it.value() = true;
        QJSEngine *engine = it.key();
        QMetaObject::invokeMethod(
            engine,
            [engine] {
                engine->collectGarbage();
                EngineRegistry &reg = engineRegistry();
                QMutexLocker l(&reg.mutex);
                auto found = reg.engines.find(engine);
                if (found != reg.engines.end())
                    found.value() = false;
            },
            Qt::QueuedConnection);
    }
}

int computePressureThreshold() {
    // clamp(RLIMIT_NOFILE soft / 4, 64, 1024); no usable limit → the cap.
    qint64 soft = 4 * 1024;
    rlimit rl{};
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
        soft = static_cast<qint64>(rl.rlim_cur);
    return static_cast<int>(qBound(qint64(64), soft / 4, qint64(1024)));
}

void fdAcquired() {
    const int live = s_liveFds.fetchAndAddRelaxed(1) + 1;
    const int threshold = UnixFd::pressureThreshold();
    // Upward crossings of threshold+1, 2*threshold+1, … — one collection
    // per `threshold` new descriptors while above it, never a stuck state.
    if (live <= threshold || (live - threshold) % threshold != 1)
        return;
    static QBasicAtomicInt warned = Q_BASIC_ATOMIC_INITIALIZER(0);
    if (warned.testAndSetRelaxed(0, 1))
        qWarning("dbusqml: %d open DBusQML.unixFd descriptors exceed the pressure threshold "
                 "(%d) — collecting garbage; call close() on fds you no longer need",
                 live, threshold);
    queueCollections();
}

} // namespace

class UnixFdPrivate : public QSharedData {
public:
    QDBusUnixFileDescriptor fd; // never shared with another owner's state
    bool escaped = false;       // reached JS: last-copy close is the GC fallback

    ~UnixFdPrivate() {
        if (!fd.isValid())
            return;
        if (escaped)
            qWarning("dbusqml: fd closed by garbage collector — call close()");
        release();
    }
    void release() {
        if (!fd.isValid())
            return;
        fd = QDBusUnixFileDescriptor(); // the last reference closes it
        s_liveFds.fetchAndSubRelaxed(1);
    }
};

UnixFd::UnixFd() = default;

UnixFd::UnixFd(const QJSValue &fd) {
    const double n = fd.isNumber() ? fd.toNumber() : -1.0;
    if (!(n >= 0 && n <= INT_MAX && std::floor(n) == n)) {
        qWarning("dbusqml: DBusQML.unixFd expects a file descriptor number, got %s",
                 qPrintable(fd.toString()));
        init({});
    } else {
        *this = UnixFd(static_cast<int>(n));
    }
    if (d)
        d->escaped = true; // constructed in JS
}

UnixFd::UnixFd(int fd) {
    if (fd < 0) {
        init({});
        return;
    }
    const QDBusUnixFileDescriptor dup(fd); // Qt dup()s; the caller keeps fd
    if (!dup.isValid())
        qWarning("dbusqml: DBusQML.unixFd cannot duplicate fd %d: %s", fd, std::strerror(errno));
    init(dup);
}

UnixFd::UnixFd(const QDBusUnixFileDescriptor &fd) {
    if (!fd.isValid()) {
        init({});
        return;
    }
    // P1: a fresh dup(), never a share — each owner closes independently.
    const QDBusUnixFileDescriptor dup(fd.fileDescriptor());
    if (!dup.isValid())
        qWarning("dbusqml: cannot duplicate a received fd: %s", std::strerror(errno));
    init(dup);
}

UnixFd::UnixFd(const UnixFd &other) = default;
UnixFd &UnixFd::operator=(const UnixFd &other) = default;
UnixFd::~UnixFd() = default;

void UnixFd::init(const QDBusUnixFileDescriptor &owned) {
    d = new UnixFdPrivate;
    if (!owned.isValid())
        return;
    d->fd = owned;
    fdAcquired();
}

bool UnixFd::isValid() const {
    return d && d->fd.isValid();
}

void UnixFd::close() {
    if (d)
        d->release();
}

int UnixFd::take() const {
    if (!isValid()) {
        qWarning("dbusqml: DBusQML.unixFd.take(): the fd is not valid");
        return -1;
    }
    // At or above 3, so closeFd() (which refuses 0-2) always accepts it.
    const int dup = ::fcntl(d->fd.fileDescriptor(), F_DUPFD_CLOEXEC, 3);
    if (dup < 0)
        qWarning("dbusqml: DBusQML.unixFd.take(): dup failed: %s", std::strerror(errno));
    return dup;
}

QString UnixFd::toString() const {
    return isValid() ? QStringLiteral("DBusQML.unixFd(valid)")
                     : QStringLiteral("DBusQML.unixFd(invalid)");
}

int UnixFd::fileDescriptor() const {
    return d ? d->fd.fileDescriptor() : -1;
}

QDBusUnixFileDescriptor UnixFd::descriptor() const {
    return d ? d->fd : QDBusUnixFileDescriptor();
}

void UnixFd::noteDelivered(QJSEngine *engine) const {
    if (!isValid() || !engine) // no engine: a C++ consumer, not JS
        return;
    d->escaped = true;
    registerEngine(engine);
}

void UnixFd::noteDelivered(const QVariant &v, QJSEngine *engine) {
    const int t = v.userType();
    if (t == qMetaTypeId<UnixFd>()) {
        v.value<UnixFd>().noteDelivered(engine);
    } else if (t == QMetaType::QVariantList) {
        for (const QVariant &e : v.toList())
            noteDelivered(e, engine);
    } else if (t == QMetaType::QVariantMap) {
        const QVariantMap map = v.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it)
            noteDelivered(it.value(), engine);
    } else if (t == qMetaTypeId<QDBusVariant>()) {
        noteDelivered(v.value<QDBusVariant>().variant(), engine);
    }
}

int UnixFd::liveCount() {
    return s_liveFds.loadRelaxed();
}

int UnixFd::pressureThreshold() {
    static const int threshold = computePressureThreshold();
    return threshold;
}

} // namespace DBus

QDBusArgument &operator<<(QDBusArgument &arg, const DBusAsArray &a) {
    arg << a.value;
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBusAsArray &a) {
    DecodeState state;
    const QVariant got = readBySignature(arg, state);
    if (!state.ok) {
        a.value.clear();
        qWarning("dbusqml: array could not be decoded");
        return arg;
    }
    if (got.userType() == QMetaType::QStringList)
        a.value = got.toStringList();
    else {
        a.value.clear();
        for (const QVariant &item : got.toList())
            a.value.append(item.toString());
    }
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const DBus::Struct &s) {
    arg.beginStructure();
    for (const QVariant &m : s.value) {
        // Marshal each member by its concrete type. QVariant doesn't have
        // a direct operator<< — dispatch on the metatype.
        switch (m.userType()) {
        case QMetaType::Bool:
            arg << m.toBool();
            break;
        case QMetaType::Int:
            arg << m.toInt();
            break;
        case QMetaType::UInt:
            arg << m.toUInt();
            break;
        case QMetaType::Short:
            arg << static_cast<short>(m.toInt());
            break;
        case QMetaType::UShort:
            arg << static_cast<ushort>(m.toUInt());
            break;
        case QMetaType::LongLong:
            arg << m.toLongLong();
            break;
        case QMetaType::ULongLong:
            arg << m.toULongLong();
            break;
        case QMetaType::Double:
            arg << m.toDouble();
            break;
        case QMetaType::QString:
            arg << m.toString();
            break;
        case QMetaType::QByteArray:
            arg << m.toByteArray();
            break;
        case QMetaType::QStringList:
            arg << m.toStringList();
            break;
        case QMetaType::QVariantList:
            arg << m.toList();
            break;
        case QMetaType::QVariantMap:
            arg << m.toMap();
            break;
        default:
            if (m.userType() == qMetaTypeId<QDBusVariant>())
                arg << m.value<QDBusVariant>();
            else if (m.userType() == qMetaTypeId<QDBusObjectPath>())
                arg << m.value<QDBusObjectPath>();
            else if (m.userType() == qMetaTypeId<QDBusSignature>())
                arg << m.value<QDBusSignature>();
            else if (m.userType() == qMetaTypeId<DBus::Struct>())
                arg << m.value<DBus::Struct>(); // nested struct
            else if (m.userType() == qMetaTypeId<QDBusUnixFileDescriptor>())
                arg << m.value<QDBusUnixFileDescriptor>(); // h (converted unixFd)
            else
                qWarning("DBus::Struct: unsupported member type %d", m.userType());
            break;
        }
    }
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, DBus::Struct &s) {
    DecodeState state;
    s.value = readBySignature(arg, state).toList();
    if (!state.ok)
        qWarning("dbusqml: struct could not be decoded");
    return arg;
}

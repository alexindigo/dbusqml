#include "dbuscatalog.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QXmlStreamReader>

DBusCatalog &DBusCatalog::instance() {
    static DBusCatalog s;
    return s;
}

DBusCatalog::DBusCatalog() {
    loadPaths();
}

std::optional<DBusCatalog::InterfaceSpec> DBusCatalog::lookup(const QString &iface) const {
    QReadLocker lock(&m_lock);
    auto it = m_ifaces.find(iface);
    if (it == m_ifaces.end())
        return std::nullopt;
    return *it;
}

void DBusCatalog::reload() {
    QWriteLocker lock(&m_lock);
    m_ifaces.clear();
    loadPaths();
}

void DBusCatalog::loadPaths() {
    QStringList paths;

    // 0. Lowest-precedence tier: the freedesktop-standard interface registry
    //    <data>/dbus-1/interfaces/ — where xdg-desktop-portal and friends
    //    install their interface descriptions. Loaded FIRST so every
    //    dbusqml-specific tier (bundled, user config, env) can override it.
    const auto genericData = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &dir : genericData)
        paths << (dir + QStringLiteral("/dbus-1/interfaces"));

    // 1. Bundled Qt resource
    paths << QStringLiteral(":/dbusqml/types");

    // 2. System XDG data dirs — reverse-iterate so user dirs are
    //    processed LAST (and thus win, since later inserts overwrite).
    //    standardLocations returns user-first, so reverse for
    //    system-first-then-user precedence.
    for (auto it = genericData.crbegin(); it != genericData.crend(); ++it)
        paths << (*it + QStringLiteral("/dbusqml/types"));

    // 3. User XDG config
    const QString userDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!userDir.isEmpty())
        paths << (userDir + QStringLiteral("/dbusqml/types"));

    // 4. DBUSQML_TYPES_PATH env var
    const QByteArray envPath = qgetenv("DBUSQML_TYPES_PATH");
    if (!envPath.isEmpty()) {
        const auto entries = QString::fromLocal8Bit(envPath).split(':', Qt::SkipEmptyParts);
        for (const QString &p : entries)
            paths << p;
    }

    for (const QString &dir : paths)
        loadDirectory(dir);
}

void DBusCatalog::loadDirectory(const QString &dir) {
    QDir d(dir);
    if (!d.exists())
        return;
    const auto files =
        d.entryList(QStringList() << QStringLiteral("*.xml"), QDir::Files | QDir::Readable);
    for (const QString &fname : files)
        loadFile(d.filePath(fname));
}

void DBusCatalog::loadFile(const QString &filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return;

    // Parse into a scratch map and merge only on success: a malformed file
    // (that directory contains arbitrary third-party content) must be
    // discarded whole, never partially merged into the catalog.
    QHash<QString, InterfaceSpec> parsed;
    QXmlStreamReader reader(&f);
    QString currentIface;
    InterfaceSpec spec;
    spec.source = filePath;

    QString currentMethod;
    QString currentSignal;
    QStringList currentArgs;
    QStringList currentOutArgs;

    auto flushMethod = [&]() {
        if (!currentMethod.isEmpty()) {
            spec.methods.insert(currentMethod,
                                MethodSpec{currentMethod, currentArgs, currentOutArgs});
            currentMethod.clear();
        }
    };
    auto flushSignal = [&]() {
        if (!currentSignal.isEmpty()) {
            spec.signals_.insert(currentSignal, SignalSpec{currentSignal, currentArgs});
            currentSignal.clear();
        }
    };
    auto clearArgs = [&]() {
        currentArgs.clear();
        currentOutArgs.clear();
    };

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.isStartElement()) {
            const auto name = reader.name();
            if (name == QLatin1String("interface")) {
                currentIface = reader.attributes().value("name").toString();
                spec = InterfaceSpec{};
                spec.source = filePath;
                currentMethod.clear();
                currentSignal.clear();
                clearArgs();
            } else if (name == QLatin1String("method") && !currentIface.isEmpty()) {
                flushMethod();
                flushSignal();
                currentMethod = reader.attributes().value("name").toString();
                clearArgs();
            } else if (name == QLatin1String("signal") && !currentIface.isEmpty()) {
                flushMethod();
                flushSignal();
                currentSignal = reader.attributes().value("name").toString();
                clearArgs();
            } else if (name == QLatin1String("arg") &&
                       (!currentMethod.isEmpty() || !currentSignal.isEmpty())) {
                const QString type = reader.attributes().value("type").toString();
                if (!currentSignal.isEmpty()) {
                    // Signal args are conventionally declared direction="out"
                    // in the spec XML; they are always signal args.
                    currentArgs << type;
                } else {
                    const QString dir = reader.attributes().value("direction").toString();
                    if (dir == QLatin1String("out"))
                        currentOutArgs << type;
                    else
                        currentArgs << type;
                }
            } else if (name == QLatin1String("property") && !currentIface.isEmpty()) {
                // A9: keep the declared type and access, not just the name.
                DBusCatalog::PropertySpec p;
                p.name = reader.attributes().value("name").toString();
                p.type = reader.attributes().value("type").toString();
                p.access = reader.attributes().value("access").toString();
                spec.properties.append(p);
            }
        } else if (reader.isEndElement()) {
            const auto name = reader.name();
            if (name == QLatin1String("interface")) {
                flushMethod();
                flushSignal();
                parsed.insert(currentIface, spec);
                currentIface.clear();
                clearArgs();
            }
        }
    }

    if (reader.hasError()) {
        qWarning() << "DBusCatalog: XML error in" << filePath
                   << "- skipping file:" << reader.errorString();
        return;
    }

    for (auto it = parsed.cbegin(); it != parsed.cend(); ++it)
        m_ifaces.insert(it.key(), it.value());
}

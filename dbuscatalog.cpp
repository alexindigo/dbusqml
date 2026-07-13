#include "dbuscatalog.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QXmlStreamReader>

DBusCatalog &DBusCatalog::instance()
{
    static DBusCatalog s;
    return s;
}

DBusCatalog::DBusCatalog()
{
    loadPaths();
}

const DBusCatalog::InterfaceSpec *DBusCatalog::lookup(const QString &iface) const
{
    QReadLocker lock(&m_lock);
    auto it = m_ifaces.find(iface);
    return it == m_ifaces.end() ? nullptr : &it.value();
}

void DBusCatalog::reload()
{
    QWriteLocker lock(&m_lock);
    m_ifaces.clear();
    loadPaths();
}

void DBusCatalog::loadPaths()
{
    QStringList paths;

    // 1. Bundled Qt resource
    paths << QStringLiteral(":/dbusqml/types");

    // 2. System XDG data dirs
    const auto genericData = QStandardPaths::standardLocations(
        QStandardPaths::GenericDataLocation);
    for (const QString &dir : genericData)
        paths << (dir + QStringLiteral("/dbusqml/types"));

    // 3. User XDG config
    const QString userDir = QStandardPaths::writableLocation(
        QStandardPaths::GenericConfigLocation);
    if (!userDir.isEmpty())
        paths << (userDir + QStringLiteral("/dbusqml/types"));

    // 4. DBUSQML_TYPES_PATH env var
    const QByteArray envPath = qgetenv("DBUSQML_TYPES_PATH");
    if (!envPath.isEmpty()) {
        const auto entries = QString::fromLocal8Bit(envPath).split(':',
            Qt::SkipEmptyParts);
        for (const QString &p : entries)
            paths << p;
    }

    for (const QString &dir : paths)
        loadDirectory(dir);
}

void DBusCatalog::loadDirectory(const QString &dir)
{
    QDir d(dir);
    if (!d.exists()) return;
    const auto files = d.entryList(QStringList() << QStringLiteral("*.xml"),
                                    QDir::Files | QDir::Readable);
    for (const QString &fname : files)
        loadFile(d.filePath(fname));
}

void DBusCatalog::loadFile(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return;

    QXmlStreamReader reader(&f);
    QString currentIface;
    InterfaceSpec spec;
    spec.source = filePath;

    QString currentMethod;
    QString currentSignal;
    QStringList currentArgs;

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
            } else if (name == QLatin1String("method") && !currentIface.isEmpty()) {
                if (!currentMethod.isEmpty()) {
                    spec.methods.insert(currentMethod,
                        MethodSpec{currentMethod, currentArgs});
                }
                currentMethod = reader.attributes().value("name").toString();
                currentSignal.clear();
                currentArgs.clear();
            } else if (name == QLatin1String("signal") && !currentIface.isEmpty()) {
                if (!currentMethod.isEmpty()) {
                    spec.methods.insert(currentMethod,
                        MethodSpec{currentMethod, currentArgs});
                    currentMethod.clear();
                }
                if (!currentSignal.isEmpty()) {
                    spec.signals_.insert(currentSignal,
                        SignalSpec{currentSignal, currentArgs});
                }
                currentSignal = reader.attributes().value("name").toString();
                currentArgs.clear();
            } else if (name == QLatin1String("arg") &&
                       (!currentMethod.isEmpty() || !currentSignal.isEmpty())) {
                currentArgs << reader.attributes().value("type").toString();
            }
        } else if (reader.isEndElement()) {
            const auto name = reader.name();
            if (name == QLatin1String("interface")) {
                if (!currentMethod.isEmpty()) {
                    spec.methods.insert(currentMethod,
                        MethodSpec{currentMethod, currentArgs});
                    currentMethod.clear();
                }
                if (!currentSignal.isEmpty()) {
                    spec.signals_.insert(currentSignal,
                        SignalSpec{currentSignal, currentArgs});
                    currentSignal.clear();
                }
                m_ifaces.insert(currentIface, spec);
                currentIface.clear();
                currentArgs.clear();
            }
        }
    }

    if (reader.hasError()) {
        qWarning() << "DBusCatalog: XML error in" << filePath
                   << ":" << reader.errorString();
    }
}

#pragma once

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>

class DBusCatalog {
public:
    struct MethodSpec {
        QString name;
        QStringList argTypes;
    };
    struct SignalSpec {
        QString name;
        QStringList argTypes;
    };
    struct InterfaceSpec {
        QString source;
        QHash<QString, MethodSpec> methods;
        QHash<QString, SignalSpec> signals_;
    };

    static DBusCatalog &instance();

    const InterfaceSpec *lookup(const QString &ifaceName) const;
    void reload();

private:
    DBusCatalog();
    Q_DISABLE_COPY_MOVE(DBusCatalog)

    void loadPaths();
    void loadDirectory(const QString &dir);
    void loadFile(const QString &filePath);

    QHash<QString, InterfaceSpec> m_ifaces;
    mutable QReadWriteLock m_lock;
};

#pragma once

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>
#include <optional>

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
        QStringList properties;
    };

    static DBusCatalog &instance();

    std::optional<InterfaceSpec> lookup(const QString &ifaceName) const;
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

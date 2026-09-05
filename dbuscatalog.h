#pragma once

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

class DBusCatalog {
public:
    struct MethodSpec {
        QString name;
        QStringList argTypes; // in-args, declaration order
        QStringList outTypes; // out-args, declaration order
    };
    struct SignalSpec {
        QString name;
        QStringList argTypes;
    };
    struct PropertySpec {
        QString name;
        QString type;   // declared D-Bus type (empty = undeclared)
        QString access; // read / readwrite (empty = undeclared)
    };
    struct InterfaceSpec {
        QString source;
        QHash<QString, MethodSpec> methods;
        QHash<QString, SignalSpec> signals_;
        QVector<PropertySpec> properties; // A9: type/access now retained
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

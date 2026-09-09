// libFuzzer entry point when built with -fsanitize=fuzzer (CI); doubles
// as a seed-corpus smoke main otherwise (see bottom).
#include <QByteArray>
#include <QString>
#include <QVariant>

#include "dbusconnection.h"
#include "dbusintrospection.h"

static int runOne(const uint8_t *data, size_t size) {
    if (size == 0 || size > 65536)
        return 0;
    const QString xml =
        QString::fromUtf8(QByteArray::fromRawData(reinterpret_cast<const char *>(data), int(size)));
    // Parse for a fixed iface + a garbage iface (both must terminate).
    const DBusIntrospectionData d1 =
        parseDBusIntrospection(xml, QStringLiteral("org.freedesktop.DBus.Properties"));
    const DBusIntrospectionData d2 =
        parseDBusIntrospection(xml, QStringLiteral("org.example.Nope"));
    (void)d1.methodNames.size();
    (void)d2.methodNames.size();
    // CF-24: chain the parse→marshal trust crossing — every arg type the
    // parser extracted flows into the REAL signature walker + strict gate
    // + writeBySignature, exactly like production peer-XML handling. A
    // hostile `type="a"×N` must fail loud here, never crash.
    for (const QHash<QString, QStringList> *maps :
         {&d1.methodArgTypes, &d1.methodOutTypes, &d2.methodArgTypes, &d2.methodOutTypes}) {
        for (const QStringList &argLists : *maps) {
            for (const QString &argSig : argLists) {
                int pos = 0;
                (void)firstCompleteType(argSig, pos);
                int spos = 0;
                if (isStrictSignature(argSig, spos) && spos == argSig.size()) {
                    const QVariant m = writeBySignature(argSig, QVariant());
                    (void)m.isValid();
                }
            }
        }
    }
    return 0;
}

#ifdef DBUSQML_FUZZ_MAIN
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("corpus/introspection");
    QDir d(dir);
    int n = 0;
    for (const QString &f : d.entryList(QDir::Files)) {
        QFile file(d.filePath(f));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = file.readAll();
        runOne(reinterpret_cast<const uint8_t *>(bytes.constData()), size_t(bytes.size()));
        ++n;
    }
    QTextStream(stderr) << "fuzz_introspection_xml: " << n << " seeds OK\n";
    return 0;
}
#else
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return runOne(data, size);
}
#endif

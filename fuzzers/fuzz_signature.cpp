// libFuzzer over signature strings (D10). Signatures arrive inside remote
// introspection XML `type=` attrs and flow verbatim into firstCompleteType
// + the recursive walkers — pathological nesting must fail loud (depth
// cap), never exhaust the stack.
//
// CF-24: this TU links the REAL firstCompleteType from dbusconnection.cpp
// (declared in dbusconnection.h) — the old TU-local copy ("identical by
// construction") could drift from the crashing function without any test
// noticing. The only TU-local logic left is the libFuzzer entry shim.
#include <QByteArray>
#include <QString>

#include "dbusconnection.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static int runOne(const uint8_t *data, size_t size) {
    // CF-24: 1 MB ceiling — the CF-3 crash needs ~200k of 'a's; the old
    // 1024-byte cap could never reach it.
    if (size == 0 || size > 1048576)
        return 0;
    const QString sig =
        QString::fromUtf8(QByteArray::fromRawData(reinterpret_cast<const char *>(data), int(size)));
    // The REAL walker (linked from dbusconnection.cpp) — a depth-cap
    // breach must return empty, never crash. Follow with the strict gate
    // and the full writeBySignature marshal, mirroring the production
    // introspection→marshal chain (parse→marshal trust crossing).
    int pos = 0;
    const QString t = firstCompleteType(sig, pos);
    (void)t.size();
    (void)pos;
    int spos = 0;
    if (isStrictSignature(sig, spos) && spos == sig.size()) {
        const QVariant m = writeBySignature(sig, QVariant());
        (void)m.isValid();
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
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("corpus/signatures");
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
    QTextStream(stderr) << "fuzz_signature: " << n << " seeds OK\n";
    return 0;
}
#else
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return runOne(data, size);
}
#endif

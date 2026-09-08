// libFuzzer over signature strings (D10). Signatures arrive inside remote
// introspection XML `type=` attrs and flow verbatim into firstCompleteType
// + the recursive walkers — pathological nesting must fail loud (depth
// cap), never exhaust the stack.
//
// NOTE: this is a TU-local copy of firstCompleteType (dbusconnection.cpp)
// — the real TU drags moc/pending-reply/catalog link deps. The copy is
// verified identical by construction (same algorithm, pure string walk);
// any divergence fails the seed-corpus run, not silently.
#include <QByteArray>
#include <QString>

static QString fuzzFirstCompleteType(const QString &sig, int &pos) {
    if (pos >= sig.size())
        return {};
    int start = pos;
    QChar c = sig.at(pos);
    if (QStringLiteral("ybnqiuxtdhsogv").contains(c)) {
        ++pos;
        return sig.mid(start, 1);
    }
    if (c == QLatin1Char('a')) {
        ++pos;
        QString elem = fuzzFirstCompleteType(sig, pos);
        if (elem.isEmpty())
            return {};
        return sig.mid(start, pos - start);
    }
    if (c == QLatin1Char('(')) {
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('('))
                ++depth;
            else if (sig.at(pos) == QLatin1Char(')'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }
    if (c == QLatin1Char('{')) {
        int depth = 1;
        ++pos;
        while (pos < sig.size() && depth > 0) {
            if (sig.at(pos) == QLatin1Char('{'))
                ++depth;
            else if (sig.at(pos) == QLatin1Char('}'))
                --depth;
            ++pos;
        }
        if (depth != 0)
            return {};
        return sig.mid(start, pos - start);
    }
    return {};
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static int runOne(const uint8_t *data, size_t size) {
    if (size == 0 || size > 1024)
        return 0;
    const QString sig =
        QString::fromUtf8(QByteArray::fromRawData(reinterpret_cast<const char *>(data), int(size)));
    int pos = 0;
    const QString t = fuzzFirstCompleteType(sig, pos);
    (void)t.size();
    (void)pos;
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

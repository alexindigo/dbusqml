// Test-only wire producer. Deliberately independent of dbusqml's signature
// parser/writer: the decode boundary must accept the protocol's full depth.
#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>

template <int Arrays, int Structs> struct DeepPayload {};

template <int A, int S> QDBusArgument &operator<<(QDBusArgument &arg, const DeepPayload<A, S> &) {
    if constexpr (A > 0) {
        arg.beginArray(QMetaType::fromType<DeepPayload<A - 1, S>>());
        arg << DeepPayload<A - 1, S>{};
        arg.endArray();
    } else if constexpr (S > 0) {
        arg.beginStructure();
        arg << DeepPayload<0, S - 1>{};
        arg.endStructure();
    } else {
        arg << 42;
    }
    return arg;
}

template <int A, int S>
const QDBusArgument &operator>>(const QDBusArgument &arg, DeepPayload<A, S> &) {
    // Only used as a producer, never as the consumer under test.
    return arg;
}

template <int A, int S> void registerDeepPayload() {
    if constexpr (A > 0)
        registerDeepPayload<A - 1, S>();
    qDBusRegisterMetaType<DeepPayload<A, S>>();
}

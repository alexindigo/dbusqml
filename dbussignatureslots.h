#pragma once

// Signature-slot pool (F1/B9): automatically minted, assign-once metatype
// registrations that let the signature-driven walker produce array/map
// element signatures with no registered C++ type (e.g. "(sa{sv})").
//
// Mechanism (verified against Qt 6.11.2 source — F1 pre-step in
// execution-report.md): QDBusArgument::beginArray/beginMap take ONLY a
// registered metatype; the header string comes from
// QDBusMetaType::registerCustomType (one fixed signature per id). N distinct
// tag types give N registry keys; the walker assigns each unproducible
// element signature to a slot ONCE and streams the elements itself
// (beginStructure needs no metatype; the depth cap and recursion already
// exist in the walker).
//
// Slot ids are minted at first use via qRegisterMetaType (stable
// process-wide; idempotent across translation units). Deliberately NOT
// Q_DECLARE_METATYPE: 65 explicit declarations are boilerplate that no
// formatter keeps stable, and compile-time ids buy nothing here.
//
// Four invariants, enforced by construction:
//  1. Assign-once, never reassigned — typeToSignature returns a pointer into
//     the registry entry after releasing its lock; overwriting frees it under
//     a concurrent marshaller (use-after-free, not a race).
//  2. Assignment happens BEFORE the metatype is returned — an unassigned slot
//     must never reach beginArray/typeToSignature. A callback-less unassigned
//     slot yields null → loud unregisteredTypeError, never silent corruption.
//  3. dbusqml's own signature→slot map is mutex-guarded and add-only; the
//     mutex is never held while streaming (check-then-register is a single
//     critical section; nested assignment re-takes it, never nested-held).
//  4. Pool exhaustion is a loud local failure (warning + invalid metatype).
//
// Design absolutes: NO qDBusRegisterMetaType, NO marshall/demarshall
// operators — ever. Registering callbacks would let Qt's createSignature
// cache junk for unassigned slots. Slot<PoolSize> is the eternal canary:
// never assigned, used only by the unassigned-slot loud-fail pin.

#include <QMetaType>
#include <QString>

template <int N> struct DbusSignatureSlot {
    char tag;
};

// Assignable slots: indices 0..PoolSize-1. Raising the bound is free (one
// tag type per slot); the pool hands out indices, never types.
constexpr int SignatureSlotPoolSize = 64;
// The eternal canary index: never assigned a signature.
constexpr int SignatureSlotCanaryIndex = 64;

// The canary metatype. Header-only and stateless — safe to use anywhere,
// including tests-first runs against trees without the pool implementation.
inline QMetaType signatureSlotCanary() {
    static const QMetaType mt =
        QMetaType(qRegisterMetaType<DbusSignatureSlot<SignatureSlotCanaryIndex>>());
    return mt;
}

// Assign-once resolution of an unproducible element signature to a pool slot.
// The signature is registered BEFORE the metatype is returned, by
// construction (invariant 2). Defined in dbusconnection.cpp; returns an
// invalid QMetaType — loudly — when the signature is not a valid single
// complete type or the pool is exhausted (invariant 4).
QMetaType signatureSlotForSignature(const QString &sig);

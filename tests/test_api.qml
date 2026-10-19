import QtQuick
import QtTest
import QtCore
import DBus 1.0
import DBus 1.0 as DBusQML

TestCase {
    id: apiTest
    name: "DBusQmlApiTest"

    property DBusQML.DBusPendingReply pendingReply

    // StandardPaths returns a file URL; fd helpers take a filesystem path.
    readonly property string tempPath: decodeURIComponent(StandardPaths.writableLocation(StandardPaths.TempLocation).toString().replace(/^file:\/\//, ""))

    // Shared proxies declared inline (declarative form). Two paths cover
    // both shapes of test in this file. Declarative destruction (managed
    // by the QML component tree at end of the run) sidesteps the Qt bug
    // in `Qt.createQmlObject(...).destroy()` on QQmlPropertyMap-derived
    // types that affects Qt 6.5–6.10. See KNOWN_ISSUES.md.
    DBus {
        id: rootProxy
        service: "org.freedesktop.DBus"
        path: "/"
        iface: "org.freedesktop.DBus"
    }

    DBus {
        id: dbusProxy
        service: "org.freedesktop.DBus"
        path: "/org/freedesktop/DBus"
        iface: "org.freedesktop.DBus"
    }

    // ── Enum ───────────────────────────────────────────────

    function test_bus_type_enum() {
        compare(DBusQML.busType.Session, 0, "busType.Session should be 0")
        compare(DBusQML.busType.System, 1, "busType.System should be 1")
    }

    // ── Singletons ─────────────────────────────────────────

    function test_bus_singletons() {
        compare(typeof DBusQML.SessionBus, "object", "SessionBus should be available")
        compare(typeof DBusQML.SystemBus, "object", "SystemBus should be available")
    }

    // ── DBusMessage ────────────────────────────────────────

    function test_dbus_message_from_map() {
        var msg = new DBusQML.dbusMessage({
            service: "org.freedesktop.DBus",
            path: "/",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
            arguments: [],
            signature: "",
        })
        compare(msg.service, "org.freedesktop.DBus", "message service property")
        compare(msg.path, "/", "message path property")
        compare(msg.iface, "org.freedesktop.DBus", "message iface property")
        compare(msg.member, "ListNames", "message member property")
        compare(msg.arguments.length, 0, "message arguments should be empty")
    }

    function test_dbus_message_with_args() {
        var msg = new DBusQML.dbusMessage({
            service: "org.freedesktop.DBus",
            path: "/",
            iface: "org.freedesktop.DBus",
            member: "NameHasOwner",
            arguments: ["org.freedesktop.DBus"],
            signature: "s",
        })
        compare(msg.arguments.length, 1, "should have one argument")
    }

    // ── DBusError ──────────────────────────────────────────

    function test_dbus_error_type() {
        compare(typeof DBusQML.dbusError, "object", "DBusError type should be available")
    }

    function test_pending_reply_type() {
        compare(typeof DBusQML.DBusPendingReply, "object",
                "DBusPendingReply type should be available")
    }

    function test_types_bool() {
        var v = new DBusQML.boolean(true)
        compare(v.value, true, "bool value")
    }

    function test_types_int32() {
        var v = new DBusQML.int32(42)
        compare(v.value, 42, "int32 value")
    }

    function test_types_int16() {
        var v = new DBusQML.int16(32767)
        compare(v.value, 32767, "int16 value")
    }

    function test_types_int64() {
        var v = new DBusQML.int64(21474836470)
        compare(v.value, 21474836470, "int64 value")
    }

    function test_types_uint32() {
        var v = new DBusQML.uint32(4294967295)
        compare(v.value, 4294967295, "uint32 value")
    }

    function test_types_uint16() {
        var v = new DBusQML.uint16(65535)
        compare(v.value, 65535, "uint16 value")
    }

    function test_types_uint64() {
        var v = new DBusQML.uint64(1048576)
        compare(v.value, 1048576, "uint64 value")
    }

    function test_types_double() {
        var v = new DBusQML.double(3.14)
        compare(v.value, 3.14, "double value")
    }

    function test_types_byte() {
        var v = new DBusQML.byte(255)
        compare(v.value, 255, "byte value")
    }

    function test_types_string() {
        var v = new DBusQML.string("hello")
        compare(v.value, "hello", "string value")
    }

    function test_types_objectPath() {
        var v = new DBusQML.objectPath("/org/freedesktop/DBus")
        compare(v.toString(), "/org/freedesktop/DBus", "objectPath toString")
    }

    function test_types_signature() {
        var v = new DBusQML.signature("s")
        compare(v.toString(), "s", "signature toString")
    }

    function test_types_variant() {
        var v = new DBusQML.variant("test")
        compare(v.toString(), "test", "variant toString")
    }

    function test_types_dict() {
        var v = new DBusQML.dict({"key": "value"})
        compare(v.value["key"], "value", "dict value")
    }

    // ── DBus element (proxy) ───────────────────────────────

    function test_proxy_properties() {
        compare(rootProxy.service, "org.freedesktop.DBus", "DBus service property")
        compare(rootProxy.path, "/", "DBus path property")
        compare(rootProxy.iface, "org.freedesktop.DBus", "DBus iface property")
    }

    function test_proxy_call_exists() {
        compare(typeof rootProxy.call, "function", "call should be a function")
    }

    // ── asyncCall (needs session bus) ──────────────────────

    function test_async_call_list_names() {
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
        })
        verify(pendingReply !== null, "asyncCall should return a pending reply")
        tryVerify(() => pendingReply.isFinished, 5000,
                  "ListNames should finish within timeout")
        verify(!pendingReply.isError,
               "ListNames should not error: " + pendingReply.error.message)
        verify(pendingReply.isValid, "reply should be valid")
        verify(pendingReply.value !== undefined, "reply should have a value")
        verify(pendingReply.values.length > 0, "reply should have at least one value")
    }

    function test_async_call_with_string_arg() {
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "NameHasOwner",
            arguments: ["org.freedesktop.DBus"],
        })
        verify(pendingReply !== null)
        tryVerify(() => pendingReply.isFinished, 5000)
        verify(!pendingReply.isError,
               "NameHasOwner should not error: " + pendingReply.error.message)
        compare(pendingReply.value, true, "DBus should have an owner")
    }

    function test_async_call_error() {
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/",
            iface: "org.freedesktop.DBus",
            member: "NonExistentMethod",
        })
        verify(pendingReply !== null)
        tryVerify(() => pendingReply.isFinished, 5000)
        verify(pendingReply.isError, "calling non-existent method should error")
        verify(!pendingReply.isValid, "errored reply should not be valid")
    }

    function test_async_call_finished_signal() {
        var signalReceived = false
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
        })
        pendingReply.finished.connect(function() {
            signalReceived = true
        })
        tryVerify(function() { return signalReceived; }, 5000,
                  "finished signal should fire")
    }

    function test_async_call_promise_style() {
        var done = false
        DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames"
        }, function(result) {
            done = true
        }, function(error) {
            done = true
        })
        tryVerify(function() { return done; }, 5000,
                  "promise-style call should complete")
    }

    // ── R4: pending-reply GC handoff (road-to-one, owner Call 4) ─────
    // Completed replies are handed to the JS GC synchronously at
    // completion (safe-A; spike green under ASan on the fork before
    // landing). QML-level pin: N looped calls with references dropped,
    // gc() forced between completion batches — every finished must still
    // fire (no collection-before-delivery), and after gc() the engine and
    // bus keep working. (QML cannot observe findChildren()/destroyed() on
    // plain wrappers — the collection/baseline proof is the C++-side pin
    // testReplyGcHandoffCollection; this pin covers the real module path:
    // DBusQML.SessionBus singleton + plugin-registered types.)
    function test_pending_reply_gc_handoff() {
        var N = 8
        var finishedCount = 0
        for (var i = 0; i < N; i++) {
            let r = DBusQML.SessionBus.asyncCall({
                service: "org.freedesktop.DBus",
                path: "/org/freedesktop/DBus",
                iface: "org.freedesktop.DBus",
                member: "ListNames"
            })
            r.finished.connect(function() { finishedCount++ })
        }
        tryVerify(function() { return finishedCount === N; }, 10000,
                  "all replies should finish")
        gc()
        gc()
        // After gc() collected the dropped, handed-off replies: a fresh
        // call through the same singleton completes with real values.
        var r = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "NameHasOwner",
            arguments: ["org.freedesktop.DBus"]
        })
        tryVerify(function() { return r.isFinished; }, 5000)
        verify(!r.isError, "post-gc call should not error: " + r.error.message)
        compare(r.value, true, "post-gc call should return real values")

        // Capturing handlers — the canonical `r.finished.connect(() => r.value)`
        // shape: after delivery the library disconnects finished's receivers,
        // so the closure no longer roots its reply and gc() collects every one.
        // Control first: non-capturing handlers (proves the probe observes
        // collection). Each batch ends with one UNPROBED call through the
        // same call site: V4's call-site lookup cache keeps the most recent
        // receiver of `r.finished` alive, which must not be a probed reply.
        replyDestroyedCount = 0
        var plain = 0
        for (var k = 0; k <= N; k++)
            startPlainCall(k < N, function(names) { if (names.length > 0) plain++ })
        tryVerify(function() { return plain === N + 1; }, 10000, "plain handlers should all run")
        tryVerify(function() { gc(); return replyDestroyedCount === N; }, 10000,
                  "control: replies with non-capturing handlers should be collected")

        replyDestroyedCount = 0
        var captured = 0
        for (var j = 0; j <= N; j++)
            startCapturingCall(j < N, function(names) { if (names.length > 0) captured++ })
        tryVerify(function() { return captured === N + 1; }, 10000,
                  "capturing handlers should all read their reply's value")
        tryVerify(function() { gc(); return replyDestroyedCount === N; }, 10000,
                  "replies captured by their finished handler should be collected")

        // A binding on reply.value (finished is every reply property's
        // NOTIFY) still updates once on completion.
        boundReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "NameHasOwner",
            arguments: ["org.freedesktop.DBus"]
        })
        tryVerify(function() { return boundReplyValue === true; }, 5000,
                  "binding on reply.value should update on completion")
        boundReply = null
    }

    property int replyDestroyedCount: 0
    property DBusQML.DBusPendingReply boundReply
    readonly property var boundReplyValue: boundReply ? boundReply.value : undefined

    function countReplyDestroyed() { replyDestroyedCount++ }

    // A child probe reports a reply's destruction (QML does not expose
    // QObject::destroyed); a child never keeps its parent alive.
    function probeReplyDestruction(reply) {
        Qt.createQmlObject(
            "import QtQml; QtObject { Component.onDestruction: apiTest.countReplyDestroyed() }",
            reply, "replyDestroyedProbe")
    }

    function listNamesCall() {
        return DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames"
        })
    }

    // Control: the handler does not reference its reply.
    function startPlainCall(probed, onDone) {
        var r = listNamesCall()
        if (probed)
            probeReplyDestruction(r)
        r.finished.connect(function() { onDone([""]) })
    }

    // Own function scope: the handler's closure holds only `r` and `onValue`.
    function startCapturingCall(probed, onValue) {
        let r = listNamesCall()
        if (probed)
            probeReplyDestruction(r)
        r.finished.connect(function() { onValue(r.value) })
    }

    function test_properties_get() {
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus.Properties",
            member: "Get",
            arguments: ["org.freedesktop.DBus", "Features"],
        })
        verify(pendingReply !== null)
        tryVerify(() => pendingReply.isFinished, 5000)
        verify(!pendingReply.isError,
               "Properties.Get should not error: " + pendingReply.error.message)
        verify(pendingReply.isValid)
        verify(pendingReply.value !== undefined,
                "Properties.Get should return a value")
    }

    function test_properties_get_interfaces() {
        pendingReply = DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus.Properties",
            member: "Get",
            arguments: ["org.freedesktop.DBus", "Interfaces"],
        })
        verify(pendingReply !== null)
        tryVerify(() => pendingReply.isFinished, 5000)
        verify(!pendingReply.isError)
        verify(pendingReply.isValid)
        compare(typeof pendingReply.value, "object",
                "Properties.Get should unwrap QDBusVariant to an array")
        verify(pendingReply.value.length > 0,
               "Interfaces array should not be empty")
    }

    // ── Proxy call ─────────────────────────────────────────

    function test_proxy_call_ping() {
        rootProxy.call("ListNames")
        verify(true, "proxy.call(ListNames) completed without crash")
    }

    function test_property_write() {
        // Writing to any property should call updateValue without crash.
        dbusProxy.someProp = "test"
        verify(true, "property write completed without crash")
    }

    // ── Dynamic methods (introspection-based) ──────────────

    function test_dynamic_methods_exist() {
        // Wait for introspection to discover methods on the shared proxy.
        var found = false
        for (var i = 0; i < 10; ++i) {
            if (typeof dbusProxy.listNames === "function"
                && typeof dbusProxy.nameHasOwner === "function") {
                found = true
                break
            }
            wait(500)
        }
        verify(found, "dynamic methods should exist after introspection")
    }

    // fd helpers over DBusQML.unixFd: open/write/read plus the
    // file:///proc/self/fd/N URL form; the unixFd owns its descriptor and
    // close() is the contract.
    function test_fd_helpers_roundtrip() {
        var path = tempPath + "/dbusqml-fd-quartet.txt"
        var fd = DBusQML.DBusUtils.openFd(path, "w")
        verify(fd.valid, "openFd w failed")
        compare(DBusQML.DBusUtils.writeFd(fd, "fd-quartet-ok\n"), 14, "writeFd byte count")
        fd.close()
        verify(!fd.valid, "close() invalidates")
        var fd2 = DBusQML.DBusUtils.openFd(path, "r")
        verify(fd2.valid, "openFd r failed")
        var data = DBusQML.DBusUtils.readFd(fd2, 4096)
        verify(data instanceof ArrayBuffer, "readFd returns ArrayBuffer")
        verify(data.byteLength === 14, "readFd byteLength: " + data.byteLength)
        compare(DBusQML.DBusUtils.textFromBytes(data), "fd-quartet-ok\n")
        fd2.close()
    }
    function test_fd_url() {
        var path = tempPath + "/dbusqml-fdurl.txt"
        var fd = DBusQML.DBusUtils.openFd(path, "rw")
        verify(fd.valid, "openFd rw failed")
        var url = DBusQML.DBusUtils.fdUrl(fd)
        verify(/^file:\/\/\/proc\/self\/fd\/[0-9]+$/.test(url), "fdUrl shape: " + url)
        compare(DBusQML.DBusUtils.writeFd(fd, "FDURL-OK"), 8, "writeFd")
        // The URL must resolve as a real filesystem path for path-based
        // consumers: read /proc/self/fd/N back through openFd+readFd.
        // (XHR on file:// is non-functional in this runtime, so it is not a
        // valid probe.)
        var pfd = DBusQML.DBusUtils.openFd(url.substring(7), "r")
        verify(pfd.valid, "openFd through " + url + " failed")
        var data = DBusQML.DBusUtils.readFd(pfd, 4096)
        pfd.close()
        compare(DBusQML.DBusUtils.textFromBytes(data), "FDURL-OK", "fdUrl read-through content")
        fd.close()
        compare(DBusQML.DBusUtils.fdUrl(fd), "", "no URL for a closed unixFd")
    }
    function test_fd_error_paths() {
        ignoreWarning(/DBusUtils::openFd: failed to open/)
        var missing = DBusQML.DBusUtils.openFd("/nonexistent/dbusqml-test", "r")
        verify(!missing.valid, "a failed open is an invalid unixFd")
        ignoreWarning(/DBusUtils::writeFd: the DBusQML.unixFd is not valid/)
        compare(DBusQML.DBusUtils.writeFd(missing, "x"), -1)
        ignoreWarning(/DBusUtils::readFd: the DBusQML.unixFd is not valid/)
        verify(DBusQML.DBusUtils.readFd(missing, 100).byteLength === 0)
        ignoreWarning(/DBusUtils::fdUrl: the DBusQML.unixFd is not valid/)
        compare(DBusQML.DBusUtils.fdUrl(missing), "")
        missing.close() // idempotent, must not crash
    }
    // A plain number is never an fd: the helpers refuse it (no coercion of
    // the number into a descriptor — 3 might be anything, 0 is stdin).
    function test_fd_helpers_refuse_numbers() {
        var path = tempPath + "/dbusqml-fd-number.txt"
        var w = DBusQML.DBusUtils.openFd(path, "w")
        var wn = (typeof w === "number") ? w : w.take()
        DBusQML.DBusUtils.writeFd(w, "not-through-a-number")
        var f = DBusQML.DBusUtils.openFd(path, "r")
        var n = (typeof f === "number") ? f : f.take()
        verify(n >= 3, "a readable number to misuse: " + n)
        ignoreWarning(/DBusUtils::readFd: expects a DBusQML.unixFd/)
        var data = DBusQML.DBusUtils.readFd(n, 64)
        compare(data.byteLength, 0, "readFd(number) must refuse, not read fd " + n)
        ignoreWarning(/DBusUtils::writeFd: expects a DBusQML.unixFd/)
        compare(DBusQML.DBusUtils.writeFd(wn, "x"), -1, "writeFd(number) must refuse")
        ignoreWarning(/DBusUtils::fdUrl: expects a DBusQML.unixFd/)
        compare(DBusQML.DBusUtils.fdUrl(n), "", "fdUrl(number) must refuse")
        DBusQML.DBusUtils.closeFd(n)
        DBusQML.DBusUtils.closeFd(wn)
        if (typeof f !== "number") { f.close(); w.close() }
    }
    // DBusQML.unixFd value semantics: construction dups (the caller keeps
    // its number), copies share one state, take() hands out a caller-owned
    // duplicate, toString() never shows the number.
    function test_unixfd_value() {
        var path = tempPath + "/dbusqml-unixfd.txt"
        var src = DBusQML.DBusUtils.openFd(path, "rw")
        DBusQML.DBusUtils.writeFd(src, "unixfd-value")
        var n = src.take()
        verify(n >= 3, "take() returns a duplicate number >= 3: " + n)
        verify(src.valid, "take() leaves the unixFd open")
        var made = new DBusQML.unixFd(n)
        verify(made.valid, "new DBusQML.unixFd(n) owns a dup")
        DBusQML.DBusUtils.closeFd(n) // the caller's own number
        verify(made.valid, "closing the caller's number leaves the unixFd's dup open")
        compare(made.toString(), "DBusQML.unixFd(valid)")
        var copy = made
        holder.held = made
        holder.held.close()
        verify(!made.valid && !copy.valid && !holder.held.valid, "close() on one copy closes all")
        compare(made.toString(), "DBusQML.unixFd(invalid)")
        ignoreWarning(/DBusQML.unixFd.take\(\): the fd is not valid/)
        compare(made.take(), -1)
        ignoreWarning(/DBusQML.unixFd expects a file descriptor number/)
        verify(!(new DBusQML.unixFd("3")).valid, "a string is not an fd number")
        src.close()
        holder.held = undefined
    }
    QtObject {
        id: holder
        property var held
    }
    function test_dynamic_method_call() {
        for (var i = 0; i < 10; ++i) {
            if (typeof dbusProxy.listNames === "function") break
            wait(500)
        }
        verify(typeof dbusProxy.listNames === "function",
               "ListNames should exist after introspection")

        var reply = dbusProxy.listNames()
        verify(reply !== undefined, "dynamic method should return a value")
        verify(typeof reply.finished !== "undefined",
               "returned value should have a finished signal")
        tryVerify(() => reply.isFinished, 5000,
                  "ListNames should finish within timeout")
        verify(!reply.isError, "ListNames should not error")
        verify(reply.isValid, "reply should be valid")
    }

    function test_dynamic_method_with_args() {
        for (var i = 0; i < 10; ++i) {
            if (typeof dbusProxy.nameHasOwner === "function") break
            wait(500)
        }
        verify(typeof dbusProxy.nameHasOwner === "function",
               "NameHasOwner should exist after introspection")

        var reply = dbusProxy.nameHasOwner(new DBusQML.string("org.freedesktop.DBus"))
        verify(reply !== undefined, "dynamic method should return a value")
        tryVerify(() => reply.isFinished, 5000)
        verify(!reply.isError, "NameHasOwner should not error")
        compare(reply.value, true, "DBus should have an owner")
    }

    // ── Promise-style asyncCall ────────────────────────────
    //
    // resolve callback must receive the reply's native value (bool, array,
    // etc.) rather than a stringified version. reject callback must receive
    // a single { name, message } object rather than two positional args.

    function test_promise_resolve_native_bool() {
        var resolved = null
        DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "NameHasOwner",
            arguments: ["org.freedesktop.DBus"],
        },
        function(v) { resolved = v },
        function(e) { resolved = "REJECTED" })
        tryVerify(() => resolved !== null, 5000)
        compare(typeof resolved, "boolean",
                "resolve must receive native bool, not stringified value")
        compare(resolved, true, "DBus itself should always have an owner")
    }

    function test_promise_resolve_native_array() {
        var resolved = null
        DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/org/freedesktop/DBus",
            iface: "org.freedesktop.DBus",
            member: "ListNames",
        },
        function(v) { resolved = v },
        function(e) { resolved = "REJECTED" })
        tryVerify(() => resolved !== null, 5000)
        verify(Array.isArray(resolved),
               "resolve must receive a native array, not a stringified value")
        verify(resolved.length > 0, "ListNames should return at least one name")
    }

    function test_promise_reject_error_object() {
        var rejected = null
        DBusQML.SessionBus.asyncCall({
            service: "org.freedesktop.DBus",
            path: "/",
            iface: "org.freedesktop.DBus",
            member: "NonExistentMethod",
        },
        function(v) { rejected = "RESOLVED" },
        function(e) { rejected = e })
        tryVerify(() => rejected !== null, 5000)
        compare(typeof rejected, "object",
                "reject must receive a single error object")
        verify(typeof rejected.name === "string",
               "error object must have a name property")
        verify(typeof rejected.message === "string",
               "error object must have a message property")
    }

    // ── DBusUtils ─────────────────────────────────────────

    function test_dbusutils_singleton() {
        compare(typeof DBusQML.DBusUtils, "object",
                "DBusUtils singleton should be available")
        compare(typeof DBusQML.DBusUtils.textFromBytes, "function",
                "textFromBytes should be a function")
        compare(typeof DBusQML.DBusUtils.bytesFromText, "function",
                "bytesFromText should be a function")
    }

    function test_dbusutils_text_roundtrip() {
        var buf = DBusQML.DBusUtils.bytesFromText("Hello")
        verify(buf instanceof ArrayBuffer, "bytesFromText returns ArrayBuffer")
        compare(buf.byteLength, 5, "byteLength matches UTF-8 length")

        var text = DBusQML.DBusUtils.textFromBytes(buf)
        compare(text, "Hello", "textFromBytes decodes UTF-8")
    }

    function test_dbusutils_utf8_multibyte() {
        var original = "Мой Wifi — 日本語"
        var buf = DBusQML.DBusUtils.bytesFromText(original)
        var decoded = DBusQML.DBusUtils.textFromBytes(buf)
        compare(decoded, original, "multibyte UTF-8 round-trip")
    }

    function test_dbusutils_text_from_number_array() {
        // Number array input (legacy path / migration)
        var text = DBusQML.DBusUtils.textFromBytes([72, 101, 108, 108, 111])
        compare(text, "Hello", "textFromBytes handles number arrays")
    }

    function test_dbusutils_bytes_type() {
        // DBus.bytes value type exists for explicit ay override
        var b = new DBusQML.bytes("test")
        verify(b !== undefined, "DBus.bytes constructible")
        compare(b.toString(), "test", "bytes toString")
    }
}

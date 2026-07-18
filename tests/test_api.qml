import QtQuick
import QtTest
import DBus 1.0 as DBusQML

TestCase {
    name: "DBusQmlApiTest"

    property DBusQML.DBusPendingReply pendingReply

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
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/"; iface: "org.freedesktop.DBus" }',
            this
        )
        compare(proxy.service, "org.freedesktop.DBus", "DBus service property")
        compare(proxy.path, "/", "DBus path property")
        compare(proxy.iface, "org.freedesktop.DBus", "DBus iface property")
        proxy.destroy()
    }

    function test_proxy_call_exists() {
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/"; iface: "org.freedesktop.DBus" }',
            this
        )
        compare(typeof proxy.call, "function", "call should be a function")
        proxy.destroy()
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
            member: "ListNames",
        }, function(result) {
            done = true
        }, function(error) {
            done = true
        })
        tryVerify(function() { return done; }, 5000,
                  "promise-style call should complete")
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
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/"; iface: "org.freedesktop.DBus" }',
            this
        )
        proxy.call("ListNames")
        verify(true, "proxy.call(ListNames) completed without crash")
        proxy.destroy()
    }

    function test_property_write() {
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/org/freedesktop/DBus"; iface: "org.freedesktop.DBus" }',
            this
        )
        // Writing to any property should call updateValue without crash
        proxy.someProp = "test"
        verify(true, "property write completed without crash")
        proxy.destroy()
    }

    // ── Dynamic methods (introspection-based) ──────────────

    function test_dynamic_methods_exist() {
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/org/freedesktop/DBus"; iface: "org.freedesktop.DBus" }',
            this
        )
        // Wait for introspection to discover methods
        var found = false
        for (var i = 0; i < 10; ++i) {
            if (typeof proxy.listNames === "function"
                && typeof proxy.nameHasOwner === "function") {
                found = true
                break
            }
            wait(500)
        }
        verify(found, "dynamic methods should exist after introspection")
        proxy.destroy()
    }

    function test_dynamic_method_call() {
        console.warn("SPIKE dmc 1: entering")
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/org/freedesktop/DBus"; iface: "org.freedesktop.DBus" }',
            this
        )
        console.warn("SPIKE dmc 2: createQmlObject returned, proxy =", proxy)
        // Wait for introspection
        for (var i = 0; i < 10; ++i) {
            if (typeof proxy.listNames === "function") break
            wait(500)
        }
        console.warn("SPIKE dmc 3: post-wait iters=" + i + " typeof=" + (typeof proxy.listNames))
        verify(typeof proxy.listNames === "function",
               "ListNames should exist after introspection")
        console.warn("SPIKE dmc 4: verified callable")

        // Dynamic method should return a DBusPendingReply
        var reply = proxy.listNames()
        console.warn("SPIKE dmc 5: called listNames, reply =", reply)
        verify(reply !== undefined, "dynamic method should return a value")
        verify(typeof reply.finished !== "undefined",
               "returned value should have a finished signal")
        console.warn("SPIKE dmc 6: about to tryVerify isFinished")
        tryVerify(() => reply.isFinished, 5000,
                  "ListNames should finish within timeout")
        console.warn("SPIKE dmc 7: post-tryVerify")
        verify(!reply.isError, "ListNames should not error")
        verify(reply.isValid, "reply should be valid")
        // SPIKE: destroy() removed to see if crash moves. If it does, the
        // SIGSEGV is triggered by destroy() itself. If it stays, something
        // else in the finalize path (queued callbacks, GC) is the cause.
        console.warn("SPIKE dmc 8: skipping destroy() this round")
        console.warn("SPIKE dmc 9: end of test")
    }

    function test_dynamic_method_with_args() {
        var proxy = Qt.createQmlObject(
            'import DBus 1.0; DBus { service: "org.freedesktop.DBus"; path: "/org/freedesktop/DBus"; iface: "org.freedesktop.DBus" }',
            this
        )
        // Wait for introspection
        for (var i = 0; i < 10; ++i) {
            if (typeof proxy.nameHasOwner === "function") break
            wait(500)
        }
        verify(typeof proxy.nameHasOwner === "function",
               "NameHasOwner should exist after introspection")

        // Dynamic method with typed argument should return a DBusPendingReply
        var reply = proxy.nameHasOwner(new DBusQML.string("org.freedesktop.DBus"))
        verify(reply !== undefined, "dynamic method should return a value")
        tryVerify(() => reply.isFinished, 5000)
        verify(!reply.isError, "NameHasOwner should not error")
        compare(reply.value, true, "DBus should have an owner")
        proxy.destroy()
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
}

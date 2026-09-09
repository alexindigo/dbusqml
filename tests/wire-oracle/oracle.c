/* Wire oracle — raw libdbus peer (Nemo dbustestd pattern).
 *
 * Serves org.dbusqml.Oracle at /Oracle with:
 *   Echo(s) -> s            (returns its arg)
 *   Repr(...) -> s           (returns the call's signature string)
 *   Ping() -> s              ("pong")
 *   ReplyCount(u) -> u       (replies EMITTED by this oracle for incoming
 *                             serial u — the oracle's own service-handler
 *                             direction; tallied only in the Echo/Repr/Ping
 *                             handlers below)
 *   ReceivedCount(u) -> u    (replies RECEIVED by this oracle for outgoing
 *                             serial u — the caller direction; tallied by the
 *                             filter below whenever the ORACLE is the caller
 *                             and a METHOD_RETURN/METHOD_ERROR arrives whose
 *                             reply_serial matches. This is the direction that
 *                             observes an adaptor under test: the oracle
 *                             invokes the adaptor and counts what came back.)
 *   CallAdaptor(s, s, s, s) -> (uu) (caller mode: invokes the named
 *                             adaptor method (service, path, iface, member),
 *                             returns (outgoing_serial, replies_received).
 *                             The caller reads the reply to learn the serial,
 *                             then queries ReceivedCount(serial).)
 *   DoubleSend(s, s, s, s) -> (uu) (sensitivity self-test: invokes the
 *                             named adaptor method TWICE on the same outgoing
 *                             serial is impossible on the bus — instead it
 *                             performs two sequential calls and returns both
 *                             serials; a harness asserting per-serial count
 *                             == 1 for each proves the counter is live, and
 *                             a deliberately double-replying service shows
 *                             count == 2 for one serial. MUST be flagged by
 *                             the self-test when pointed at a double-sender.)
 *
 * D-Bus correlation semantics (no invention — this is the spec's own
 * model): a METHOD_RETURN/METHOD_ERROR carries a reply_serial header
 * matching the serial of the METHOD_CALL it answers. The caller learns
 * its outgoing serial from the reply itself
 * (dbus_message_get_reply_serial); the callee learns the incoming
 * serial from the call (dbus_message_get_serial). Counting replies
 * keyed by reply_serial on the CALLER side is therefore exactly "how
 * many replies did serial N produce" — the exactly-one-reply
 * observable. ReplyCount (callee side) and ReceivedCount (caller side)
 * are the same keying applied to the two directions; neither replaces
 * the other.
 *
 * Build: cmake -S . -B build && cmake --build build
 * Needs libdbus-1 dev headers. Qt-free by design.
 */
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SERIALS 1048576

static unsigned s_emitCounts[MAX_SERIALS];
static unsigned s_recvCounts[MAX_SERIALS];
static dbus_uint32_t s_lastSeen = 0;

static DBusHandlerResult oracle_filter(DBusConnection *conn, DBusMessage *msg, void *data);

static void record_emit(dbus_uint32_t serial) {
    if (serial < MAX_SERIALS)
        s_emitCounts[serial]++;
    if (serial > s_lastSeen)
        s_lastSeen = serial;
}

/* Caller direction: a METHOD_RETURN/METHOD_ERROR arriving at the oracle
 * answers one of the oracle's own outgoing calls. Key by reply_serial —
 * the spec's correlation header — so ReceivedCount(N) is "how many
 * replies did my call serial N produce". Exactly one per well-behaved
 * callee; two exposes a double-reply; zero exposes a swallow. */
static void record_recv(dbus_uint32_t replySerial) {
    if (replySerial < MAX_SERIALS)
        s_recvCounts[replySerial]++;
}

static DBusMessage *handle_echo(DBusMessage *msg, DBusConnection *conn) {
    const char *arg = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID)) {
        DBusMessage *e = dbus_message_new_error(msg, "org.dbusqml.Oracle.Error", "want s");
        dbus_error_free(&err);
        return e;
    }
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
    record_emit(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_repr(DBusMessage *msg, DBusConnection *conn) {
    const char *sig = dbus_message_get_signature(msg);
    if (!sig)
        sig = "";
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &sig, DBUS_TYPE_INVALID);
    record_emit(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_ping(DBusMessage *msg, DBusConnection *conn) {
    const char *pong = "pong";
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &pong, DBUS_TYPE_INVALID);
    record_emit(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_reply_count(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &serial, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    dbus_uint32_t n = serial < MAX_SERIALS ? s_emitCounts[serial] : 0;
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
    /* The count query itself is not tallied (it is meta, not a reply
     * under test) — but record it as seen for diagnostics. */
    if (serial > s_lastSeen)
        s_lastSeen = serial;
    (void)conn;
    return reply;
}

static DBusMessage *handle_received_count(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &serial, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    dbus_uint32_t n = serial < MAX_SERIALS ? s_recvCounts[serial] : 0;
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
    if (serial > s_lastSeen)
        s_lastSeen = serial;
    (void)conn;
    return reply;
}

/* Caller mode: synchronously invoke (service, path, iface, member) with
 * no args and report (outgoing_serial, replies_received_so_far). The
 * harness then pumps the bus / waits and polls ReceivedCount(serial)
 * until it settles at 1 (exactly-one-reply) — or observes 0 (swallow)
 * or 2 (double-reply). Blocking with a bounded timeout so a swallowed
 * reply fails loud, never hangs the harness. */
static DBusMessage *handle_call_adaptor(DBusMessage *msg, DBusConnection *conn) {
    const char *service = "";
    const char *path = "";
    const char *iface = "";
    const char *member = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &service, DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &member,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *e = dbus_message_new_error(msg, "org.dbusqml.Oracle.Error", "want ssss");
        dbus_error_free(&err);
        return e;
    }
    dbus_error_free(&err);
    DBusMessage *call = dbus_message_new_method_call(service, path, iface, member);
    if (!call) {
        return dbus_message_new_error(msg, "org.dbusqml.Oracle.Error", "cannot build call");
    }
    dbus_uint32_t outSerial = 0;
    DBusError replyErr;
    dbus_error_init(&replyErr);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &replyErr);
    if (reply) {
        outSerial = dbus_message_get_reply_serial(reply);
        record_recv(dbus_message_get_reply_serial(reply));
        dbus_message_unref(reply);
    } else {
        /* Timed out or errored with no reply message: still report the
         * outgoing serial so ReceivedCount(serial)==0 pins the swallow. */
        outSerial = dbus_message_get_serial(call);
        dbus_error_free(&replyErr);
    }
    dbus_message_unref(call);
    DBusMessage *ret = dbus_message_new_method_return(msg);
    dbus_uint32_t got = outSerial < MAX_SERIALS ? s_recvCounts[outSerial] : 0;
    dbus_message_append_args(ret, DBUS_TYPE_UINT32, &outSerial, DBUS_TYPE_UINT32, &got,
                             DBUS_TYPE_INVALID);
    return ret;
}

/* Sensitivity self-test helper: two sequential calls at two serials.
 * Returns (serial_a, serial_b); the harness asserts ReceivedCount == 1
 * for each. When pointed at a deliberately double-replying service
 * (see below), one serial shows 2 — proving the counter is live and
 * would flag a real double-send instead of passing vacuously. */
static DBusMessage *handle_double_send(DBusMessage *msg, DBusConnection *conn) {
    const char *service = "";
    const char *path = "";
    const char *iface = "";
    const char *member = "";
    DBusError err;
    dbus_error_init(&err);
    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &service, DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &member,
                               DBUS_TYPE_INVALID)) {
        DBusMessage *e = dbus_message_new_error(msg, "org.dbusqml.Oracle.Error", "want ssss");
        dbus_error_free(&err);
        return e;
    }
    dbus_error_free(&err);
    dbus_uint32_t serials[2] = {0, 0};
    for (int i = 0; i < 2; ++i) {
        DBusMessage *call = dbus_message_new_method_call(service, path, iface, member);
        if (!call)
            break;
        DBusError replyErr;
        dbus_error_init(&replyErr);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &replyErr);
        if (reply) {
            serials[i] = dbus_message_get_reply_serial(reply);
            record_recv(dbus_message_get_reply_serial(reply));
            dbus_message_unref(reply);
        } else {
            serials[i] = dbus_message_get_serial(call);
            dbus_error_free(&replyErr);
        }
        dbus_message_unref(call);
    }
    DBusMessage *ret = dbus_message_new_method_return(msg);
    dbus_message_append_args(ret, DBUS_TYPE_UINT32, &serials[0], DBUS_TYPE_UINT32, &serials[1],
                             DBUS_TYPE_INVALID);
    return ret;
}

int main(void) {
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "oracle: no session bus: %s\n", err.message);
        return 1;
    }
    int rc = dbus_bus_request_name(conn, "org.dbusqml.Oracle", DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        fprintf(stderr, "oracle: cannot own name\n");
        return 1;
    }
    printf("ORACLE-READY\n");
    fflush(stdout);
    dbus_connection_add_filter(conn, oracle_filter, NULL, NULL);
    while (dbus_connection_read_write_dispatch(conn, 100)) {
    }
    return 0;
}

static DBusHandlerResult oracle_filter(DBusConnection *conn, DBusMessage *msg, void *data) {
    (void)data;
    /* Caller direction: any reply arriving at the oracle answers one of
     * its own outgoing calls. Tally by reply_serial BEFORE dispatching —
     * this is what makes ReceivedCount observe the adaptor under test. */
    const int mtype = dbus_message_get_type(msg);
    if (mtype == DBUS_MESSAGE_TYPE_METHOD_RETURN || mtype == DBUS_MESSAGE_TYPE_ERROR) {
        record_recv(dbus_message_get_reply_serial(msg));
        /* Replies to OUR calls are consumed by the blocking
         * send_with_reply_and_block above, not by this filter — but the
         * tally must happen regardless of who consumes them. */
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
    if (mtype != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (!dbus_message_has_interface(msg, "org.dbusqml.Oracle"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    {
        const char *member = dbus_message_get_member(msg);
        DBusMessage *reply = NULL;
        if (!strcmp(member, "Echo"))
            reply = handle_echo(msg, conn);
        else if (!strcmp(member, "Repr"))
            reply = handle_repr(msg, conn);
        else if (!strcmp(member, "Ping"))
            reply = handle_ping(msg, conn);
        else if (!strcmp(member, "ReplyCount"))
            reply = handle_reply_count(msg, conn);
        else if (!strcmp(member, "ReceivedCount"))
            reply = handle_received_count(msg, conn);
        else if (!strcmp(member, "CallAdaptor"))
            reply = handle_call_adaptor(msg, conn);
        else if (!strcmp(member, "DoubleSend"))
            reply = handle_double_send(msg, conn);
        else
            reply = dbus_message_new_error(msg, "org.freedesktop.DBus.Error.UnknownMethod",
                                           "no such method");
        if (reply) {
            dbus_connection_send(conn, reply, NULL);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
    }
    return DBUS_HANDLER_RESULT_HANDLED;
}

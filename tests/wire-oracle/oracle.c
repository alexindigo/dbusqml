/* Wire oracle — raw libdbus peer (Nemo dbustestd pattern).
 *
 * Serves org.dbusqml.Oracle at /Oracle with:
 *   Echo(s) -> s            (returns its arg)
 *   Repr(...) -> s           (returns the call's signature string)
 *   Ping() -> s              ("pong")
 *   ReplyCount(u) -> u       (replies emitted for incoming serial u)
 *
 * Build: cmake -S . -B build && cmake --build build
 * Needs libdbus-1 dev headers. Qt-free by design.
 */
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SERIALS 4096

static unsigned s_counts[MAX_SERIALS];
static dbus_uint32_t s_lastSeen = 0;

static DBusHandlerResult oracle_filter(DBusConnection *conn, DBusMessage *msg, void *data);

static void record_reply(dbus_uint32_t serial) {
    if (serial < MAX_SERIALS)
        s_counts[serial]++;
    if (serial > s_lastSeen)
        s_lastSeen = serial;
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
    record_reply(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_repr(DBusMessage *msg, DBusConnection *conn) {
    const char *sig = dbus_message_get_signature(msg);
    if (!sig)
        sig = "";
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &sig, DBUS_TYPE_INVALID);
    record_reply(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_ping(DBusMessage *msg, DBusConnection *conn) {
    const char *pong = "pong";
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_STRING, &pong, DBUS_TYPE_INVALID);
    record_reply(dbus_message_get_serial(msg));
    (void)conn;
    return reply;
}

static DBusMessage *handle_reply_count(DBusMessage *msg, DBusConnection *conn) {
    dbus_uint32_t serial = 0;
    DBusError err;
    dbus_error_init(&err);
    dbus_message_get_args(msg, &err, DBUS_TYPE_UINT32, &serial, DBUS_TYPE_INVALID);
    dbus_error_free(&err);
    dbus_uint32_t n = serial < MAX_SERIALS ? s_counts[serial] : 0;
    DBusMessage *reply = dbus_message_new_method_return(msg);
    dbus_message_append_args(reply, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
    /* The count query itself is not tallied (it is meta, not a reply
     * under test) — but record it as seen for diagnostics. */
    if (serial > s_lastSeen)
        s_lastSeen = serial;
    (void)conn;
    return reply;
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
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
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

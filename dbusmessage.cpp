#include "dbusmessage.h"

DBusMessage::DBusMessage(const QVariantMap &data) {
    if (data.contains("service"))  m_service = data["service"].toString();
    if (data.contains("path"))     m_path = data["path"].toString();
    if (data.contains("iface"))    m_iface = data["iface"].toString();
    if (data.contains("member"))   m_member = data["member"].toString();
    if (data.contains("signature")) m_signature = data["signature"].toString();
    if (data.contains("arguments")) m_arguments = data["arguments"].toList();
}

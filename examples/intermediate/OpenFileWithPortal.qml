import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0

Window {
    id: root
    visible: true
    width: 600
    height: 400
    title: "DBus — Open a file with the portal"

    property bool busy: false
    property string statusText: "Choose an existing file, then send its descriptor to the portal."

    function openFile(localPath) {
        if (root.busy)
            return
        const fd = DBusUtils.openFd(localPath, "r")
        if (!fd.valid) {
            root.statusText = "Could not open " + localPath + ". Check the path and read permissions."
            return
        }
        root.busy = true
        root.statusText = "Sending the file descriptor…"
        const reply = SessionBus.asyncCall({
            service: "org.freedesktop.portal.Desktop",
            path: "/org/freedesktop/portal/desktop",
            iface: "org.freedesktop.portal.OpenURI",
            member: "OpenFile",
            // No exported parent window; an empty options map is a{sv}.
            arguments: ["", fd, {}],
            signature: "sha{sv}",
            timeout: 5000
        })
        // asyncCall captured the outgoing fd. We own no long-lived resource.
        fd.close()
        reply.finished.connect(function() {
            root.busy = false
            if (reply.isError) {
                root.statusText = reply.error.name + ": " + reply.error.message
                return
            }
            root.statusText = "Portal request handle: " + reply.value
                + "\n\nThe local fd is closed. A handle means the request was accepted, not that the file opened. This example does not monitor the later Request.Response signal."
        })
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: "Send a file, close your copy"
            font.pixelSize: 22
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Uses OpenURI.OpenFile on the session bus, not FileChooser.OpenFile. A portal frontend, backend and file handler are needed. The interaction has no parent window."
        }
        TextField {
            id: pathField
            Layout.fillWidth: true
            text: "/etc/hostname"
            placeholderText: "Absolute local file path (not a file: URL)"
            enabled: !root.busy
            onAccepted: root.openFile(text)
        }
        Button {
            text: root.busy ? "Sending…" : "Open with portal"
            enabled: !root.busy && pathField.text.length > 0
            onClicked: root.openFile(pathField.text)
        }
        TextArea {
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: String(root.statusText)
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
        }
    }
}

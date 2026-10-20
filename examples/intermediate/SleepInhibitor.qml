import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import DBus 1.0

Window {
    id: root
    visible: true
    width: 560
    height: 340
    title: "DBus — Sleep inhibitor"

    property var inhibitor: null
    property bool requested: false
    property bool busy: false
    property string statusText: "Sleep is not inhibited by this example."

    function release() {
        root.requested = false
        if (root.inhibitor) {
            root.inhibitor.close()
            root.inhibitor = null
        }
        root.statusText = root.busy ? "Waiting for logind; the returned fd will be closed."
                                    : "Released. Sleep is not inhibited by this example."
    }

    function setRequested(enabled) {
        if (!enabled) {
            root.release()
            return
        }
        root.requested = true
        if (root.busy || root.inhibitor)
            return
        root.busy = true
        root.statusText = "Requesting a sleep inhibitor from logind…"
        const reply = SystemBus.asyncCall({
            service: "org.freedesktop.login1",
            path: "/org/freedesktop/login1",
            iface: "org.freedesktop.login1.Manager",
            member: "Inhibit",
            arguments: ["sleep", "dbusqml-sleep-inhibitor", "User enabled the sleep inhibitor", "block"],
            signature: "ssss",
            timeout: 5000
        })
        reply.finished.connect(function() {
            root.busy = false
            if (reply.isError) {
                root.requested = false
                root.statusText = reply.error.name + ": " + reply.error.message
                return
            }
            const fd = reply.value
            if (!fd || !fd.valid) {
                root.requested = false
                root.statusText = "logind returned an invalid file descriptor."
                return
            }
            // The toggle may have been turned off while the call was pending.
            if (!root.requested) {
                fd.close()
                root.statusText = "Released the inhibitor received after cancellation."
                return
            }
            root.inhibitor = fd
            root.statusText = "Sleep inhibited. The unixFd stays open until you switch off or close this window."
        })
    }

    onClosing: root.release()
    Component.onDestruction: root.release()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Label {
            text: "Hold an fd, hold an inhibitor"
            font.pixelSize: 22
            font.bold: true
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: "Calls logind Inhibit on the system bus. This blocks sleep while the descriptor is open; it does not prevent screen blanking or request sleep. System policy may deny the call."
        }
        Switch {
            text: root.inhibitor ? "Sleep inhibitor held" : "Prevent sleep"
            checked: root.requested
            onToggled: root.setRequested(checked)
        }
        BusyIndicator {
            running: root.busy
            visible: root.busy
            Layout.alignment: Qt.AlignHCenter
        }
        TextArea {
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: root.statusText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
        }
    }
}

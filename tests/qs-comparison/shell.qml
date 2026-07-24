import QtQuick
import Quickshell
import Quickshell.Networking

// Reactive-binding comparison spike: verify whether Quickshell exhibits
// the same undefined-then-not-reactive behavior dbusqml sees when an
// intermediate `readonly property` wraps an async-populated DBus property.
//
// Direct: reads Networking.wifiEnabled directly.
// Wrapper: reads it via an intermediate readonly property (the pattern
//          that breaks in dbusqml).
ShellRoot {
    id: root

    // Direct binding — Qt sees each access individually.
    property var direct: Networking.wifiEnabled

    // Wrapper binding — intermediate readonly property. This is the
    // pattern that resolves to `false` forever in dbusqml.
    readonly property bool wrapper: Networking.wifiEnabled === true

    Component.onCompleted: {
        console.log("SPIKE t+0 direct=", root.direct,
                    "  wrapper=", root.wrapper,
                    "  raw=", Networking.wifiEnabled);
    }

    Timer {
        interval: 2000
        running: true
        repeat: false
        onTriggered: {
            console.log("SPIKE t+2 direct=", root.direct,
                        "  wrapper=", root.wrapper,
                        "  raw=", Networking.wifiEnabled);
            Qt.quit();
        }
    }
}

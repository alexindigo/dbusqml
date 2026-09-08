import DBus 1.0

// T1 relay gate G3 helper (committed, staged next to the test binary at
// runtime): the re-entrant onNameAcquired handler createObjects a
// same-service child through this file — genuine attach re-entrancy
// from inside delivery, not just a createComponent probe.
DBusAdaptor {
    service: 'org.dbusqml.T1G3'
    path: '/T1G3Child'
    iface: 'org.dbusqml.T1G3'
    allowReplacement: true
    function ping() { return 'kid' }
}

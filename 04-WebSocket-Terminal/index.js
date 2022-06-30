var terminal;
this.onerror = alert;
cmd = prompt("process name(e.g. wsl, bash, cmd, powershell)", "cmd");
if (!cmd) {
    throw "error: user refuse to enter process name";
}
var ws = new WebSocket('ws://' + location.host, cmd);
ws.onopen = () => {
    terminal.write('* * * connection established * * *\r\n');
};
terminal = new Terminal({
    fontFamily: "'Source Sans Pro', 'Lucida Console','Source Code Pro', 'monospace'",
    cursorBlink: true,
    scrollback: 1000,
    windowsMode: true,
    bellStyle: "sound",

});
terminal.open(document.body);
ws.onclose = e => {
    if (e.reason != '') {
        terminal.write("\r\n* * * connection closed * * *\r\n"+e.reason);
    }
    else {
        terminal.write('\r\n* * *connection closed...* * *\r\n' + e.code);
    }
};
ws.onerror = console.error;
ws.onmessage = (m) => {
    m.data.startsWith && terminal.write(m.data);
    m.data.text && m.data.text().then((t) => {
        terminal.write(t);
    });
};
var buf = String();
terminal.onData((e) => {
    switch (e) {
        case '\u0003':
            terminal.write('^C');
            terminal.write("\r\n");
            ws.send(e);
            break;
        case '\u0004':
            terminal.write('^D');
            terminal.write("\r\n");
            ws.send(e);
            break;
        case '\r':
            ws.send(buf + '\n');
            terminal.write("\r\n");
            buf = "";
            break;
        case '\u007F':
            if (terminal._core.buffer.x > 2) {
                terminal.write('\b \b');
                if (buf.length > 0) {
                    buf = buf.substr(0, buf.length - 1);
                }
            }
            break;
        default:
            if (e >= String.fromCharCode(0x20) && e <= String.fromCharCode(0x7E) || e >= '\u00a0') {
                buf += e;
                terminal.write(e);
            }
    }
});
const fitAddon = new FitAddon.FitAddon();
terminal.loadAddon(fitAddon);
fitAddon.fit();
terminal.focus();

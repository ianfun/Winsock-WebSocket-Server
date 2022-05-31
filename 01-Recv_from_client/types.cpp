#define Nthreads 3
#pragma comment(lib, "WS2_32")
#pragma comment(lib, "Mswsock") 

#define CRLF "\r\n"

template <ULONG N>
consteval ULONG cstrlen(const char(&)[N]) {
    return N - 1;
}
consteval ULONG cstrlen(const char* s) {
    ULONG res=0;
    for (; *s; s++) {
        res++;
    }
    return res;
}

struct Request {
    Request() : headers(), uri{}, parse_data{}, client(0), buf{ 0 }, parser{}{};
    ~Request() = default;
    llhttp_t parser;
    char buf[65535+8];
    std::map<std::string, std::string> headers;
    std::string uri; // init after
    SOCKET client;
    struct {
        size_t length;
        const char* at;
    } parse_data;
};
namespace Websocket {
    using BIT = BYTE;
    enum class Opcode: BYTE {
        Continuation=0,
        Text=0x1,
        Binary=0x2,
        Close=0x8,
        Ping=0x9,
        Pong=0xA
    };
}

constexpr const char* indexHtml = "HTTP/1.1 200 OK" CRLF
                                  "Content-Type: text/html" CRLF CRLF
"<!DOCTYPE html>\
<html>\
<head>\
    <meta charset=\"utf-8\">\
    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\
    <title>Terminal</title>\
    <link rel=\"stylesheet\" type=\"text/css\" href=\"https://cdnjs.cloudflare.com/ajax/libs/xterm/3.14.5/xterm.min.css\">\
    <script src=\"https://cdnjs.cloudflare.com/ajax/libs/xterm/3.14.5/xterm.min.js\"></script>\
</head>\
<body>\
<script type=\"text/javascript\">\
    var ws = new WebSocket(\"ws://192.168.0.103\");\
    ws.onopen = ()=>{\
        terminal.write('terminal is ready...\\n');\
    };\
    ws.onerror = ws.onclose = console.error;\
    ws.onmessage = (m)=>{\
        m.data.startsWith && terminal.write(m.data);\
        m.data.text && m.data.text().then((t)=>{\
                terminal.write(t);\
            }\
        )\
    };\
    var terminal = new Terminal();\
    terminal.open(document.body);\
    var buf = String();\
    terminal.on('data', (key, ev) => {\
    if (key.charCodeAt(0) == 13){\
        terminal.writeln('');\
        ws.send(buf + '\\n');\
        buf = \"\";\
    }else{\
      terminal.write(key);\
      buf += key;\
    }\
    });\
</script>\
</body>\
</html>";
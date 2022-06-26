

namespace Websocket {
	using BIT = BYTE;
	enum Opcode : BYTE {
		Continuation = 0,
		Text = 0x1,
		Binary = 0x2,
		Close = 0x8,
		Ping = 0x9,
		Pong = 0xA
	};
}

template <ULONG N>
consteval ULONG cstrlen(const char(&)[N]) {
	return N - 1;
}
consteval ULONG cstrlen(const char* s) {
	ULONG res = 0;
	for (; *s; s++) {
		res++;
	}
	return res;
}
enum class State : unsigned __int8 {
	AfterRecv, ReadStaticFile, SendPartFile, AfterSendHTML, AfterHandShake, WebSocketConnecting, AfterClose
};
int http_on_header_field(llhttp_t* parser, const char* at, size_t length);
int http_on_header_value(llhttp_t* parser, const char* at, size_t length);
int http_on_url(llhttp_t* parser, const char* at, size_t length);

struct Parse_Data {
	Parse_Data() : headers{}, uri{} {
		llhttp_settings_init(&settings); 
		settings.on_url = http_on_url;
		settings.on_header_field = http_on_header_field;
		settings.on_header_value = http_on_header_value; 
		llhttp_init(&parser, HTTP_REQUEST, &settings);
	};
	llhttp_t parser;
	std::map<std::string, std::string> headers;
	const CHAR* uri;
	ULONG uriLen;
	size_t length;
	const char* at;
	llhttp_settings_t settings;
};

struct Stdio {
	OVERLAPPED ol, sendOL;
	HANDLE pipe;
	CHAR buf[100];
	WSABUF wsaBuf[2];
};

struct IOCP {
	SOCKET client;
	State state;
	OVERLAPPED recvOL, sendOL;
	char buf[4096];
	DWORD dwFlags;
	WSABUF sendBuf[2], recvBuf[1];
	bool Reading6Bytes;
	unsigned __int64 payload_len;
	BYTE header[4];
	struct Stdio sIn, sOut, sErr;
	HANDLE hProcess;
	Websocket::Opcode op;
	char* process_name;
	bool keepalive, firstCon;
};


constexpr const char* not_implemented = \
"HTTP/1.1 501 Not Implemented\r\n"
"Content-Type: text/html\r\n\r\n"
"<!DOCTYPE html><html><head><title>501 Not Implemented</title></head><body><h1 align=\"center\">501 Not Implemented</h1><hr><p style=\"text-align: center\">something wrong?<p/></body></html>";
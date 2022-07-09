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
	Parse_Data() : headers{}, uri{}, at{}, length{}, uriLen{}, rows{ 60 }, cols{ 30 } {
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
	SHORT rows, cols;
};

struct IOCP {
	COORD coord;
	SOCKET client;
	State state;
	OVERLAPPED recvOL, sendOL;
	char buf[4096+64];
	DWORD dwFlags;
	WSABUF sendBuf[2], recvBuf[1], conpty[2];
	bool Reading6Bytes;
	unsigned __int64 payload_len;
	BYTE header[4];
	Websocket::Opcode op;
	bool keepalive, firstCon;
	HANDLE hStdOut, hStdIn;
	HPCON hPC;
	HANDLE hReadThread;
	HANDLE waitHandle;
	HANDLE hProcess;
};

namespace HTTP_ERR_RESPONCE {
	static char sinternal_server_error[] = "HTTP/1.1 500 Internal Server Error\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"\r\n"
		"<!DOCTYPE html><html><head><title>500 Internal Server Error</title></head><body><h1 align=\"center\">500 Internal Server Error</h1><hr><p style=\"text-align: center\">The server has some error...<p/></body></html>",
		snot_found[] =
		"HTTP/1.1 404 Not Found\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"\r\n"
		"<!DOCTYPE html><html><head><title>404 Not Found</title></head><body><h1 align=\"center\">404 Not Found</h1><hr><p style=\"text-align: center\">The Request File is not found in the server<p/></body></html>",

		smethod_not_allowed[] =
		"HTTP/1.1 405 Method Not Allowed\r\n"
		"Connection: close\r\n"
		"Allow: GET, HEAD\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"\r\n"
		"<!DOCTYPE html><html><head><title>405 Method Not Allowed</title></head><body><h1 align=\"center\">405 Method Not Allowed</h1><hr><p style=\"text-align: center\">You can only use GET, HEAD request<p/></body></html>",
		sbad_request[] =
		"HTTP/1.1 400 Bad Request\r\n"
		"Connection: close\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"\r\n"
		"<!DOCTYPE html><html><head><title>400 Bad Request</title></head><body><h1 align=\"center\">400 Bad Request</h1><hr><p style=\"text-align: center\">Thes server can't process the request<br>Maybe you miss some header(s)<p/></body></html>";
	static WSABUF
		internal_server_error = {
			cstrlen(sinternal_server_error), sinternal_server_error
	},
		not_found = {
			cstrlen(snot_found), snot_found
	},
		method_not_allowed = {
		cstrlen(smethod_not_allowed), smethod_not_allowed
	},
		bad_request = {
		cstrlen(sbad_request), sbad_request
	};
}
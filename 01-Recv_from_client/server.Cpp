// compile with `/std:c++latest`
// simple WebSocket recv server

//#include "llhttp.h" // nodejs llhttp libary
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <map>
#include <string>
#include <assert.h>

HANDLE heap;

DWORD WINAPI WorkerThread(LPVOID WorkThreadContext);
#define writeLog(s) writeLog__impl(s L"\n")

void writeLog__impl(const WCHAR *s){
	fputws(s, stdout);
}

#include "types.cpp"
#include "handshake.cpp"
#include "utility.cpp"
#include "frame.cpp"

int http_on_header_field(llhttp_t* parser, const char* at, size_t length){
	Request* req = (Request*)parser;
	req->parse_data.length = length;
	req->parse_data.at = at;
    return 0;
}
int http_on_header_value(llhttp_t* parser, const char* at, size_t length){
	Request* req = (Request*)parser;
	req->headers[std::string(req->parse_data.at, req->parse_data.length)] = std::string(at, length);
    return 0;
}

DWORD WINAPI WorkerThread(LPVOID WorkThreadContext) {
	Request* ctx = (Request*)WorkThreadContext;
	int dwIoSize = recv(ctx->client, ctx->buf, sizeof(ctx->buf), 0);
	if (dwIoSize <= 0)
		return 1;
	llhttp_settings_t settings;
	llhttp_settings_init(&settings);
	llhttp_init(&ctx->parser, HTTP_REQUEST, &settings);
	settings.on_header_field = http_on_header_field;
	settings.on_header_value = http_on_header_value;
	enum llhttp_errno err = llhttp_execute(&ctx->parser, (char*)ctx->buf, dwIoSize);
	auto upgrade = ctx->headers.find("Upgrade");
	auto connection = ctx->headers.find("Connection");
	auto ws_key = ctx->headers.find("Sec-WebSocket-Key");
	if (ws_key == ctx->headers.end() ||
		upgrade == ctx->headers.end() ||
		connection == ctx->headers.end() ||
		upgrade->second != "websocket" ||
		!connection->second.contains("Upgrade"))
	{
		writeLog(L"HTTP Request");
		send(ctx->client, indexHtml, cstrlen(indexHtml), 0);
		CloseClient(ctx);
	}
	else {
		writeLog(L"WebSocket Request");
		ws_key->second += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		char buf[31];
		HashHanshake(ws_key->second.data(), (ULONG)ws_key->second.length(), buf);
		int len;
		len = snprintf((char*)ctx->buf, sizeof(ctx->buf),
			"HTTP/1.1 101 Switching Protocols" CRLF
			"Upgrade: websocket" CRLF
			"Connection: Upgrade" CRLF
			"Sec-Websocket-Accept: %s" CRLF
			"Access-Allow-Origin: *" CRLF CRLF, buf);
		assert(len > 0);
		send(ctx->client, (char*)ctx->buf, len, 0);
		const char* info = mainloop(ctx);
		puts(info);
		CloseClient(ctx);
	}
	return 0;
}

int main()
{
	{
		WSADATA wsaData{};
		WSAStartup(MAKEWORD(2, 2), &wsaData);
	}
	heap = GetProcessHeap();
	assert(initHash());
	assert(initListenSocket());
	writeLog(L"[info] HTTP server running at http://localhost, websocket server listening at ws://localhost");
	for (;;) {
		SOCKET client = WSAAccept(g_sdListen, NULL, NULL, NULL, 0);
		Request* ctx = UpdateCompletionPort(client);
		assert(ctx);
		CloseHandle(CreateThread(NULL, 0, WorkerThread, ctx, 0, 0));
	}
	WSACleanup();
	closeHash();
	writeLog(L"[info] server shutdowned");
}
#include "llhttp.h"
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <wchar.h>
#include <map>
#include <string>

#define assert(x) {if (!(x)){printf("error in %s.%d: %s", __FILE__, __LINE__, #x);}}

HANDLE heap;

#include "types.h"
#include "handshake.cpp"

VOID CloseClient (Request* ctx) {
    if (ctx->client){
        shutdown(ctx->client, SD_BOTH);
        closesocket(ctx->client);
        ctx->client = NULL;
        ctx->~Request(); // free std::map, std::string
        if (!HeapFree(heap,0, ctx)){
            assert(0);
        }
    }
}

Request* initSocket(SOCKET client) {
    void* mem = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Request));
    if (mem == NULL)
    	return NULL;
    Request *ctx = new (mem)Request(); // init std::map, std::string
    if (ctx==NULL)
        return NULL;
   ctx->client = client;
   return ctx;
}

BOOL sendall(SOCKET socket, const void *buffer, int length)
{
    char *ptr = (char*) buffer;
    while (length > 0)
    {
        int i = send(socket, ptr, length, 0);
        if (i < 1) return FALSE;
        ptr += i;
        length -= i;
    }
    return TRUE;
}

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
		assert(sendall(ctx->client, indexHtml, cstrlen(indexHtml)));
		CloseClient(ctx);
	}
	else {
		ws_key->second += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
		char buf[29]; /*base64 encode 20 bytes => 28 bytes output + 1 null character*/
		BOOL ret = HashHanshake(ws_key->second.data(), (ULONG)ws_key->second.length(), buf);
		assert(ret);
		int len;
		len = snprintf((char*)ctx->buf, sizeof(ctx->buf),
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: WebSocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: %s\r\n\r\n", buf);
		assert(len > 0);
		ret = sendall(ctx->client, (void*)ctx->buf, len);
		assert(ret);
		const char* reason = mainloop(ctx);
		printf("WebSocket connection closed because: %s\n", reason);
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
	BOOL ret = initHash();
	assert(ret);
	sockaddr_in ip4 {.sin_family=AF_INET, .sin_port=htons(80)};
	SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server==INVALID_SOCKET)
		return 1;
	if ( bind(server, (struct sockaddr*)&ip4, sizeof(ip4)) == SOCKET_ERROR)
		return 1;
	if (listen(server, SOMAXCONN) == SOCKET_ERROR)
		return 1;
	puts("recv Server at http://localhost, ws://localhost");
	for (;;) {
		SOCKET client = accept(server, NULL, NULL);
		Request* ctx = initSocket(client);
		assert(ctx);
		CreateThread(NULL, 0, WorkerThread, ctx, 0, 0);
	}
	WSACleanup();
	closeHash();
}
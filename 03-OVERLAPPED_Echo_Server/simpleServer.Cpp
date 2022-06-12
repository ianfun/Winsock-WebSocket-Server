#include "llhttp.h"
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <wchar.h>
#include <map>
#include <string>

#define assert(x) {if (!(x)){printf("error in %s.%d: %s, err=%d\n", __FILE__, __LINE__, #x, WSAGetLastError());}}

HANDLE heap, iocp;

#include "types.h"
#include "handshake.cpp"

VOID CloseClient (Request* ctx) {
    if (ctx->state != Request::AfterClose){
		ctx->state = Request::AfterClose;
        shutdown(ctx->client, SD_BOTH);
        closesocket(ctx->client);
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
   HANDLE ret = CreateIoCompletionPort((HANDLE)client, iocp, (ULONG_PTR)ctx, 0);
   assert(ret);
   return ctx;
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
	DWORD dwbytes=0;
	Request* ctx=NULL;
	OVERLAPPED* ol=NULL;
	for (;;) {
		BOOL ret = GetQueuedCompletionStatus((HANDLE)WorkThreadContext, &dwbytes, (PULONG_PTR)&ctx, &ol, INFINITE);
		if (ret == FALSE) {
			printf("GetQueuedCompletionStatus failed, WSAGetLastError()=%d\n", WSAGetLastError());
			continue;
		}
		if (dwbytes == 0) {
			puts("dwbytes==0, close client");
			CloseClient(ctx);
			continue;
		}
		if (ctx == NULL || ol == NULL) {
			puts("ctx == NULL || ol == NULL, do nothing");
			continue;
		}
		switch (ctx->state) {
		case Request::AfterRecv:
		{
			llhttp_settings_t settings;
			llhttp_settings_init(&settings);
			llhttp_init(&ctx->parser, HTTP_REQUEST, &settings);
			settings.on_header_field = http_on_header_field;
			settings.on_header_value = http_on_header_value;
			enum llhttp_errno err = llhttp_execute(&ctx->parser, (char*)ctx->buf, dwbytes);
			auto upgrade = ctx->headers.find("Upgrade");
			auto connection = ctx->headers.find("Connection");
			auto ws_key = ctx->headers.find("Sec-WebSocket-Key");
			if (ws_key == ctx->headers.end() ||
				upgrade == ctx->headers.end() ||
				connection == ctx->headers.end() ||
				upgrade->second != "websocket" ||
				!connection->second.contains("Upgrade"))
			{
				ctx->state = Request::AfterSendHTML;
				ctx->sendBuf[0].buf = (char*)indexHtml;
				ctx->sendBuf[0].len = cstrlen(indexHtml);
				WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
			}
			else {
				ctx->state = Request::AfterHandShake;
				ws_key->second += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
				char buf[29];
				BOOL ret = HashHanshake(ws_key->second.data(), (ULONG)ws_key->second.length(), buf);
				assert(ret);
				int len;
				len = snprintf((char*)ctx->buf, sizeof(ctx->buf),
					"HTTP/1.1 101 Switching Protocols\r\n"
					"Upgrade: WebSocket\r\n"
					"Connection: Upgrade\r\n"
					"Sec-WebSocket-Accept: %s\r\n\r\n", buf);
				assert(len > 0);
				ctx->sendBuf[0].buf = ctx->buf;
				ctx->sendBuf[0].len = (ULONG)len;
				WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
				assert(ret);
			}
		}break;
		case Request::AfterHandShake:
		{
			ctx->state = Request::WebSocketConnecting;
			ctx->recvBuf[0].len = 6;
			ctx->dwFlags = MSG_WAITALL;
			ctx->Reading6Bytes = true;
			WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		}break;
		case Request::WebSocketConnecting: {
			if (ol == &ctx->recvOL) {
				/*A recv operation finished*/
				if (ctx->Reading6Bytes) {
					onRead6Complete(ctx);
				}
				else {
					onRecvData(ctx);
				}
			}
			else {
				/*A send operation finished*/
				assert(ol == &ctx->sendOL);
				/*ignore it*/
			}
		}break;
		case Request::AfterSendHTML: {
			CloseClient(ctx);
		}break;
		case Request::AfterClose:
		{
			assert(0);
		}break;
		default:
		{
			assert(0);
		}

		}
	}
}

int main()
{
	{
		WSADATA wsaData{};
		int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
		assert(ret==0);
	}
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	assert(iocp);
	heap = GetProcessHeap();
	BOOL ret = initHash();
	assert(ret);
	sockaddr_in ip4{ .sin_family = AF_INET, .sin_port = htons(80)};
	ip4.sin_addr.s_addr=INADDR_ANY;
	SOCKET server = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (server==INVALID_SOCKET)
		return 1;
	if ( bind(server, (struct sockaddr*)&ip4, sizeof(ip4)) == SOCKET_ERROR)
		return 1;
	if (listen(server, SOMAXCONN) == SOCKET_ERROR)
		return 1;
	CreateThread(NULL, 0, WorkerThread, iocp, 0, 0);
	puts("server listening at http://localhost/ and ws://localhost/");
	for (;;) {
		SOCKET client = WSAAccept(server, NULL, 0, NULL, NULL);
		Request* ctx = initSocket(client);
		assert(ctx);
		ctx->recvBuf[0].buf = ctx->buf;
		ctx->recvBuf[0].len = sizeof(ctx->buf);
		WSARecv(client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
	}
	WSACleanup();
	closeHash();
}
#include "llhttp.h"

#include <winsock2.h>
#include <Ws2tcpip.h>
#include <mstcpip.h>
#include <wchar.h>
#include "Mine.h"
#include <crtdbg.h>
#pragma comment(lib, "WS2_32")
#pragma comment(lib, "Mswsock") 

HANDLE heap, iocp;

#define assert(x) {if (!(x)){printf("error in %s.%d: %s, err=%d\n", __FILE__, __LINE__, #x, WSAGetLastError());}}

#include "types.h"
#include "pipe.cpp"
#include "handshake.cpp"

VOID CloseClient(IOCP* ctx) {
	_ASSERT(_CrtCheckMemory());
	if (ctx->client) {
		shutdown(ctx->client, SD_BOTH);
		closesocket(ctx->client);
		ctx->client = NULL;
#define USEMALLOC 1
#ifdef USEMALLOC
		free(ctx);
#else
		HeapFree(heap, 0, ctx);
#endif
		ctx->state = State::AfterClose;
	}
}
IOCP* initSocket(SOCKET client) {
#ifdef USEMALLOC
	IOCP* ctx = (IOCP*)malloc(sizeof(IOCP));
	if (ctx == NULL)
		return NULL;
	ZeroMemory(ctx, sizeof(*ctx));
#else
	IOCP* ctx = (IOCP*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(IOCP));
	if (ctx == NULL)
		return NULL;
#endif
	ctx->client = client;
	HANDLE ret = CreateIoCompletionPort((HANDLE)client, iocp, (ULONG_PTR)ctx, 0);
	assert(ret);
	return ctx;
}

#include "frame.cpp"

int http_on_header_field(llhttp_t* parser, const char* at, size_t length) {
	Parse_Data* p = (Parse_Data*)parser;
	p->length = length;
	p->at = at;
	return 0;
}
int http_on_header_value(llhttp_t* parser, const char* at, size_t length) {
	Parse_Data* p = (Parse_Data*)parser;
	p->headers[std::string(p->at, p->length)] = std::string(at, length);
	return 0;
}
int http_on_url(llhttp_t* parser, const char* at, size_t length) {
	Parse_Data* p = (Parse_Data*)parser;
	std::string tmp{ at, length };
	DWORD escaped = 1;
	char dummy;
	HRESULT res = UrlUnescapeA((PSTR)tmp.data(), &dummy, &escaped, 0);
	p->uri = (CHAR*)HeapAlloc(heap, 0, escaped + 1);
	assert(p->uri);
	*(CHAR*)&p->uri[escaped] = '\0';
	p->uriLen = escaped;
	res = UrlUnescapeA(tmp.data(), (PSTR)p->uri, &escaped, 0);
	assert(res == S_OK);
	return 0;
}
DWORD WINAPI WorkerThread(LPVOID WorkThreadContext) {
	DWORD dwbytes = 0;
	IOCP* ctx = NULL;
	OVERLAPPED* ol = NULL;
	for (;;) {
		BOOL ret = GetQueuedCompletionStatus((HANDLE)WorkThreadContext, &dwbytes, (PULONG_PTR)&ctx, &ol, INFINITE);
		if (ret == FALSE) {
			/*
			* GetLastError() retrun thread local error
			*/
			if (ctx) {
				int err;
				switch ((err=GetLastError())) {
				case ERROR_BROKEN_PIPE:
				{
					if (ctx->hProcess != NULL) {
						*(PWORD)ctx->buf = htons(1000);
						DWORD dwExitCode = 0;
						const char* msg;
						if (GetExitCodeProcess(ctx->hProcess, &dwExitCode)) {
							msg = "process exit with code %u";
						}
						else {
							msg = "process was exited(failed to get exit code)";
							// ERROR_INVALID_HANDLE on GetExitCodeProcess
						}
						int n = snprintf(ctx->buf + 2, sizeof(ctx->buf) - 2, msg, dwExitCode);
						assert(n > 0);
						websocketWrite(ctx, ctx->buf, n + 2, &ctx->sendOL, ctx->sendBuf, Websocket::Opcode::Close);
						CloseHandle(ctx->hProcess);
						ctx->hProcess = NULL;
					}
				}break;
				case ERROR_HANDLE_EOF:
				{
					BOOL res = CloseHandle(ctx->hProcess);
					assert(res);
					if (ctx->keepalive)
					{
					
						ctx->state = State::AfterRecv;
						puts("KeepAlive connection: continue");
						ctx->recvBuf[0].buf = ctx->buf;
						ctx->recvBuf[0].len = sizeof(ctx->buf);
						ctx->recvOL.Offset = ctx->recvOL.OffsetHigh = 0; // reset reading position
						WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
						continue;
					}
					else {
						CloseClient(ctx);
					}
				}break;
				default:
					printf("undefined error(GetLastError=%d): I'm going to ignore it\n", err);
				}
				
			}
			continue;
		}
		if (ctx == NULL || ol==NULL) {
			printf("%d\n", GetLastError());
			continue;
		}
		if (dwbytes == 0) {
			printf("Close connection, err=%d\n", GetLastError());
			CloseClient(ctx);
			continue;
		}
		switch (ctx->state) {
			// Accept-Ranges: bytes
		case State::AfterRecv:
		{
			ctx->buf[dwbytes] = '\0';
			Parse_Data parse_data{};
			enum llhttp_errno err = llhttp_execute(&parse_data.parser, ctx->buf, dwbytes);
			if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
				printf("llhttp_execute: %s\n", llhttp_errno_name(err));
				CloseClient(ctx);
				continue;
			}
			if (parse_data.parser.http_major != 1 || parse_data.parser.http_minor != 1) {
				puts("expect HTTP/1.1");
				CloseClient(ctx);
				continue;
			}
			printf("%s [%s]\n", llhttp_method_name((llhttp_method)parse_data.parser.method), parse_data.uri);
			switch (err) {
			case HPE_OK:
			{
				if (ctx->firstCon) {
					ctx->firstCon = false;
					auto connection = parse_data.headers.find("Connection");
					if (connection != parse_data.headers.end()) {
						if (connection->second == "keep-alive" || connection->second == "Keep-Alive") {
							ctx->keepalive = true;
							DWORD yes = TRUE;
							int success = setsockopt(ctx->client, SOL_SOCKET, SO_KEEPALIVE, (char*)&yes, sizeof(yes));
							assert(success == 0);
							puts("set tcp keep alive(SO_KEEPALIVE)");
							auto keepalive = parse_data.headers.find("Keep-Alive");
							if (keepalive == parse_data.headers.end())
								keepalive = parse_data.headers.find("keep-alive");
							if (keepalive != parse_data.headers.end()) {
								auto s = keepalive->second.data();
								auto timeouts = StrStrA(s, "timeout");
								if (timeouts) {
									int timeout;
									int res = sscanf_s(timeouts + 7, "=%d", &timeout);
									if (res > 0) {
										printf("set TCP keep alive=%d\n", timeout);
										int yes = TRUE;
										res = setsockopt(ctx->client, SOL_SOCKET, TCP_KEEPIDLE, (char*)&yes, sizeof yes);
										assert(res == 0);
									}
									else {
										puts("Error: failed to parse keepalive seconds...");
									}
								}
							}
						}
						else {
							printf("I got Connection: %s\n", connection->second.data());
						}
					}
				}
				int res = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, parse_data.uri, (int)parse_data.uriLen, (LPWSTR)ctx->buf, sizeof(ctx->buf) / 2);
				ctx->state = State::ReadStaticFile;
				LPCWSTR file = (LPWSTR)ctx->buf;
				if (parse_data.uriLen == 2 && *parse_data.uri == '/') {
					file = L"index.html";
				}
				else {
					file += 1;
				}
				printf("responce file %ws\n", file);
				HANDLE hFile = CreateFileW(file,
					GENERIC_READ,
					FILE_SHARE_READ,
					NULL,
					OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
					NULL);
				if (hFile == INVALID_HANDLE_VALUE) {
					printf("File not found %ws\n", file);
					goto BAD_REQUEST_AND_RELEASE;
				}
				HANDLE r = CreateIoCompletionPort(hFile, iocp, (ULONG_PTR)ctx, 0);
				assert(r);
				const char* mine = getType(file);
				ctx->hProcess = hFile;
				LARGE_INTEGER fsize{};
				BOOL bSuccess = GetFileSizeEx(hFile, &fsize);
				assert(bSuccess);
				res = snprintf(ctx->buf, sizeof(ctx->buf), 
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: %s\r\n"
					"Content-Length: %lld\r\n"
					"Connection: %s\r\n\r\n", mine, fsize.QuadPart, ctx->keepalive ? "keep-alive":"close");
				assert(res > 0);
				ctx->sendBuf->buf = ctx->buf;
				ctx->sendBuf->len = (ULONG)res;
				WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
			}break;
			case HPE_PAUSED_UPGRADE:
			{
				auto upgrade = parse_data.headers.find("Upgrade");
				auto pro = parse_data.headers.find("Sec-WebSocket-Protocol");
				if (upgrade != parse_data.headers.end() && pro != parse_data.headers.end()) {
					if (upgrade->second == "websocket") {
						auto ws_key = parse_data.headers.find("Sec-WebSocket-Key");
						if (ws_key != parse_data.headers.end()) {
							ctx->state = State::AfterHandShake;
							ws_key->second += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
							char buf[29];
							BOOL ret = HashHanshake(ws_key->second.data(), (ULONG)ws_key->second.length(), buf);
							assert(ret);
							int len;
							len = snprintf(ctx->buf, sizeof(ctx->buf),
								"HTTP/1.1 101 Switching Protocols\r\n"
								"Upgrade: WebSocket\r\n"
								"Connection: Upgrade\r\n"
								"Sec-WebSocket-Protocol: %s\r\n"
								"Sec-WebSocket-Accept: %s\r\n\r\n", pro->second.data(), buf);
							assert(len > 0);

							ctx->process_name = StrDupA(pro->second.data());
							ctx->sendBuf[0].buf = ctx->buf;
							ctx->sendBuf[0].len = (ULONG)len;
							WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
						}
						else {
							goto BAD_REQUEST_AND_RELEASE;
						}
					}
					else {
						goto BAD_REQUEST_AND_RELEASE;
					}
				}
				else {
					goto BAD_REQUEST_AND_RELEASE;
				}
			}break;
			default:
			BAD_REQUEST_AND_RELEASE:
			{
				ctx->state = State::AfterSendHTML;
				ctx->sendBuf[0].buf = (char*)not_implemented;
				ctx->sendBuf[0].len = cstrlen(not_implemented);
				WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
			}
			} // end switch
		}break; // end case
		case State::AfterHandShake:
		{
			ctx->state = State::WebSocketConnecting;
			if (ctx->process_name == NULL) {
				CloseClient(ctx);
				continue;
			}
			int len = (int)lstrlenA(ctx->process_name);
			int size = MultiByteToWideChar(CP_UTF8, 0, ctx->process_name, len, NULL, 0) * 2;
			if (size <= 0) {
				assert(0);
				CloseClient(ctx);
				continue;
			}
			WCHAR* name = (WCHAR*)HeapAlloc(heap, 0, size);
			size = MultiByteToWideChar(CP_UTF8, 0, ctx->process_name, len, name, size / 2);
			LocalFree(ctx->process_name);
			if (size <= 0) {
				assert(0);
				CloseClient(ctx);
				continue;
			}
			printf("create process: %ws\n", name);
			BOOL res = CreateCGI(name, ctx);
			HeapFree(heap, 0, name);
			if (res == FALSE) {
				int n = snprintf(ctx->buf+2, sizeof(ctx->buf)-2, "Error: create process failed, GetLastError()=%d", GetLastError());
				*(PWORD)ctx->buf = htons(1000);
				assert(n > 0);
				websocketWrite(ctx, ctx->buf, n + 2, &ctx->sendOL, ctx->sendBuf, Websocket::Opcode::Close);
				continue;
			}
			ctx->recvBuf[0].buf = ctx->buf;
			ctx->recvBuf[0].len = 6;
			ctx->dwFlags = MSG_WAITALL;
			ctx->Reading6Bytes = true;
			WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		}break;
		case State::ReadStaticFile:
		{
			(void)ReadFile(ctx->hProcess, ctx->buf, sizeof(ctx->buf), NULL, &ctx->recvOL);
			ctx->state = State::SendPartFile;
		}break;
		case State::SendPartFile:
		{
			ctx->recvOL.Offset += dwbytes;
			ctx->sendBuf->len = dwbytes;
			WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
			ctx->state = State::ReadStaticFile;
		}break;
		case State::WebSocketConnecting: {
			if (ol == &ctx->recvOL) {
				if (ctx->Reading6Bytes) {
					onRead6Complete(ctx);
				}
				else {
					onRecvData(ctx);
				}
			}
			else if (ol == &ctx->sOut.ol) {
				websocketWrite(ctx, (CHAR*)ctx->sOut.buf, dwbytes, &ctx->sOut.sendOL, ctx->sOut.wsaBuf);
			}
			else if (ol == &ctx->sErr.ol) {
				websocketWrite(ctx, (CHAR*)ctx->sErr.buf, dwbytes, &ctx->sErr.sendOL, ctx->sErr.wsaBuf);
			}
			else if (ol == &ctx->sOut.sendOL) {
				(void)ReadFile(ctx->sOut.pipe, ctx->sOut.buf, sizeof(ctx->sOut.buf), NULL, &ctx->sOut.ol);
			}
			else if (ol == &ctx->sErr.sendOL) {
				(void)ReadFile(ctx->sErr.pipe, ctx->sErr.buf, sizeof(ctx->sErr.buf), NULL, &ctx->sErr.ol);
			}
			else if (ol == &ctx->sendOL) {
				CloseClient(ctx);
			}
		}break;
		case State::AfterSendHTML: {
			CloseClient(ctx);
		}break;
		case State::AfterClose:
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
	system("chcp 65001");
	{
		WSADATA wsaData{};
		int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
		assert(ret == 0);
	}
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	assert(iocp);
	heap = GetProcessHeap();
	BOOL ret = initHash();
	assert(ret);
	sockaddr_in ip4{ .sin_family = AF_INET, .sin_port = htons(80) };
	ip4.sin_addr.s_addr = INADDR_ANY;
	SOCKET server = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (server == INVALID_SOCKET)
		return 1;
	if (bind(server, (struct sockaddr*)&ip4, sizeof(ip4)) == SOCKET_ERROR)
		return 1;
	if (listen(server, SOMAXCONN) == SOCKET_ERROR)
		return 1;
	CreateThread(NULL, 0, WorkerThread, iocp, 0, 0);
	puts("server listening at http://localhost/ and ws://localhost/");
	for (;;) {
		sockaddr_in sIn{};
		int sInLen = sizeof(sIn);
		SOCKET client = WSAAccept(server, (sockaddr*)&sIn, &sInLen, NULL, NULL);
		IOCP* ctx = initSocket(client);
		assert(ctx);
		ctx->recvBuf[0].buf = ctx->buf;
		ctx->recvBuf[0].len = sizeof(ctx->buf);
		ctx->firstCon = true;
		printf("=== new socket accepted: %lld ===\n", client);
		WSARecv(client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
	}
	WSACleanup();
	closeHash();
}


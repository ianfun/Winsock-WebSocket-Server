#include <winsock2.h>
#include <Ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#include <crtdbg.h>
#include <wchar.h>
#include <WinInet.h>
#include "Mine.h"
#include "llhttp.h"

#pragma comment(lib, "WS2_32")
#pragma comment(lib, "Mswsock") 
#pragma comment(lib, "Wininet")
#pragma comment(lib, "IPhlpAPI.lib")

static LPFN_ACCEPTEX pAcceptEx;
static HANDLE heap, iocp;
static OVERLAPPED dumyOL = { 0 };

__declspec(noreturn) void fatal(const char* msg) {
	printf("[fatal error] %s\nExit...\n", msg);
	ExitProcess(1);
}

#ifdef _DEBUG
#define assert(x) {if (!(x)){printf("[error] %s.%d: %s, err=%d\n", __FILE__, __LINE__, #x, WSAGetLastError());}}
#else
#define assert(x) (void)(x)
#endif
#include "types.h"
VOID CloseClient(IOCP* ctx);
#include "handshake.cpp"
#include "frame.cpp"
#include "pipe.cpp"
static struct {
	HANDLE iocp;
	SOCKET server;
	OVERLAPPED ol;
	IOCP* currentCtx;
} acceptIOCP{};
void processRequest(IOCP* ctx, DWORD dwbytes);
void processIOCP(IOCP* ctx, OVERLAPPED* ol, DWORD dwbytes);
VOID CloseClient(IOCP* ctx) {
	if (ctx->state != State::AfterClose) {
		ctx->state = State::AfterClose;
		(void)shutdown(ctx->client, SD_BOTH);
		if (closesocket(ctx->client) != 0) {
			assert(0);
		}
		if (HeapFree(heap, 0, ctx) == FALSE) {
			assert(0);
		}
	}
}
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
	const char* q = strchr(at, '?');
	if (q) {
		q++;
		int n = sscanf_s(q, "rows=%hu&cols=%hu", &p->rows, &p->cols);
		if (n == 2) {
			// good
		}
		else {
			p->rows = 70;
			p->cols = 130;
		}
	}
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
LPCWSTR encodePath(IOCP* ctx, Parse_Data& parse_data) {
	int res = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, parse_data.uri, (int)parse_data.uriLen, (LPWSTR)ctx->buf, sizeof(ctx->buf) / 2);
	ctx->state = State::ReadStaticFile;
	LPCWSTR file = (LPWSTR)ctx->buf;
	if (parse_data.uriLen == 2 && *parse_data.uri == '/') {
		file = L"index.html";
	}
	else {
		file += 1;
	}
	return file;
}
void processIOCP(IOCP* ctx, OVERLAPPED* ol, DWORD dwbytes) {
	switch (ctx->state) {
	case State::AfterRecv:
	{
		processRequest(ctx, dwbytes);
	}break;
	case State::AfterHandShake:
	{
		ctx->state = State::WebSocketConnecting;
		WCHAR name[] = L"cmd";
		BOOL res = spawn(name, ctx);
		if (res == FALSE) {
			int n = snprintf(ctx->buf + 2, sizeof(ctx->buf) - 2, "Server Error: create process failed, GetLastError()=%d", GetLastError());
			*(PWORD)ctx->buf = htons(1000);
			assert(n > 0);
			websocketWrite(ctx, ctx->buf, n + 2, &ctx->sendOL, ctx->sendBuf, Websocket::Opcode::Close);
			return;
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
		else if (ol == &ctx->sendOL) {
			CloseClient(ctx);
		}
		else if (ol == &dumyOL) {
			/*discard*/
		}
		else {
			assert(0);
		}
	}break;
	case State::AfterSendHTML: {
		CloseClient(ctx);
	}break;
	case State::AfterClose:
	default:
	{
		assert(0);
	}
	}
}
void processRequest(IOCP* ctx, DWORD dwbytes) {
	ctx->buf[dwbytes] = '\0';
	Parse_Data parse_data{};
	enum llhttp_errno err = llhttp_execute(&parse_data.parser, ctx->buf, dwbytes);
	if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
		printf("llhttp_execute error: %s\n", llhttp_errno_name(err));
		CloseClient(ctx);
		return;
	}
	if (parse_data.parser.http_major != 1 || (parse_data.parser.http_minor != 1 && parse_data.parser.http_minor != 0)) {
		printf("version not accepable HTTP/%d.%d\n", (int)parse_data.parser.http_major, (int)parse_data.parser.http_minor);
		CloseClient(ctx);
		return;
	}
	printf("[%s] %s\n", llhttp_method_name((llhttp_method)parse_data.parser.method), parse_data.uri);
	WSABUF* errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
	switch (parse_data.parser.method) {
	case llhttp_method::HTTP_GET: {
		switch (err) {
		case HPE_OK:
		{
			if (ctx->firstCon) {
				ctx->firstCon = false;
				int iResult = 0;
				iResult = setsockopt(ctx->client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
					(char*)&acceptIOCP.server, sizeof(acceptIOCP.server));

				auto connection = parse_data.headers.find("Connection");
				if (connection != parse_data.headers.end()) {
					if (connection->second == "keep-alive" || connection->second == "Keep-Alive") {
						ctx->keepalive = true;
						DWORD yes = TRUE;
						int success = setsockopt(ctx->client, SOL_SOCKET, SO_KEEPALIVE, (char*)&yes, sizeof(yes));
						assert(success == 0);
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
									printf("set TCP keep alive timeout=%d\n", timeout);
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
				}
			}
			auto file = encodePath(ctx, parse_data);
			HANDLE hFile = CreateFileW(file,
				GENERIC_READ,
				FILE_SHARE_READ,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
				NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				errBuf = &HTTP_ERR_RESPONCE::not_found;
				goto BAD_REQUEST_AND_RELEASE;
			}
			HANDLE r = CreateIoCompletionPort(hFile, iocp, (ULONG_PTR)ctx, 0);
			assert(r);
			const char* mine = getType(file);
			ctx->hProcess = hFile;
			LARGE_INTEGER fsize{};
			BOOL bSuccess = GetFileSizeEx(hFile, &fsize);
			assert(bSuccess);
			int res = snprintf(ctx->buf, sizeof(ctx->buf),
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: %s\r\n"
				"Content-Length: %lld\r\n"
				"Connection: %s\r\n\r\n", mine, fsize.QuadPart, ctx->keepalive ? "keep-alive" : "close");
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
						ctx->sendBuf[0].buf = ctx->buf;
						ctx->sendBuf[0].len = (ULONG)len;
						WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
						ctx->coord.X = parse_data.cols;
						ctx->coord.Y = parse_data.cols;
					}
					else {
						errBuf = &HTTP_ERR_RESPONCE::bad_request;
						goto BAD_REQUEST_AND_RELEASE;
					}
				}
				else {
					errBuf = &HTTP_ERR_RESPONCE::bad_request;
					goto BAD_REQUEST_AND_RELEASE;
				}
			}
			else {
				errBuf = &HTTP_ERR_RESPONCE::bad_request;
				goto BAD_REQUEST_AND_RELEASE;
			}
		}break;
		DEFAULT_UNREACHABLE;
		}
	}break;
	case llhttp_method::HTTP_HEAD:
	{
		if (err == llhttp_errno::HPE_OK) {
			auto file = encodePath(ctx, parse_data);
			HANDLE hFile = CreateFileW(file,
				GENERIC_READ,
				FILE_SHARE_READ,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				NULL);
			if (hFile == INVALID_HANDLE_VALUE)
			{
				constexpr const char* msg = "HTTP/1.1 404 Not Found\r\n\r\n";
				ctx->sendBuf[0].buf = (CHAR*)msg;
				ctx->sendBuf[0].len = cstrlen(msg);
				ctx->state = State::AfterSendHTML;
				WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
				return;
			}
			FILETIME ftWrite;
			SYSTEMTIME stUTC;
			LARGE_INTEGER fsize;
			if (GetFileTime(hFile, NULL, NULL, &ftWrite)) {
				if (FileTimeToSystemTime(&ftWrite, &stUTC)) {
					char timebuf[30];
					if (InternetTimeFromSystemTimeA(&stUTC, INTERNET_RFC1123_FORMAT, timebuf, sizeof(timebuf))) {
						if (GetFileSizeEx(hFile, &fsize)) {
							int len = snprintf(ctx->buf, sizeof(ctx->buf),
								"HTTP/1.1 200 OK\r\n" // no "Accept-Ranges: bytes\r\n" now
								"Last-Modified: %s\r\n"
								"Conetnt-Length: %lld\r\n"
								"\r\n", timebuf, fsize.QuadPart);
							ctx->sendBuf[0].buf = ctx->buf;
							ctx->sendBuf[0].len = len;
							ctx->state = State::AfterSendHTML;
							WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
							return;
						}
					}
				}
			}
			errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
			goto BAD_REQUEST_AND_RELEASE;
		}
		else {
			goto BAD_REQUEST_AND_RELEASE;
		}
	}break;
	default:
	{
		errBuf = &HTTP_ERR_RESPONCE::method_not_allowed;
	BAD_REQUEST_AND_RELEASE:
		ctx->state = State::AfterSendHTML;
		WSASend(ctx->client, errBuf, 1, NULL, 0, &ctx->sendOL, NULL);
	}
	}
}
DWORD WINAPI WorkerThread(LPVOID WorkThreadContext) {
	DWORD dwbytes = 0;
	IOCP* ctx = NULL;
	OVERLAPPED* ol = NULL;
	for (;;) {
		BOOL ret = GetQueuedCompletionStatus((HANDLE)WorkThreadContext, &dwbytes, (PULONG_PTR)&ctx, &ol, INFINITE);
		if (ret == FALSE) {
			if (ctx) {
				int err = GetLastError();
				switch (err) {
				case ERROR_HANDLE_EOF:
				{
					BOOL res = CloseHandle(ctx->hProcess);
					assert(res);
					if (ctx->keepalive)
					{
						ctx->state = State::AfterRecv;
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
				case ERROR_CONNECTION_ABORTED:
				{
					CloseClient(ctx);
				}break;
				default:
					assert(0);
				}

			}
			continue;
		}
		if (ctx == NULL || ol == NULL) {
			if (ctx == (IOCP*)1) {
				puts("[info] Exit WorkerThread");
				return 0;
			}
			assert(0);
			continue;
		}
		if (dwbytes == 0) {
			CloseClient(ctx);
			continue;
		}
		processIOCP(ctx, ol, dwbytes);
	}
}
void accept_next() {
	while (1) {
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		BOOL bSuccess = SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
		assert(bSuccess);
		IOCP* ctx = (IOCP*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(IOCP));
		if (ctx == NULL) {
			assert(0);
			ExitThread(1);
		}
		HANDLE bRet = CreateIoCompletionPort((HANDLE)client, iocp, (ULONG_PTR)ctx, 0);
		assert(bRet);
		ctx->client = client;
		ctx->recvBuf->buf = ctx->buf;
		ctx->recvBuf->len = sizeof(ctx->buf);
		ctx->state = State::AfterRecv;
		ctx->firstCon = true;
		acceptIOCP.currentCtx = ctx;
		DWORD dwbytes;
		if (pAcceptEx(acceptIOCP.server, client, ctx->buf,
			sizeof(ctx->buf) - 64,
			32, 32,
			&dwbytes, &ctx->recvOL)) {
			processIOCP(ctx, &ctx->recvOL, dwbytes);
		}
		else {
			if (WSAGetLastError() == ERROR_IO_PENDING) {
				break;
			}
			else {
				assert(0);
				ExitThread(1);
			}
		}
	}
}
DWORD WINAPI acceptThread(LPVOID) {
	ULONG_PTR key; LPOVERLAPPED dummy2;
	DWORD dwbytes;
	for (;;)
	{
		BOOL bSuccess = GetQueuedCompletionStatus(acceptIOCP.iocp, &dwbytes, &key, &dummy2, INFINITE);
		if (bSuccess && dwbytes > 0) {
			processRequest(acceptIOCP.currentCtx, dwbytes);
			accept_next();
		}
		else {
			if (key) {
				puts("[info] exit acceptThread");
				return 0;
			}
			assert(0);
			accept_next();
		}
	}
}
BOOL WINAPI ConsoleHandler(DWORD event)
{
	switch (event) {
	case CTRL_C_EVENT:
		PostQueuedCompletionStatus(iocp, 0, 1, 0);
		PostQueuedCompletionStatus(acceptIOCP.iocp, 0, 1, 0);
		return TRUE;
	}
	return FALSE;
}
int wmain(void)
{
	puts("initializing...");
	{
		WSADATA wsaData{};
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			fatal("WSAStartup");
		}
	}
	
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	acceptIOCP.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	heap = GetProcessHeap();
	if (heap == NULL) {
		fatal("GetProcessHeap");
	}
	if (iocp == NULL || acceptIOCP.iocp == NULL) {
		fatal("CreateIoCompletionPort");
	}
	if (!initHash()) {
		fatal("initHash");
	}
	sockaddr_in ip4{ .sin_family = AF_INET};

	{
		PIP_ADAPTER_INFO pAdapter = (IP_ADAPTER_INFO*)HeapAlloc(heap, 0, sizeof(IP_ADAPTER_INFO));
		ULONG ulOutBufLen = sizeof(IP_ADAPTER_INFO);
		if (pAdapter == NULL) {
			fatal("HeapAlloc");
		}
		if (GetAdaptersInfo(pAdapter, &ulOutBufLen) == ERROR_BUFFER_OVERFLOW) {
			pAdapter = (IP_ADAPTER_INFO*)HeapReAlloc(heap, 0, pAdapter, ulOutBufLen);
			if (pAdapter == NULL) {
				fatal("HeapReAlloc");
				return 1;
			}
		}
		if (GetAdaptersInfo(pAdapter, &ulOutBufLen) == NO_ERROR) {
			PIP_ADAPTER_INFO head = pAdapter;
			DWORD i = 0;
			puts("select your ip address");
			while (pAdapter) {
				printf("[%d] %s\n", i, pAdapter->IpAddressList.IpAddress.String);
				pAdapter = pAdapter->Next;
				i++;
			}
			DWORD num;
			puts("enter number:");
			while (scanf_s("%u", &num) != 1 || num > i) { puts("invalid number, try again"); }
			while (num != 0) {
				num--;
				head = head->Next;
			}
			printf("selected address: %s\n", head->IpAddressList.IpAddress.String);
			if (inet_pton(AF_INET, head->IpAddressList.IpAddress.String, (SOCKADDR*)&ip4) != 1) {
				fatal("inet_pton");
			}
			USHORT port;
			puts("enter port");
			while (scanf_s("%hu", &port) != 1) { puts("invalid port, try again"); }
			printf("select port: %hu\n", port);
			ip4.sin_port = htons(port);
			ip4.sin_family = AF_INET;
		}
		else {
			fatal("GetAdaptersInfo");
		}
		HeapFree(heap, 0, pAdapter);
	}

	acceptIOCP.server = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (acceptIOCP.server == INVALID_SOCKET) {
		fatal("WSASocketW");
	}
	if (CreateIoCompletionPort((HANDLE)acceptIOCP.server, acceptIOCP.iocp, 0, 0) == NULL) {
		fatal("CreateIoCompletionPort");
	}
	if (bind(acceptIOCP.server, (struct sockaddr*)&ip4, sizeof(ip4)) == SOCKET_ERROR) {
		fatal("bind");
	}
	if (listen(acceptIOCP.server, SOMAXCONN) == SOCKET_ERROR) {
		fatal("listen");
	}
	{
		DWORD dwBytes = 0;
		GUID GuidAcceptEx = WSAID_ACCEPTEX;
		if (WSAIoctl(acceptIOCP.server, SIO_GET_EXTENSION_FUNCTION_POINTER,
			&GuidAcceptEx, sizeof(GuidAcceptEx),
			&pAcceptEx, sizeof(pAcceptEx),
			&dwBytes, NULL, NULL) == SOCKET_ERROR) {
			return 1;
		}
	}
	puts("[info] server start running");
	CreateThread(NULL, 0, acceptThread, NULL, 0, NULL);
	accept_next();
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	(void)WorkerThread(iocp);

	shutdown(acceptIOCP.server, SD_BOTH);
	closesocket(acceptIOCP.server);
	WSACleanup();

	closeHash();
	_ASSERT(_CrtCheckMemory());
	CloseHandle(iocp);
	CloseHandle(acceptIOCP.iocp);
	CloseHandle(heap);
	puts("[info] Exit Process");
}

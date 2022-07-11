#include <winsock2.h>
#include <Ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
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
struct IOCP;
void accept_next();
void CloseClient(IOCP* ctx);

#ifdef _DEBUG
#define log_puts(x) puts(x)
#define log_fmt printf
#define assert(x) {if (!(x)){printf("[error] %s.%d: %s, err=%d\n", __FILE__, __LINE__, #x, WSAGetLastError());}}
#else
#define assert(x) (void)(x)
#define log_puts(x) 
#define log_fmt(x, ...)
#endif // _DEBUG

#include "types.h"
#include "handshake.cpp"
#include "frame.cpp"
#include "pipe.cpp"

static struct {
	SOCKET server;
	IOCP* currentCtx;
} acceptIOCP{};

void processRequest(IOCP* ctx, DWORD dwbytes);
void processIOCP(IOCP* ctx, OVERLAPPED* ol, DWORD dwbytes);
__declspec(noreturn) void fatal(const char* msg) {
	printf("[fatal error] %s\nExit...\n", msg);
	ExitProcess(1);
}
void parse_keepalive(IOCP* ctx) {
	if (ctx->firstCon) {
		ctx->firstCon = false;
		int iResult = 0;
		iResult = setsockopt(ctx->client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
			(char*)&acceptIOCP.server, sizeof(acceptIOCP.server));

		auto connection = ctx->p.headers.find("Connection");
		if (connection != ctx->p.headers.end()) {
			if (connection->second == "keep-alive" || connection->second == "Keep-Alive") {
				ctx->keepalive = true;
				DWORD yes = TRUE;
				int success = setsockopt(ctx->client, SOL_SOCKET, SO_KEEPALIVE, (char*)&yes, sizeof(yes));
				if (success != 0) {
					log_fmt("setsockopt(SO_KEEPALIVE) failed on socket %lld\n", ctx->client);
				}
				auto keepalive = ctx->p.headers.find("Keep-Alive");
				if (keepalive == ctx->p.headers.end())
					keepalive = ctx->p.headers.find("keep-alive");
				if (keepalive != ctx->p.headers.end()) {
					auto s = keepalive->second.data();
					auto timeouts = StrStrA(s, "timeout");
					if (timeouts) {
						int timeout;
						int res = sscanf_s(timeouts + 7, "=%d", &timeout);
						if (res > 0) {
							log_fmt("set TCP keep alive timeout=%d\n", timeout);
							int yes = TRUE;
							res = setsockopt(ctx->client, SOL_SOCKET, TCP_KEEPIDLE, (char*)&yes, sizeof yes);
							assert(res == 0);
						}
						else {
							log_puts("Error: failed to parse keepalive seconds...");
						}
					}
				}
			}
		}
	}
	_ASSERT(_CrtCheckMemory());
}
void CloseClient(IOCP* ctx) {
	if (ctx->state != State::AfterClose) {
		ctx->state = State::AfterClose;
		(void)shutdown(ctx->client, SD_BOTH);
		if (closesocket(ctx->client) != 0) {
			assert(0);
		}
		if (ctx->sbuf) {
			ctx->sbuf->~basic_string();
		}
		if (ctx->hasp) {
			(&ctx->p)->~Parse_Data();
		}
		if (ctx->hProcess && ctx->hProcess != INVALID_HANDLE_VALUE) {
			CloseHandle(ctx->hProcess);
		}
		if (HeapFree(heap, 0, ctx) == FALSE) {
			assert(0);
		}
	}
}
int http_on_header_complete(llhttp_t* parser) {
	IOCP* ctx = (IOCP*)parser;
	if (parser->method == llhttp_method_t::HTTP_POST) {
		WSABUF* errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
		parse_keepalive(ctx);
		auto x_FileName = ctx->p.headers.find("X-FileName");
		if (x_FileName != ctx->p.headers.end()) {
			if (UrlUnescapeA(&x_FileName->second[0], NULL, NULL, URL_UNESCAPE_INPLACE | URL_UNESCAPE_AS_UTF8) != S_OK) {
				errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
				goto BAD_REQUEST_AND_RELEASE;
			}
			const char* utf8 = x_FileName->second.data();
			int utf8len = (int)x_FileName->second.size();
			int nChars = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS,
				utf8, utf8len,
				NULL, 0
			);
			if (nChars <= 0) {
				assert(0);
				errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
				goto BAD_REQUEST_AND_RELEASE;
			}
			WCHAR* buf = (WCHAR*)HeapAlloc(heap, 0, (nChars + 1) * 2);
			DWORD size = nChars + 1;
			if (buf == NULL) {
				assert(0);
				errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
				goto BAD_REQUEST_AND_RELEASE;
			}
			buf[nChars] = '\0';
			nChars = MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS,
				utf8, utf8len,
				buf, (nChars + 1)
			);
			if (nChars <= 0) {
				assert(0);
				errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
				goto BAD_REQUEST_AND_RELEASE;
			}
			_ASSERT(_CrtCheckMemory());
			HANDLE hFile = CreateFileW(buf,
				GENERIC_WRITE,
				FILE_SHARE_WRITE,
				NULL,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
				NULL);
			log_fmt("[info] create file: %ws\n", buf);
			if (HeapFree(heap, 0, buf) == FALSE) {
				assert(0);
			}
			if (hFile == INVALID_HANDLE_VALUE || CreateIoCompletionPort(hFile, iocp, (ULONG_PTR)ctx, 0)==NULL) {
				log_fmt("[warn] file handle error %lld, err=%d\n", (__int64)hFile, GetLastError());
				errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
				goto BAD_REQUEST_AND_RELEASE;
			}
			ctx->hProcess = hFile;
			assert(ctx->sendOL.Offset == 0);
			assert(ctx->sendOL.OffsetHigh == 0);
			assert(ctx->sendOL.hEvent==NULL);
			ctx->state = State::POSTWaitFileData;
			ctx->recvBuf->buf = ctx->buf;
			ctx->recvBuf->len = sizeof(ctx->buf);
			return 0;
		}
	BAD_REQUEST_AND_RELEASE:
		CloseClient(ctx); // quickly shutdown POST request
		return -1;
	}

	return 0;
}
int http_on_body(llhttp_t* parser, const char* at, size_t length) {
	IOCP* ctx = reinterpret_cast<IOCP*>(parser);
	if (ctx->hProcess) {
		ctx->state = State::PostWritePartFile;
		assert(ctx->sendOL.hEvent == NULL);
		WriteFile(ctx->hProcess, at, (DWORD)length, NULL, &ctx->sendOL);
	}
	else {
		log_puts("[client error] not a POST body, close client");
		CloseClient(ctx);
	}
	return 0;
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
	if (parser->method == llhttp_method_t::HTTP_GET) {
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
		
	}
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
		*(reinterpret_cast<UINT64*>(&ctx->recvOL.Offset)) += dwbytes;
		ctx->sendBuf->len = dwbytes;
		WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
		if (*(reinterpret_cast<UINT64*>(&ctx->recvOL.Offset)) == ctx->filesize) {
			log_puts("file reading complete");
			if (ctx->keepalive) {
				ctx->state = State::RecvNextRequest;
			}
			else {
				ctx->state = State::AfterSendHTML;
			}
		}
		else {
			ctx->state = State::ReadStaticFile;
		}
	}break;
	case State::RecvNextRequest:
	{
		puts("State::RecvNextRequest");
		CloseHandle(ctx->hProcess);
		ctx->dwFlags = 0;
		assert(ctx->recvBuf[0].buf == ctx->buf);
		assert(ctx->recvBuf[0].len == sizeof(ctx->buf));
		assert(ctx->recvOL.hEvent == NULL);
		ctx->recvOL.Offset = ctx->recvOL.OffsetHigh = 0;
		WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		ctx->state = State::AfterRecv;
	}break;
	case State::ListDirRecvNextRequest:
	{
		if (ctx->keepalive) {
			ctx->sbuf->~basic_string();
			ctx->state = State::AfterRecv;
			ctx->dwFlags = 0;
			assert(ctx->recvOL.hEvent == NULL);
			assert(ctx->recvBuf[0].buf == ctx->buf);
			assert(ctx->recvBuf[0].len == sizeof(ctx->buf));
			WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		}
		else {
			CloseClient(ctx);
		}
		log_fmt("State::ListDirRecvNextRequest, err=%d, keepalive: %s\n", WSAGetLastError(), std::to_string(ctx->keepalive).data());
	}break;
	case State::PostWritePartFile:
	{
		*(reinterpret_cast<UINT64*>(& ctx->sendOL.Offset)) += dwbytes;
		if (ctx->p.parser.content_length==0) {
			CloseHandle(ctx->hProcess);
			ctx->state = State::AfterSendHTML;
			ctx->sendBuf->buf = HTTP_ERR_RESPONCE::post_ok;
			ctx->sendBuf->len = cstrlen(HTTP_ERR_RESPONCE::post_ok);
//			ctx->sendOL.Offset = ctx->sendOL.OffsetHigh = 0;
			WSASend(ctx->client, ctx->sendBuf, 1, NULL, 0, &ctx->sendOL, NULL);
			break;
		}
		WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		ctx->state = State::POSTWaitFileData;
	}break;
	case State::POSTWaitFileData:
	{
		auto err = llhttp_execute(&ctx->p.parser, ctx->buf, dwbytes);
		if (err != llhttp_errno::HPE_OK) {
			assert(0);
			CloseClient(ctx);
		}
		assert(ctx->state == State::PostWritePartFile);
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
		log_fmt("invalid state: %u\n", ctx->state);
		assert(0);
	}
	}
}
void processRequest(IOCP* ctx, DWORD dwbytes) {
	ctx->buf[dwbytes] = '\0';
	ctx->hasp = true;
	if (ctx->firstCon) {
		new(&ctx->p)Parse_Data{};
	}
	enum llhttp_errno err = llhttp_execute(&ctx->p.parser, ctx->buf, dwbytes);
	if (ctx->p.parser.method == llhttp_method_t::HTTP_POST) {
		if (ctx->state == State::POSTWaitFileData) {
			WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
		}
		return;
	}
	if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
		log_fmt("llhttp_execute error: %s\n", llhttp_errno_name(err));
		CloseClient(ctx);
		return;
	}
	if (ctx->p.parser.http_major != 1 || (ctx->p.parser.http_minor != 1 && ctx->p.parser.http_minor != 0)) {
		log_fmt("version not accepable HTTP/%d.%d\n", (int)ctx->p.parser.http_major, (int)ctx->p.parser.http_minor);
		CloseClient(ctx);
		return;
	}
	log_fmt("[%s] %s\n", llhttp_method_name((llhttp_method)ctx->p.parser.method), ctx->p.uri);
	WSABUF* errBuf = &HTTP_ERR_RESPONCE::internal_server_error;
	switch (ctx->p.parser.method) {
	case llhttp_method::HTTP_GET: {
		switch (err) {
		case HPE_OK:
		{
			parse_keepalive(ctx);
			auto file = encodePath(ctx, ctx->p);
			HANDLE hFile = CreateFileW(file,
				GENERIC_READ,
				FILE_SHARE_READ,
				NULL,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
				NULL);
			if (hFile == INVALID_HANDLE_VALUE) {
				if (GetLastError() == ERROR_ACCESS_DENIED) {
					std::wstring wpath{ file };
					wpath += L"/*";
					WIN32_FIND_DATAW fdata;
					WideCharToMultiByte(CP_UTF8, 0,
						file, -1,
						ctx->buf, sizeof(ctx->buf),
						NULL, NULL
					);
					HANDLE hFind = FindFirstFileW(wpath.data(), &fdata);
					if (hFind == INVALID_HANDLE_VALUE) {
						goto BAD_REQUEST_AND_RELEASE;
					}
					{
						ctx->state = State::ListDirRecvNextRequest;
						ctx->sbuf = new std::string();
						*ctx->sbuf += ctx->buf;
						*ctx->sbuf += "/*\"></base><ul>";
						do {
							int b = WideCharToMultiByte(CP_UTF8, 0,
								fdata.cFileName, -1,
								ctx->buf, sizeof(ctx->buf),
								NULL, NULL
							);
							assert(b > 0);
							*ctx->sbuf += "<li><a href=\"";
							*ctx->sbuf += ctx->buf;
							*ctx->sbuf += "\">";
							*ctx->sbuf += ctx->buf;
							*ctx->sbuf += "</a></li>";
						} while (FindNextFileW(hFind, &fdata) != 0);
						FindClose(hFind);
						*ctx->sbuf += "</ul></body></html>";
#define LISTDIR_FIRST "<!DOCTYPE html><html><head><script src=\"/upload.js\"></script><title>Directory Listing For</title></head>"\
							"<body><h1>Directory listing for</h1>"\
							"<div id=\"upload\"><input type=\"file\" id=\"f\" multiple><button onclick=\"fsubmit()\">Upload</button></div>"\
							"<base href=\"/"
						int n = snprintf(
							ctx->buf,
							sizeof(ctx->buf),
								"HTTP/1.1 200 OK\r\n"
								"Content-Type: text/html; charset=utf-8\r\n"
								"Connection: %s\r\n"
								"Content-Length: %zu\r\n"
								"\r\n"
								LISTDIR_FIRST, 
							ctx->keepalive ? "keep-alive" : "close", 
							ctx->sbuf->length() + cstrlen(LISTDIR_FIRST)
						);
						
						ctx->sendBuf[0].buf = ctx->buf;
						ctx->sendBuf[0].len = (DWORD)n;
						ctx->sendBuf[1].buf = (CHAR*)ctx->sbuf->c_str();
						ctx->sendBuf[1].len = (DWORD)ctx->sbuf->size();

						_ASSERT(_CrtCheckMemory());
						WSASend(ctx->client, ctx->sendBuf, 2, NULL, 0, &ctx->sendOL, NULL);
						break;
					}
				}
				errBuf = &HTTP_ERR_RESPONCE::not_found;
				goto BAD_REQUEST_AND_RELEASE;
			}
			HANDLE r = CreateIoCompletionPort(hFile, iocp, (ULONG_PTR)ctx, 0);
			assert(r);
			const char* mine = getType(file);
			ctx->hProcess = hFile;
			LARGE_INTEGER fsize{};
			BOOL bSuccess = GetFileSizeEx(hFile, &fsize);
			ctx->filesize = (UINT64)fsize.QuadPart;
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
			parse_keepalive(ctx);
			auto upgrade = ctx->p.headers.find("Upgrade");
			auto pro = ctx->p.headers.find("Sec-WebSocket-Protocol");
			if (upgrade != ctx->p.headers.end() && pro != ctx->p.headers.end()) {
				if (upgrade->second == "websocket") {
					auto ws_key = ctx->p.headers.find("Sec-WebSocket-Key");
					if (ws_key != ctx->p.headers.end()) {
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
						ctx->coord.X = ctx->p.cols;
						ctx->coord.Y = ctx->p.cols;
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
		// no parse_keepalive(ctx);, connection close
		if (err == llhttp_errno::HPE_OK) {
			auto file = encodePath(ctx, ctx->p);
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
void RunIOCPLoop() {
	DWORD dwbytes = 0;
	IOCP* ctx = NULL;
	OVERLAPPED* ol = NULL;
	for (;;) {
		BOOL ret = GetQueuedCompletionStatus(iocp, & dwbytes, (PULONG_PTR)&ctx, & ol, INFINITE);
		if (ret == FALSE) {
			if (ctx) {
				if ((void*)ctx == (void*)pAcceptEx) {
					assert(0);
					accept_next();
					continue;
				}
				int err = GetLastError();
				switch (err) {
				case ERROR_CONNECTION_ABORTED:
				{
					CloseClient(ctx);
				}break;
				case ERROR_OPERATION_ABORTED:
				{
					log_puts("[error] ERROR_OPERATION_ABORTED");
				}break;
				default:
					assert(0);
				}

			}
			continue;
		}
		if ((void*)ctx == (void*)pAcceptEx) {
			if (dwbytes != 0) {
				processRequest(acceptIOCP.currentCtx, dwbytes);
			}
			else {
				log_fmt("[client error] empty request");
				CloseClient(acceptIOCP.currentCtx);
			}
			accept_next();
			continue;
		}
		if (ctx == NULL || ol == NULL) {
			if (ctx == (IOCP*)1) {
				log_puts("[info] Exit WorkerThread");
				return;
			}
			assert(0);
			continue;
		}
		if (dwbytes == 0) {
			CloseClient(ctx);
			continue;
		}
		if (ol == &dumyOL) {
			log_puts("ignore dumyOL");
			continue;
		}
		processIOCP(ctx, ol, dwbytes);
	}
}
void accept_next() {
	while (1) {
		SOCKET client = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
		BOOL bSuccess = SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
		if (bSuccess == FALSE) {
			fatal("SetHandleInformation");
		}
		IOCP* ctx = (IOCP*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(IOCP));
		if (ctx == NULL) {
			fatal("HeapAlloc");
		}
		HANDLE bRet = CreateIoCompletionPort((HANDLE)client, iocp, (ULONG_PTR)ctx, 0);
		if(bRet==NULL){
			fatal("CreateIoCompletionPort");
		}
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
BOOL WINAPI ConsoleHandler(DWORD event)
{
	switch (event) {
	case CTRL_C_EVENT:
		PostQueuedCompletionStatus(iocp, 0, 1, 0);
		return TRUE;
	}
	return FALSE;
}
int wmain(void)
{
	system("chcp 65001");
	SetConsoleTitleW(L"Web Server");
	{
		WSADATA wsaData{};
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			fatal("WSAStartup");
		}
	}
	iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	heap = GetProcessHeap();
	if (heap == NULL) {
		fatal("GetProcessHeap");
	}
	if (iocp == NULL) {
		fatal("CreateIoCompletionPort");
	}
	if (!initHash()) {
		fatal("initHash");
	}
	sockaddr_in ip4{ .sin_family = AF_INET, .sin_port=htons(80)};
	/*
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
	}*/

	acceptIOCP.server = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (acceptIOCP.server == INVALID_SOCKET) {
		fatal("WSASocketW");
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
	if (CreateIoCompletionPort((HANDLE)acceptIOCP.server, iocp, (ULONG_PTR)pAcceptEx, 0) == NULL) {
		fatal("CreateIoCompletionPort");
	}
	log_puts("[info] server start running");
	accept_next();
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	RunIOCPLoop();

	shutdown(acceptIOCP.server, SD_BOTH);
	closesocket(acceptIOCP.server);
	WSACleanup();

	closeHash();
	_ASSERT(_CrtCheckMemory());
	CloseHandle(iocp);
	CloseHandle(heap);
	log_puts("[info] Exit Process");
}

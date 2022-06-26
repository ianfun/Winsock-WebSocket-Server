HANDLE newPipe(WCHAR buf[50], DWORD dwOpenMode) {
	HANDLE ret = 0;
	static __int64 volatile c = 0;
	for (;;) {
		swprintf(buf, 50, L"\\\\?\\pipe\\child\\%lld-%lld", InterlockedIncrement64(&c), GetTickCount64());
		ret = CreateNamedPipeW(
			buf,
			dwOpenMode,
			PIPE_TYPE_BYTE,
			1,
			4096,
			4096,
			5000,
			NULL);
		if (ret != INVALID_HANDLE_VALUE)
			return ret;
		int err = GetLastError();
		if (err != ERROR_PIPE_BUSY && err != ERROR_ACCESS_DENIED) {
			printf("CreateNamedPipeW: %d\n", err);
			return INVALID_HANDLE_VALUE;
		}
	}
}

BOOL CreateCGI(WCHAR* cmd, IOCP* ctx) {
	{
		STARTUPINFOW si{ .cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES };
		{
			WCHAR buf[50];
			SECURITY_ATTRIBUTES sa{ .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE };
			ctx->sOut.pipe = newPipe(buf, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED);
			if (ctx->sOut.pipe == INVALID_HANDLE_VALUE) {
				return FALSE; 
			}
			si.hStdOutput = CreateFileW(buf, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (si.hStdOutput == INVALID_HANDLE_VALUE) {
				return FALSE;
			}
			ctx->sErr.pipe = newPipe(buf, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED);
			if (ctx->sErr.pipe == INVALID_HANDLE_VALUE) {
				return FALSE;
			}
			si.hStdError = CreateFileW(buf, GENERIC_WRITE, 0, &sa, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (si.hStdError == INVALID_HANDLE_VALUE) {
				return FALSE;
			}
			ctx->sIn.pipe = newPipe(buf, PIPE_ACCESS_OUTBOUND);
			if (ctx->sIn.pipe == INVALID_HANDLE_VALUE) { return FALSE; }
			si.hStdInput = CreateFileW(buf, GENERIC_READ, 0, &sa, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (si.hStdInput == INVALID_HANDLE_VALUE) {
				return FALSE; 
			}
		}
		PROCESS_INFORMATION pInfo;
		if (CreateProcessW(
			NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pInfo)==FALSE){
			return FALSE;
		}
		CloseHandle(pInfo.hThread);
		ctx->hProcess = pInfo.hProcess;
		CloseHandle(si.hStdError);
		CloseHandle(si.hStdInput);
		CloseHandle(si.hStdOutput); 
	}
	HANDLE hRet;
	hRet = CreateIoCompletionPort(ctx->sOut.pipe, iocp, (ULONG_PTR)ctx, 0);
	if (hRet == NULL) {
		return FALSE;
	}
	hRet = CreateIoCompletionPort(ctx->sErr.pipe, iocp, (ULONG_PTR)ctx, 0);
	if (hRet == NULL) {
		return FALSE;
	}
	(void)ReadFile(ctx->sOut.pipe, ctx->sOut.buf, sizeof(ctx->sOut.buf), NULL, &ctx->sOut.ol);
	(void)ReadFile(ctx->sErr.pipe, ctx->sErr.buf, sizeof(ctx->sErr.buf), NULL, &ctx->sErr.ol);
	return TRUE;
}

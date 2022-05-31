SOCKET g_sdListen = INVALID_SOCKET;

VOID CloseClient (Request* ctx) {
    if (ctx->client){
        shutdown(ctx->client, SD_BOTH);
        closesocket(ctx->client);
        ctx->client = NULL;
        ctx->~Request();
        if (!HeapFree(heap,0, ctx)){
            writeLog(L"[warning] HeapFree failed");
        }
    }
}

BOOL initListenSocket(void) {
    int nZero = 0;
    struct addrinfo hints = {0};
    struct addrinfo *addrlocal = NULL;
    hints.ai_flags  = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_IP;
    if( getaddrinfo(NULL, "80", &hints, &addrlocal) != 0 ) {
        printf("%d\n", WSAGetLastError());
        writeLog(L"[fatal error] getaddrinfo failed");
        return FALSE;
    }
    if (addrlocal == NULL) {
        writeLog(L"[fatal error] getaddrinfo failed");
        return FALSE;
    }
    g_sdListen = WSASocketW(addrlocal->ai_family, addrlocal->ai_socktype, addrlocal->ai_protocol, 
                           NULL, 0, WSA_FLAG_OVERLAPPED); 
    if( g_sdListen == INVALID_SOCKET ) {
        writeLog(L"[fatal error] WSASocketW failed");
        return FALSE;
    }
    if( bind(g_sdListen, addrlocal->ai_addr, (int) addrlocal->ai_addrlen) == SOCKET_ERROR ) {
        writeLog(L"[fatal error] bind failed");
        return FALSE;
    }
    if( listen(g_sdListen, SOMAXCONN) == SOCKET_ERROR ) {
        return FALSE;
    }
    freeaddrinfo(addrlocal);
    return TRUE;
}

Request* UpdateCompletionPort(SOCKET client) {
    void* mem = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(Request));
    Request *ctx = new (mem)Request();
    if (ctx==NULL){
        writeLog(L"[error] HeapAlloc failed");
        return NULL;
    }
   ctx->client = client;
   return ctx;
}

void writeFrame(struct Request* ctx, Websocket::Opcode op, const void* msg, ULONG length) {
	ULONG hsize = 2;
	ctx->header[0] = 0b10000000 | BYTE(op);
	if (length < 126){
		ctx->header[1] = (BYTE)length;
	}else if (length < 0b10000000000000000){
		hsize += 2;
		ctx->header[1] = 126;
		ctx->header[2] = (BYTE)(length >> 8);
		ctx->header[3] = (BYTE)length;
	}else{
		puts("error: data size too big");
		return;
	}
	ctx->sendBuf[0].buf = (char*)ctx->header;
	ctx->sendBuf[0].len = hsize;
	ctx->sendBuf[1].buf = (char*)msg;
	ctx->sendBuf[1].len = length;
	WSASend(ctx->client, ctx->sendBuf, 2, NULL, 0, &ctx->sendOL, NULL);
}

void onRecvData(struct Request* ctx) {
	PBYTE mask = (PBYTE)ctx->buf;
	PBYTE payload = mask + 4;
	for (unsigned __int64 i = 0; i < ctx->payload_len; ++i) {
		payload[i] = payload[i] ^ mask[i % 4];
		putchar(payload[i]);
	}
	ULONG len = ctx->payload_len > sizeof(ctx->buf2) ? (ULONG)100 : (ULONG)ctx->payload_len;
	memcpy(ctx->buf2, payload, len);
	writeFrame(ctx, ctx->op, ctx->buf2, len);
	ctx->recvBuf[0].len = 6;
	ctx->dwFlags = MSG_WAITALL;
	ctx->recvBuf[0].buf = ctx->buf;
	WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
	ctx->Reading6Bytes = true;
	/*continue reading 6 bytes*/
}
void onRead6Complete(struct Request *ctx) {
	using namespace Websocket;
	PBYTE data = (PBYTE)ctx->buf;
	BIT FIN = data[0] & 0b10000000;
	if (!FIN) {
		puts("expect FIN set 1");
		CloseClient(ctx);
		return;
	}
	ctx->op = Websocket::Opcode(data[0] & 0b00001111);
	if (data[0] & 0b01110000) {
		puts("RSV is not zero");
		CloseClient(ctx);
		return;
	}
	BIT hasmask = data[1] & 0b10000000;
	if (!hasmask) {
		puts("client MUST set MASK 1");
		CloseClient(ctx);
		return;
	}
	switch (ctx->op) {
	case Opcode::Close:
		puts("connection close");
		CloseClient(ctx);
		return;
	}
	ctx->payload_len = data[1] & 0b01111111;
	PBYTE precv;
	ULONG offset;
	switch (ctx->payload_len) {
	default:
		offset = 0;
		data[0] = data[2];
		data[1] = data[3];
		data[2] = data[4];
		data[3] = data[5];
		precv = data + 4;
		break;
	case 126:
		ctx->payload_len = data[2] << 8 | data[3];
		offset = 2;
		data[0] = data[4];
		data[1] = data[5];
		precv = data + 2;
		break;
	case 127:
		CloseClient(ctx);
		return;
		offset = 8;
		ctx->payload_len = ntohll(*(unsigned __int64*)&data[2]);
		break;
	}
	if (ctx->payload_len == 0) {
		ctx->Reading6Bytes = true;
		/*continue reading 6 bytes*/
		puts("payload length is zero, continue reading");
		return;
	}
	ctx->Reading6Bytes = false;
	ctx->recvBuf[0].len = (ULONG)ctx->payload_len + offset;
	ctx->recvBuf[0].buf = (char*)precv;
	ctx->dwFlags = MSG_WAITALL;
	WSARecv(ctx->client, ctx->recvBuf, 1, NULL, &ctx->dwFlags, &ctx->recvOL, NULL);
	/*read payload data*/
}
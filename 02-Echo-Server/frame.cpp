void writeFrame(struct Request* ctx, Websocket::Opcode op, const void* msg, ULONG length){
	ULONG hsize = 2;
	ctx->buf[0] = 0b10000000 | BYTE(op);
	if (length < 126){
		ctx->buf[1] = (BYTE)length;
	}else if (length < 0b10000000000000000){
		hsize += 2;
		ctx->buf[1] = 126;
		ctx->buf[2] = (BYTE)(length >> 8);
		ctx->buf[3] = (BYTE)length;
	}else{
		ctx->buf[1] = 127;
		hsize += 8;
		*(unsigned __int64*)ctx->buf[2] = htonll(length);
	}
	memcpy(ctx->buf+hsize, msg, length);
	hsize += length;
	assert(sendall(ctx->client, ctx->buf, hsize));
	printf("send %d bytes\n", hsize);
}

const char* mainloop(struct Request *ctx){
	/*
	small: 1(pin*1+rsv*3+opcode*4) + 1(has_mask*1+length*7) + 4(mask) = 6 bytes
	*/
	using namespace Websocket;
	int ret;
	PBYTE data = (PBYTE)ctx->buf;

	for(;;){
		ret = recv(ctx->client, ctx->buf, 6, MSG_WAITALL);
		if (ret <= 0){
			return "connection error 1";
		}
		BIT FIN =  data[0] & 0b10000000;
		if (!FIN){
			return "expect final frame";
		}
		Opcode op = Websocket::Opcode(data[0] & 0b00001111);
		if (data[0] & 0b01110000){
			return "RSV1, RSV2, RSV3 MUST be 0";
		}
		BIT hasmask = data[1] & 0b10000000;
		if (! hasmask){
			return "client MUST set mask bit to 1";
		}
		switch (op){
			case Opcode::Close:
			return "client send a close frame(normal close)";
		}
		unsigned __int64 payload_len = data[1] & 0b01111111;
		int offset;
		PBYTE precv;
		switch(payload_len){
			default:
				offset = 0;
				data[0]=data[2];
				data[1]=data[3];
				data[2]=data[4];
				data[3]=data[5];
				precv = data + 4;
				break;
			case 126:
				payload_len = data[2]<<8 | data[3];
				offset = 2; // 2 bytes as 16 bit
				data[0]=data[4];
				data[1]=data[5];
				precv = data + 2;
				break;
			case 127:
				return "not implemented";
				payload_len = ntohll(*(unsigned __int64*)&data[2]);
				break;
		}
		if (payload_len==0){
			continue;
		}
		ret = recv(ctx->client, (char*)precv, (int)payload_len+offset, MSG_WAITALL);
		PBYTE mask = (PBYTE)ctx->buf;
		PBYTE payload = mask + 4;
		if (ret <= 0){
			return "connection error 2";
		}
		for (unsigned __int64 i = 0; i<payload_len; ++i) {
			payload[i] = payload[i] ^ mask[i % 4];
			putchar(payload[i]);
		}
		writeFrame(ctx, op, payload, (ULONG)payload_len);
	}
}

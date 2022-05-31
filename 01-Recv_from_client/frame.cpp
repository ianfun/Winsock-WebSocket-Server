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
			return "connection error";
		}
		BIT FIN =  data[0] & 0b10000000;
		if (!FIN){
			return "expect final frame";
		}
		if (data[0] & 0b01110000){
			return "RSV1, RSV2, RSV3 MUST be 0";
		}
		BIT hasmask = data[1] & 0b10000000;
		if (! hasmask){
			return "client MUST set mask bit to 1";
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
		printf("payload length=%lld\n", payload_len);
		if (payload_len==0){
			puts("there are no more data to recv");
			continue;
		}
		ret = recv(ctx->client, (char*)precv, (int)payload_len+offset, MSG_WAITALL);
		PBYTE mask = (PBYTE)ctx->buf;
		PBYTE payload = mask + 4;
		if (ret <= 0){
			return "connection error";
		}
		for (unsigned __int64 i = 0; i<payload_len; ++i) {
			payload[i] = payload[i] ^ mask[i % 4];
			putchar(payload[i]);
		}
		putchar(10);
	}
}

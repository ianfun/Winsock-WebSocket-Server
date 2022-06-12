## echo server

To send message to client, server will not set MASK bit to 1, and no mask data.

```
First byte 

FIN=1  RSV1=0  RSV2=0  RSV3=0 | 4 bit Opcode (Text, Binary...)

Second Byte MUST send MSB to 0 (no MASK)

if payload length less or equal than 125
   Second Byte is payload length
else if payload length less than high WORD(2 bytes):
   Second Byte is 126
   Third Byte is the higher of payload length
   Fourth Byte is lower of payload length
else:
   Second Byte is 127(0b1111111)
   The Following 8 byte is a 64 bit integer

```

## File changed

change frame.cpp, add function `writeFrame`, and call `writeFrame` in function `mainloop`

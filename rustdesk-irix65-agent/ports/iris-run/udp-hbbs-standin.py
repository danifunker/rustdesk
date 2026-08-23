# A stand-in for hbbs, just enough to prove the agent's registration loop runs:
# listen on 21116, log every datagram, and answer a RegisterPeer with a
# RegisterPeerResponse so the agent believes it is registered.
#
# RendezvousMessage is a protobuf oneof; register_peer is field 6 and
# register_peer_response is field 7 in this proto. The response body is a single
# optional bool `request_pk` (field 2) which we leave unset, so the whole
# message is field 7 with an empty submessage: 0x3a 0x00.
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", 21116))
print("udp hbbs stand-in on :21116", flush=True)
n = 0
while True:
    data, addr = s.recvfrom(4096)
    n += 1
    printable = "".join(chr(b) if 32 <= b < 127 else "." for b in data[:48])
    print("%s  #%d from %s  %d bytes: %s | %s"
          % (time.strftime("%H:%M:%S"), n, addr, len(data), data[:24].hex(), printable), flush=True)
    s.sendto(bytes([0x3a, 0x00]), addr)

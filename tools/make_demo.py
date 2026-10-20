#!/usr/bin/env python3
"""Write deterministic synthetic CME/LME Ethernet/UDP PCAP, never sends traffic."""
import argparse
import pathlib
import socket
import struct


def put(buf, offset, value, size):
    buf[offset:offset+size] = (value % (1 << (8*size))).to_bytes(size, 'little')


def message(template, block, body):
    return struct.pack('<5H', len(body)+10, block, template, 1, 13) + body


def cme(seq, *messages):
    return struct.pack('<IQ', seq, 1700000000000000000 + seq) + b''.join(messages)


def definition():
    b = bytearray(224)
    b[5] = ord('A')
    b[23:25], b[35:39], b[76:79] = b'PL', b'PLZ6', b'USD'
    for off, value, size in [(55,42,4), (91,100000000,8), (99,1000000000,8)]:
        put(b, off, value, size)
    b += bytes([9,0,0, 4,0,1]) + b'GBX' + bytes([10, 4,0,0, 5,0,0])
    return message(54,224,b)


def quote(rpt, qty, end=True):
    b = bytearray(11)
    put(b, 0, 1700000000000000000+rpt, 8)
    b[8] = 128 if end else 0
    e = bytearray(32)
    for off, value, size in [(0,100000000000,8),(8,qty,4),(12,42,4),(16,rpt,4),(20,1,4),(27,2147483647,4)]:
        put(e, off, value, size)
    e[24:27] = bytes([1,1,ord('0')])
    return message(46,11,b+bytes([32,0,1])+e+bytes([24,0,0,0,0,0,0,0]))


def snapshot():
    b = bytearray(59)
    for off, value, size in [(0,99,4),(4,1,4),(8,42,4),(12,10,4),(16,1700000000000000000,8)]:
        put(b, off, value, size)
    b += bytes([22,0,2])
    for side in range(2):
        e = bytearray(22)
        for off, value, size in [(0,(100+side)*1000000000,8),(8,5,4),(12,1,4)]:
            put(e, off, value, size)
        e[16], e[21] = 1, ord('0')+side
        b += e
    return message(52,59,b)


def auction(price):
    b = bytearray(27)
    put(b, 2, 405, 2)
    put(b, 4, 1700000000000000000, 8)
    b[12] = ord('B')
    put(b, 13, 20261015, 4)
    b[17:21] = b'PTAM'
    put(b, 21, 10, 2)
    put(b, 23, 11, 2)
    put(b, 25, 2, 2)
    b += b'D\x02\x00USD' + struct.pack('<Q',price) + b'EUR' + struct.pack('<Q',price+1)
    b += b'M\x01\x00GBP' + struct.pack('<Q',price+2)
    put(b, 0, len(b), 2)
    return b


def refresh(water):
    return struct.pack('<HHI',8,203,water)


def lme(seq, *messages):
    payload = b''.join(messages)
    return struct.pack('<HBBIQ',16+len(payload),len(messages),0,seq,1700000000000000000+seq)+payload


def frame(group, port, payload, vlan=False, options=False):
    source = '10.123.100.21' if port == 14384 else '103.183.219.33'
    ip = bytearray(24 if options else 20)
    ip[0], ip[8], ip[9] = 0x46 if options else 0x45, 16, 17
    struct.pack_into('>H',ip,2,len(ip)+8+len(payload))
    ip[12:16], ip[16:20] = socket.inet_aton(source), socket.inet_aton(group)
    # IPv4 checksum; UDP checksum is optional for IPv4 and deliberately zero.
    total = sum(struct.unpack('>'+'H'*(len(ip)//2),ip))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    struct.pack_into('>H',ip,10,(~total)&65535)
    g = socket.inet_aton(group)
    eth = bytes([1,0,94,g[1]&127,g[2],g[3]]) + bytes.fromhex('020000000001')
    eth += bytes.fromhex('810000010800' if vlan else '0800')
    return eth+ip+struct.pack('>HHHH',20000,port,8+len(payload),0)+payload


def fixtures():
    return [
        ('224.0.31.174',14384,cme(1,definition())),
        ('224.0.31.132',14384,cme(100,quote(11,3))),
        ('224.0.31.153',14384,cme(1,snapshot())),
        ('224.0.31.132',14384,cme(101,quote(12,4,False))),
        ('224.0.31.132',14384,cme(102,quote(13,6))),
        ('224.0.240.119',61000,lme(100,auction(123456789))),
        ('224.0.240.120',61000,lme(1,refresh(99))),
        ('224.0.240.120',61000,lme(2,auction(123000000),refresh(99))),
        ('224.0.241.119',61000,lme(100,auction(123456789),auction(124000000))),
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',nargs='?',default='build/demo.pcap')
    args = parser.parse_args()
    path = pathlib.Path(args.output)
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open('wb') as out:
        out.write(struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,65535,1))
        for i, (group,port,payload) in enumerate(fixtures()):
            data = frame(group,port,payload,vlan=i%2==0,options=i%3==0)
            out.write(struct.pack('<IIII',1700000000,i*100,len(data),len(data)))
            out.write(data)
    print(f'Wrote {path}: synthetic data, 9 packets, CME 2 complete books; LME 9 reference-price events.')


if __name__ == '__main__':
    main()

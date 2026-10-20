#!/usr/bin/env python3
"""End-to-end replay checks, including endian, VLAN, IP options and corruption."""
import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('demo', root/'tools/make_demo.py')
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)
exe = sys.argv[1]
config = root/'config/replay.yaml'
frames = [demo.frame(g,p,b,vlan=i%2==0,options=i%3==0) for i,(g,p,b) in enumerate(demo.fixtures())]

def write(path, endian='<', nano=False, raw=False, corrupt=None):
    with path.open('wb') as f:
        f.write(struct.pack(endian+'IHHIIII',0xa1b23c4d if nano else 0xa1b2c3d4,2,4,0,0,65535,101 if raw else 1))
        for i, frame in enumerate(frames):
            if raw:
                frame = frame[18 if i%2==0 else 14:]
            if corrupt == 'fragment' and i == 1:
                frame = bytearray(frame)
                off = 0 if raw else (18 if i%2==0 else 14)
                frame[off+6] = 0x20
            length = len(frame)
            f.write(struct.pack(endian+'IIII',1700000000,i*100,length,length+1 if corrupt=='snaplen' and i==1 else length))
            f.write(frame)

def run(path, rate=0):
    return subprocess.run([exe,'--config',str(config),'--pcap',str(path),'--rate',str(rate)],text=True,capture_output=True)

with tempfile.TemporaryDirectory(prefix='fq-pcap-') as directory:
    p = pathlib.Path(directory)/'test.pcap'
    for endian in ('<','>'):
        for raw in (False,True):
            write(p,endian,nano=True,raw=raw)
            r = run(p)
            assert r.returncode==0, r.stderr
            lines=r.stdout.splitlines()
            assert sum(' type=4 ' in l for l in lines)==2,r.stdout
            assert sum(' type=8 ' in l for l in lines)==9,r.stdout
            assert '100.000000000/6' in r.stdout
            assert 'type_name=BOOK' in r.stdout and 'bid=[' in r.stdout and 'ask=[' in r.stdout
            assert 'book_valid=1' in r.stdout and 'tick_size=' in r.stdout
            assert 'parse_errors=0' in r.stderr
    # 一次回放跨两个 feed；每个 feed 各有自己的打印配额，解码总事件仍保留。
    write(p)
    limited = run(p, rate=1)
    assert limited.returncode == 0, limited.stderr
    # 两条普通事件分别来自两个 feed；同步状态是控制事件，不受限速影响。
    assert len(limited.stdout.splitlines()) == 3, limited.stdout
    assert 'feed=1 ' in limited.stdout and 'feed=2 ' in limited.stdout
    assert 'type_name=STATUS' in limited.stdout
    assert 'events=23' in limited.stderr and 'sampled=20' in limited.stderr
    for corruption in ('fragment','snaplen'):
        write(p,corrupt=corruption)
        assert run(p).returncode!=0,corruption
    write(p)
    p.write_bytes(p.read_bytes()[:-1])
    assert run(p).returncode!=0,'truncated final packet'
print('PASS PCAP little/big endian, nanoseconds, Ethernet/VLAN/IP-options/raw IPv4, malformed input')

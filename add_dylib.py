# -*- coding: utf-8 -*-
"""把 seedump.dylib 挂进 IPA：
   1) 往 UnityFramework 的 load command 区追加一条 LC_LOAD_DYLIB
   2) 把 dylib 放进 Payload/SoulKnight.app/Frameworks/
   其余条目按原始压缩字节直通复制（复用 port.py 的 RawZip）。
用法: python add_dylib.py <源IPA> <dylib> <输出IPA>
"""
import os, struct, sys, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "元气骑士移植工具包"))
import port  # noqa: E402

UF = "Payload/SoulKnight.app/Frameworks/UnityFramework.framework/UnityFramework"
DYLIB_ENTRY = "Payload/SoulKnight.app/Frameworks/seedump.dylib"
INSTALL_NAME = b"@executable_path/Frameworks/seedump.dylib\x00"

LC_LOAD_DYLIB = 0xC


def add_load_dylib(buf):
    b = bytearray(buf)
    magic, cputype, cpusub, ftype, ncmds, sizeofcmds, flags, res = struct.unpack_from("<IIIIIIII", b, 0)
    assert magic == 0xFEEDFACF, hex(magic)

    path = INSTALL_NAME + b"\x00" * ((8 - len(INSTALL_NAME) % 8) % 8)
    cmdsize = 24 + len(path)
    lc = struct.pack("<IIIIII", LC_LOAD_DYLIB, cmdsize, 24, 0, 0, 0) + path

    off = 32 + sizeofcmds
    end = off + cmdsize

    # 找第一个有数据的地方，确认不会覆盖
    first = None
    o = 32
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", b, o)
        if cmd == 0x19:
            nsects = struct.unpack_from("<I", b, o + 64)[0]
            p = o + 72
            for _ in range(nsects):
                soff = struct.unpack_from("<I", b, p + 48)[0]
                if soff and (first is None or soff < first):
                    first = soff
                p += 80
        o += sz
    assert first is None or end <= first, ("空隙不够", off, end, first)

    b[off:end] = lc
    struct.pack_into("<II", b, 16, ncmds + 1, sizeofcmds + cmdsize)
    return bytes(b), cmdsize, end, first


def main():
    src, dylib, dst = sys.argv[1], sys.argv[2], sys.argv[3]
    dy = open(dylib, "rb").read()
    print("dylib %d 字节" % len(dy))

    rz = port.RawZip(src, dst)
    with zipfile.ZipFile(src) as z:
        for info in sorted(z.infolist(), key=lambda x: x.header_offset):
            if info.filename == UF:
                uf = z.read(info.filename)
                new, cmdsize, end, first = add_load_dylib(uf)
                print("UnityFramework %d -> %d 字节；新 LC 在 0x%X..0x%X（下一块数据在 %s）"
                      % (len(uf), len(new), 32 + struct.unpack_from("<I", uf, 20)[0], end,
                         hex(first) if first else "无穷"))
                c = zlib.compressobj(9, zlib.DEFLATED, -15)
                data = c.compress(new) + c.flush()
                zi = zipfile.ZipInfo(info.filename, info.date_time)
                zi.flag_bits = info.flag_bits
                zi.compress_type = 8
                zi.create_system = info.create_system
                zi.external_attr = info.external_attr
                zi.internal_attr = info.internal_attr
                zi.CRC = zlib.crc32(new) & 0xFFFFFFFF
                zi.file_size = len(new)
                rz.add_raw(zi, b"", data)
            else:
                rz.src.seek(info.header_offset)
                hdr = rz.src.read(30)
                nlen, elen = struct.unpack_from("<HH", hdr, 26)
                extra = rz.src.read(nlen + elen)[nlen:]
                data = rz.src.read(info.compress_size)
                rz.add_raw(info, extra, data)
    rz.add_new(DYLIB_ENTRY, dy, 0o100755)
    rz.finish()
    print("写出 %s  %.1f MB" % (dst, os.path.getsize(dst) / 1048576))


main()

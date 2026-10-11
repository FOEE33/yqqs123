#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""手工构造合法 .deb（标准 ar + gzip tar），不依赖 dpkg-deb 或 macOS 的 BSD ar。
用法: mkdeb.py <架构名> <二进制> <输出deb>"""
import io, os, sys, tarfile, time

CONTROL = """Package: com.foee.seedump
Name: seedump
Version: 1.0
Architecture: {arch}
Description: SoulKnight ItemInfo dumper (task_for_pid based)
Maintainer: foee
Author: foee
Section: Utilities
Priority: optional
"""

POSTINST = r"""#!/bin/sh
LOG=/var/mobile/seedump.log
exec >"$LOG" 2>&1
echo "=== seedump postinst $(date) ==="
BIN=/var/mobile/seedump/seedump
echo "binary: $BIN"
ls -l "$BIN"
i=1
while [ $i -le 20 ]; do
  echo ""
  echo "--- attempt $i ---"
  "$BIN" /var/mobile/seed_dump.txt
  rc=$?
  echo "exit=$rc"
  if [ $rc -eq 0 ]; then break; fi
  if [ $rc -eq 1 ] || [ $rc -eq 4 ] || [ $rc -eq 5 ]; then
    sleep 3
  else
    break
  fi
  i=$((i+1))
done
echo ""
echo "=== done $(date) ==="
ls -l /var/mobile/seed_dump.txt 2>&1
exit 0
"""


POSTRM = r"""#!/bin/sh
# 卸载时清干净：工具本体、日志、以及运行期产生的文件
case "$1" in
  remove|purge)
    rm -rf /var/mobile/seedump
    rm -f  /var/mobile/seedump.log
    ;;
esac
exit 0
"""


def add(tf, name, data, mode):
    ti = tarfile.TarInfo(name)
    ti.size = len(data)
    ti.mode = mode
    ti.mtime = int(time.time())
    ti.uid = 0
    ti.gid = 0
    ti.uname = "root"
    ti.gname = "root"
    tf.addfile(ti, io.BytesIO(data))


def tar_gz(items):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz", format=tarfile.GNU_FORMAT) as tf:
        for name, data, mode in items:
            add(tf, name, data, mode)
    return buf.getvalue()


def ar_entry(name, data):
    # 标准（GNU/SVR4）ar：名字 16 字节，允许 "name/" 结尾
    n = (name + "/").encode()
    assert len(n) <= 16, name
    hdr = (n.ljust(16, b" ")
           + str(int(time.time())).encode().ljust(12, b" ")
           + b"0".ljust(6, b" ")
           + b"0".ljust(6, b" ")
           + b"100644".ljust(8, b" ")
           + str(len(data)).encode().ljust(10, b" ")
           + b"`\n")
    out = hdr + data
    if len(data) % 2:
        out += b"\n"
    return out


def main():
    arch, binary, out = sys.argv[1], sys.argv[2], sys.argv[3]
    blob = open(binary, "rb").read()

    control_tar = tar_gz([
        ("./control", CONTROL.format(arch=arch).encode(), 0o644),
        ("./postinst", POSTINST.encode(), 0o755),
        ("./postrm", POSTRM.encode(), 0o755),
    ])
    data_tar = tar_gz([
        ("./var/mobile/seedump/seedump", blob, 0o755),
    ])

    deb = b"!<arch>\n"
    deb += ar_entry("debian-binary", b"2.0\n")
    deb += ar_entry("control.tar.gz", control_tar)
    deb += ar_entry("data.tar.gz", data_tar)
    open(out, "wb").write(deb)
    print("built %s  %d bytes" % (out, len(deb)))


if __name__ == "__main__":
    main()

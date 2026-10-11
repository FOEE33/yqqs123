#!/bin/bash
# 用法: ./makedeb.sh <架构名> <二进制> <输出deb>
# 不用 dpkg-deb（macOS 上没有），直接 ar + tar 手工构造合法 .deb
set -e
ARCH="$1"; BIN="$2"; OUT="$3"
ROOT="build_$ARCH"
rm -rf "$ROOT" "$OUT"
mkdir -p "$ROOT/DEBIAN" "$ROOT/var/mobile/seedump"
cp "$BIN" "$ROOT/var/mobile/seedump/seedump"
chmod 755 "$ROOT/var/mobile/seedump/seedump"

cat > "$ROOT/DEBIAN/control" <<EOF
Package: com.foee.seedump
Name: seedump
Version: 1.0
Architecture: $ARCH
Description: SoulKnight ItemInfo dumper (task_for_pid based)
Maintainer: foee
Author: foee
Section: Utilities
Priority: optional
EOF

cat > "$ROOT/DEBIAN/postinst" <<'POSTINST'
#!/bin/sh
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
  # 1=没找到进程 4=类没初始化 5=单例没建 -> 值得等一等再试
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
POSTINST
chmod 755 "$ROOT/DEBIAN/postinst"

COPYFILE_DISABLE=1 tar -C "$ROOT/DEBIAN" -czf control.tar.gz .
COPYFILE_DISABLE=1 tar -C "$ROOT"        -czf data.tar.gz var
printf '2.0\n' > debian-binary
ar rcs "$OUT" debian-binary control.tar.gz data.tar.gz
rm -f control.tar.gz data.tar.gz debian-binary
echo "built $OUT"
ls -l "$OUT"

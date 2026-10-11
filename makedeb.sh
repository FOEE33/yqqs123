#!/bin/bash
# 用法: ./makedeb.sh <架构名> <二进制> <输出deb>
set -e
python3 mkdeb.py "$1" "$2" "$3"
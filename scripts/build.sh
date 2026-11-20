#!/usr/bin/env bash
# 用途：在调用方已加载的 ESP-IDF 环境编译及报告尺寸；无参数，失败返回非零，不执行烧录。
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v idf.py >/dev/null 2>&1; then
    echo "请先 source 对应 ESP-IDF 的 export.sh 或本机 ~/esp/activate-esp-idf.sh" >&2
    exit 1
fi
cd "$project_root"
exec idf.py build size

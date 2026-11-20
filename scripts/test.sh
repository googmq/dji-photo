#!/usr/bin/env bash
# 用途：运行无设备测试；无参数，失败返回非零，不读取串口或连接相机。
set -euo pipefail
project_root="$(cd "$(dirname "$0")/.." && pwd)"
exec make -C "$project_root/tests/host" test

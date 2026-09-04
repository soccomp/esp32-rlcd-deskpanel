#!/bin/bash
# RLCD 键盘切页启动器 —— 双击即可运行（会打开一个终端窗口）。
#
# 默认走「全局监听」模式：任何 App 里按 Ctrl+1 / Ctrl+2 / Ctrl+3 都能切页。
# 该模式需要 macOS 辅助功能授权：
#   系统设置 -> 隐私与安全性 -> 辅助功能 -> 允许下面这个 Python 解释器
#   /Users/m1work/.workbuddy/binaries/python/envs/default/bin/python
# 未授权时表现为：窗口显示 listening...，但按键完全没反应。
#
# 若授权不通，改用「终端焦点」模式（零授权）：在终端里执行
#   ./start_rlcd_key.command --stdin
# 然后保持该窗口在最前，按 1 / 2 / 3 切页（无需 Ctrl），q 退出。

cd "$(dirname "$0")" || exit 1
PY="/Users/m1work/.workbuddy/binaries/python/envs/default/bin/python"

if [ ! -x "$PY" ]; then
    echo "找不到 Python 解释器：$PY"
    read -n 1 -s -r -p "按任意键关闭..."
    exit 1
fi

echo "RLCD 键盘切页 —— Ctrl+1 首页 / Ctrl+2 吉他页 / Ctrl+3 摄像头页"
echo "（无反应多是辅助功能授权没给，改用 ./start_rlcd_key.command --stdin）"
echo "关闭本窗口即停止监听。"
echo
exec "$PY" rlcd_key_control.py "$@"

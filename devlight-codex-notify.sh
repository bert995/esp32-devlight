#!/usr/bin/env bash
# Codex 的 notify 程序:Codex 以 `本脚本 <event-json>` 调用。
# 职责:1) 设置 devlight 的 codex 状态;2) 若本机配置了下游 notify(如 Codex Computer Use),原样转发事件(链式)。
payload="$1"
# 实测期落盘,便于观察 Codex 实际 emit 了哪些 type,再细化映射。
echo "$(date -u +%FT%TZ) $payload" >> /tmp/devlight-codex-events.log

# --- 1) 点灯 ---
type=$(printf '%s' "$payload" | /usr/bin/python3 -c "import sys,json; print(json.load(sys.stdin).get('type',''))" 2>/dev/null)
case "$type" in
  agent-turn-complete) state=idle ;;     # 回合结束 -> 绿
  *)                   state=idle ;;     # 未知事件先保守 idle(实测后细化 working/confirm)
esac
"$(dirname "$0")/devlight-set" codex "$state"

# --- 2) 链式转发给下游 notify(可选,机器本地,不入库) ---
# ~/.config/devlight/codex-chain.json 内容 = 原 Codex notify 数组,如:
#   ["/path/to/SkyComputerUseClient", "turn-ended"]
# 把事件 payload 追加到该数组后调用,复刻 Codex 原本的调用方式(后台 fire-and-forget)。
chain="$HOME/.config/devlight/codex-chain.json"
if [ -f "$chain" ]; then
  /usr/bin/python3 -c '
import json,sys,subprocess
try:
    arr=json.load(open(sys.argv[1]))
    subprocess.Popen(arr+[sys.argv[2]])
except Exception:
    pass
' "$chain" "$payload" >/dev/null 2>&1 &
fi
exit 0

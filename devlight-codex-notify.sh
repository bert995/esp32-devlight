#!/usr/bin/env bash
# Codex 的 notify 程序:Codex 用单个 JSON 字符串参数调用本脚本。
# 实测期把事件 JSON 落盘,便于观察 Codex 实际 emit 了哪些 type,再细化映射。
payload="$1"
echo "$(date -u +%FT%TZ) $payload" >> /tmp/devlight-codex-events.log

type=$(printf '%s' "$payload" | /usr/bin/python3 -c "import sys,json; print(json.load(sys.stdin).get('type',''))" 2>/dev/null)
case "$type" in
  agent-turn-complete) state=idle ;;     # 回合结束 -> 绿
  *)                   state=idle ;;     # 未知事件先保守归 idle(实测后再细化 working/confirm)
esac
# 复用 Mac 侧脚本(单一配置点)
"$(dirname "$0")/devlight-set" codex "$state"

# DevLight — ESP32 开发状态指示灯 设计文档

- 日期:2026-05-22
- 硬件:ESP32-WROOM-32E + 三色交通灯模块(G/Y/R/-),CH340 串口
- 接线(沿用现有):`-`→GND,`G`→GPIO25,`Y`→GPIO26,`R`→GPIO27(共阴,高电平点亮)
- 现状基线:`/Users/bytedance/esp32-traffic-light` 红绿灯测试程序已能编译烧录运行(本项目**另存**,不覆盖它)

## 1. 目标

把这盏三色灯接成 **Claude Code + OpenAI Codex 两个编码 agent 的实时状态指示灯**,放在桌上一眼看出当前该干嘛。

## 2. 状态模型与灯色映射

每个 agent 有一个子状态 ∈ `{idle, working, confirm}`:

| 子状态 | 含义 | 触发 |
|---|---|---|
| `working` | 正在开发中 | agent 开始处理 |
| `confirm` | 需要人来确认(授权/输入) | agent 等待人工 |
| `idle` | 已完成 / 空闲 | 本轮结束、会话结束、默认 |

灯只有一盏,显示**两个 agent 聚合后**的状态,优先级 **红 > 黄 > 绿**:

| 聚合结果 | 条件 | 灯效 |
|---|---|---|
| 🔴 红 | 任一 agent = `confirm` | GPIO27 **持续闪烁**(~2Hz) |
| 🟡 黄 | 否则任一 agent = `working` | GPIO26 **持续闪烁**(~2Hz) |
| 🟢 绿 | 否则(都 idle) | GPIO25 **常亮** |
| ⚪ 离线 | WiFi 未连接 | **三灯一起慢闪**(~0.5Hz),提示"灯没联网,别信它" |

闪烁节奏由 ESP32 本地循环驱动,与 Mac 上报频率无关。

## 3. 架构与数据流

```
Claude Code 事件 ──hook──┐
                          ├─→ HTTP GET ─→ ESP32(持有 claude/codex 子状态,算优先级)─→ GPIO
Codex 事件 ──notify脚本──┘
   curl -s --max-time 1 "http://devlight.local/set?agent=<claude|codex>&state=<idle|working|confirm>" || true
```

- 状态聚合逻辑放在 **ESP32**(Mac 侧无常驻进程,hook 就是一行 curl)。
- 服务发现用 **mDNS**:ESP32 注册为 `devlight.local`(macOS 自带 Bonjour 可解析)。

## 4. 组件

### 4.1 ESP32 固件 `esp32-devlight.ino`
- **联网**:连 2.4GHz WiFi → mDNS `devlight.local` → 起 WebServer(ESP32 core 自带 `WiFi`/`WebServer`/`DNSServer`/`ESPmDNS`/`Preferences`,**无需额外库**)。
- **状态存储**:`claude`、`codex` 子状态在内存;WiFi 凭据存 NVS(`Preferences`),掉电不丢。
- **HTTP 接口**:
  - `GET /set?agent=<claude|codex>&state=<idle|working|confirm>` → 更新子状态,返回 `200`;参数非法返回 `400`。
  - `GET /` → 纯文本返回当前 `claude`/`codex` 子状态 + 聚合结果(浏览器调试用)。
- **主循环**:算聚合 → 驱动对应灯效(见 §2),非阻塞实现闪烁(基于 `millis()`,不用 `delay`)。
- **WiFi 容错**:断线自动重连;重连期间走"离线三灯慢闪"。
- **配网(provisioning)**:
  - **优先**:若 `secrets.h` 预置了 SSID/密码(用户直接提供),首次开机直接连。
  - **回退**:无预置凭据、或连接失败超过 N 秒 → 进 **captive portal**:开 AP `DevLight-Setup`,DNS 劫持到本机配网页,手机连上填 WiFi,存 NVS 后重启连入。
  - 凭据**不写进 git、不回显**;`secrets.h` 加入 `.gitignore`,提供 `secrets.h.example` 模板。

### 4.2 Claude Code hooks(追加到 `~/.claude/settings.json`)
保留现有 `SessionStart`/`SessionEnd`(mempalace 同步),仅**追加**:

| 事件 | 上报 state |
|---|---|
| `UserPromptSubmit` | `working` 🟡 |
| `Notification` | `confirm` 🔴 |
| `Stop` | `idle` 🟢 |
| `SessionEnd` | `idle` 🟢(追加到现有 SessionEnd) |

命令统一 `curl -s --max-time 1 "http://devlight.local/set?agent=claude&state=X" >/dev/null 2>&1 || true` —— **超时即弃、绝不阻塞/报错拖慢 Claude**。

### 4.3 Codex 集成
- `~/.codex/config.toml` 设 `notify = ["/Users/bytedance/esp32-devlight/devlight-codex-notify.sh"]`。
- 脚本解析 Codex 传入的事件 JSON,映射到 working/idle/confirm 后 curl(同样非阻塞、失败不报错)。

## 5. 错误处理
- curl `--max-time 1` + `|| true`:灯/网络不可达完全不影响 agent 工作。
- ESP32 WiFi 断线自动重连,期间离线指示。
- 凭据存 NVS,掉电不丢;连不上自动回退配网。

## 6. 已知不确定点(实现阶段需实测,不臆断)
1. **Codex notify 粒度**:Codex 的 `notify` 主要 emit `agent-turn-complete`,可能没有独立的"开始/等确认"事件。需先实测 Codex 实际 emit 哪些事件 JSON,再定 working/confirm/idle 的映射;若只有 turn-complete,Codex 可能只能表达 idle↔working 两态。
2. **Claude `Notification` 语义**:既含"要授权"也含"闲置提醒"。若闲置提醒误触发红灯,按 hook 传入的 message 内容过滤。
3. **mDNS 可达性**:确认这台 Mac 能解析 `devlight.local`(企业网络偶有 mDNS 限制);不行则回退到固定 IP / 在配网页显示 IP。

## 7. 产物清单
- `esp32-devlight/esp32-devlight.ino`(新固件)
- `esp32-devlight/secrets.h`(本地,gitignored)+ `secrets.h.example`
- `esp32-devlight/devlight-codex-notify.sh`
- `~/.claude/settings.json` 追加 hooks(保留现有)
- `~/.codex/config.toml` 改 `notify`
- 烧录参数:FQBN `esp32:esp32:esp32`,端口 `/dev/cu.wchusbserial10`

## 8. 非目标(YAGNI)
- 不做手机/网页实时看板(`GET /` 纯文本调试足够)。
- 不做多灯/多设备、不做历史记录、不做亮度/颜色自定义。
- 不支持企业级 802.1X WiFi(用普通 2.4GHz 或热点)。
- 不动 `esp32-traffic-light` 测试程序。

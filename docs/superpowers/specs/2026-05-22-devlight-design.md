# DevLight — ESP32 开发状态指示灯 设计文档

- 日期:2026-05-22
- 硬件:ESP32-WROOM-32E + 三色交通灯模块(G/Y/R/-),CH340 串口
- 接线(沿用现有):`-`→GND,`G`→GPIO25,`Y`→GPIO26,`R`→GPIO27(共阴,高电平点亮)
- 现状基线:`/Users/bytedance/esp32-traffic-light` 红绿灯测试程序已能编译烧录运行(本项目**另存**,不覆盖它)

## 1. 目标

把这盏三色灯接成 **Claude Code + OpenAI Codex 两个编码 agent 的实时状态指示灯**,放在桌上一眼看出当前该干嘛。

**可复用目标(2026-05-22 追加):** 项目要做成可复用、可推 GitHub 的仓库——用户后续会再做几盏。拓扑为**各连各的、一台机器一盏灯**:每盏灯独立配网、独立命名;这台 Mac 只驱动一盏;别人 clone 仓库、烧录、配网、起名、设一个 URL 即可拥有自己的灯。多盏灯靠**唯一 mDNS 名**互不干扰。

## 1.5 实施状态(2026-05-22 收尾)

- ✅ **固件**:WiFi/mDNS/WebServer、聚合、LED(绿常亮/黄闪/红闪/离线慢闪)、自动重连、captive portal、设备命名,已烧录验证(设备 `devlight-4b70.local`,三色 + 优先级肉眼确认)。
- ✅ **Claude Code 接入**:hooks 已装并实测会随会话状态变灯。
- ✅ **Mac 侧 `devlight-set`**:已修两个关键问题 ——(a)`curl -4` 强制 IPv4,避免 `.local` 的 IPv6 mDNS 查询挂起 ~5s(原 `--max-time 1` 因此必然超时、状态从不生效);(b)后台 fire-and-forget,0.5s 阻塞→0.05s,消除灯滞后。
- ⏸️ **Codex**:用户决定暂缓、自理。脚本 `devlight-codex-notify.sh`(支持链式转发下游)已在仓库,`~/.codex/config.toml` 已**还原**为原 Computer Use,不留侵入。实测确认 Codex notify 可用且 `agent-turn-complete` 信息全,但无"回合开始"事件。
- 已知限制见 README(批准后卡红、Notification 误红、多窗口覆盖)。

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
                          ├─→ devlight-set <agent> <state> ─→ HTTP GET ─→ ESP32(持有 claude/codex 子状态,算优先级)─→ GPIO
Codex 事件 ──notify脚本──┘
   devlight-set 内部:curl -s --max-time 1 "$BASE/set?agent=<claude|codex>&state=<idle|working|confirm>" || true
   $BASE 来自 ~/.config/devlight/url(默认 http://devlight.local)
```

- 状态聚合逻辑放在 **ESP32**(Mac 侧无常驻进程,hook 最终就是一行 curl)。
- 服务发现用 **mDNS**:ESP32 注册为 `devlight-<设备名>.local`(设备名在配网时设定,未设则用芯片 ID 后缀,如 `devlight-a1b2.local`);macOS 自带 Bonjour 可解析。
- **Mac 侧单一配置点**:`devlight-set` 复用脚本从 `~/.config/devlight/url` 读目标 URL,所有 hook/notify 都调它。换灯/换名只改这一个文件。

## 4. 组件

### 4.1 ESP32 固件 `esp32-devlight.ino`
- **联网**:连 2.4GHz WiFi → mDNS `devlight-<设备名>.local` → 起 WebServer(ESP32 core 自带 `WiFi`/`WebServer`/`DNSServer`/`ESPmDNS`/`Preferences`,**无需额外库**)。
- **设备命名(复用关键)**:NVS 存 `name`;主机名 = `devlight-<name>`,`name` 为空时回退 `devlight-<芯片MAC后4位hex>`。配网页提供"设备名"输入。多盏灯因此不撞名。
- **状态存储**:`claude`、`codex` 子状态在内存;WiFi 凭据 + 设备名存 NVS(`Preferences`),掉电不丢。
- **HTTP 接口**:
  - `GET /set?agent=<claude|codex>&state=<idle|working|confirm>` → 更新子状态,返回 `200`;参数非法返回 `400`。
  - `GET /` → 纯文本返回当前 `claude`/`codex` 子状态 + 聚合结果(浏览器调试用)。
- **主循环**:算聚合 → 驱动对应灯效(见 §2),非阻塞实现闪烁(基于 `millis()`,不用 `delay`)。
- **WiFi 容错**:断线自动重连;重连期间走"离线三灯慢闪"。
- **配网(provisioning)**:
  - **优先**:若 `secrets.h` 预置了 SSID/密码(用户直接提供),首次开机直接连。
  - **回退**:无预置凭据、或连接失败超过 ~20 秒 → 进 **captive portal**:开 AP `DevLight-Setup`,DNS 劫持到本机配网页,手机连上填 **WiFi + 设备名**,存 NVS 后重启连入。
  - 凭据**不写进 git、不回显**;`secrets.h` 加入 `.gitignore`,提供 `secrets.h.example` 模板。

### 4.2 Mac 侧复用脚本 `devlight-set`
- 仓库内 `devlight-set`(bash):用法 `devlight-set <agent> <state>`;从 `~/.config/devlight/url` 读 BASE(默认 `http://devlight.local`),执行 `curl -s --max-time 1 "$BASE/set?agent=&state=" || true`。
- **所有 hook 与 Codex notify 都调它** —— 单一配置点,换设备只改 `~/.config/devlight/url`。非阻塞、失败不报错。

### 4.3 Claude Code hooks(追加到 `~/.claude/settings.json`)
保留现有 `SessionStart`/`SessionEnd`(mempalace 同步),仅**追加**,命令统一调 `devlight-set`:

| 事件 | 命令 |
|---|---|
| `UserPromptSubmit` | `devlight-set claude working` 🟡 |
| `Notification` | `devlight-set claude confirm` 🔴 |
| `Stop` | `devlight-set claude idle` 🟢 |
| `SessionEnd` | `devlight-set claude idle` 🟢(追加到现有 SessionEnd) |

### 4.4 Codex 集成
- `~/.codex/config.toml` 设 `notify = ["/Users/bytedance/esp32-devlight/devlight-codex-notify.sh"]`。
- 脚本解析 Codex 传入的事件 JSON,映射到 working/idle/confirm 后调 `devlight-set codex <state>`。

## 5. 错误处理
- curl `--max-time 1` + `|| true`:灯/网络不可达完全不影响 agent 工作。
- ESP32 WiFi 断线自动重连,期间离线指示。
- 凭据存 NVS,掉电不丢;连不上自动回退配网。

## 6. 已知不确定点(实现阶段需实测,不臆断)
1. **Codex notify 粒度**:Codex 的 `notify` 主要 emit `agent-turn-complete`,可能没有独立的"开始/等确认"事件。需先实测 Codex 实际 emit 哪些事件 JSON,再定 working/confirm/idle 的映射;若只有 turn-complete,Codex 可能只能表达 idle↔working 两态。
2. **Claude `Notification` 语义**:既含"要授权"也含"闲置提醒"。若闲置提醒误触发红灯,按 hook 传入的 message 内容过滤。
3. **mDNS 可达性**:确认这台 Mac 能解析 `devlight.local`(企业网络偶有 mDNS 限制);不行则回退到固定 IP / 在配网页显示 IP。

## 7. 产物清单
- `esp32-devlight/esp32-devlight.ino`(新固件,含设备命名)
- `esp32-devlight/secrets.h`(本地,gitignored)+ `secrets.h.example`
- `esp32-devlight/devlight-set`(Mac 侧复用脚本,入库)
- `esp32-devlight/devlight-codex-notify.sh`(入库)
- `esp32-devlight/README.md`(复用/烧录/配网/GitHub 说明)
- git 仓库(分支 main,后续推 GitHub)
- `~/.claude/settings.json` 追加 hooks(保留现有)
- `~/.config/devlight/url`(本机目标地址,**不入库**)
- `~/.codex/config.toml` 改 `notify`
- 烧录参数:FQBN `esp32:esp32:esp32`,端口 `/dev/cu.wchusbserial10`

## 8. 非目标(YAGNI)
- 不做手机/网页实时看板(`GET /` 纯文本调试足够)。
- 不做"一台 Mac 同时驱动多盏灯"、不做不同灯对应不同项目的状态路由(已选**各连各的、一机一灯**;留口子但本期不做)。
- 不做历史记录、不做亮度/颜色自定义。
- 不支持企业级 802.1X WiFi(用普通 2.4GHz 或热点)。
- 不动 `esp32-traffic-light` 测试程序。

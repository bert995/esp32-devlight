# DevLight — ESP32 开发状态指示灯

把一个 ESP32 + 三色交通灯模块,做成 **Claude Code 开发状态指示灯**:一眼看出 AI 该干嘛。
通过 WiFi + HTTP 接收状态,Mac 侧零常驻进程(hook 就是一行命令)。可复用:一台机器一盏灯,clone→烧录→配网→起名即可。

## 灯色映射

| 灯 | 含义 |
|---|---|
| 🟢 绿常亮 | 收工 / 空闲(回合结束、没在等你) |
| 🟡 黄闪(~2Hz) | 我在干活、不需要你(开发中 / 工具刚执行完) |
| 🔴 红闪(~2Hz) | 该你了:我在问你/等你回话,或需要确认(权限 / 输入) |
| ⚪ 三灯一起慢闪(~0.5Hz) | 未联网 / 配网模式 |

多 agent 时按优先级 **红 > 黄 > 绿** 聚合(任一该你了→红;否则任一在干活→黄;否则绿)。

## 硬件接线

ESP32-WROOM-32E + 三色交通灯模块(共阴,高电平点亮):

| 模块 | ESP32 |
|---|---|
| `-` | GND |
| `G` | GPIO25 |
| `Y` | GPIO26 |
| `R` | GPIO27 |

## 接一盏新灯(4 步)

### 1. 烧录固件
需要 `arduino-cli` + ESP32 core(国内镜像见下)。可选:在 `secrets.h`(从 `secrets.h.example` 拷贝)预置 WiFi 和设备名,免去手机配网。
```bash
unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY   # 国内直连镜像
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload -p /dev/cu.wchusbserialXXXX --fqbn esp32:esp32:esp32 .
```
> ESP32 core 国内安装(否则下载极慢):`arduino-cli config set board_manager.additional_urls https://jihulab.com/esp-mirror/espressif/arduino-esp32/-/raw/gh-pages/package_esp32_index_cn.json` → 删掉缓存的 GitHub 索引 `~/Library/Arduino15/package_esp32_index.json` → `arduino-cli core install esp32:esp32@<版本>-cn`(绕过代理直连 dl.espressif.cn)。

### 2. 配网(没预置 secrets 时)
首次开机若连不上,会开热点 **`DevLight-Setup`**。手机连上 → 弹出配网页 → 填 WiFi + 设备名(如 `desk`)→ 保存重启。
- 主机名 = `devlight-<设备名>.local`;设备名留空则用芯片 ID(如 `devlight-4b70.local`)。

### 3. 设 Mac 端目标地址
```bash
mkdir -p ~/.config/devlight
echo "http://devlight-desk.local" > ~/.config/devlight/url   # 换成你的设备主机名
# 找设备名:dns-sd -B _http._tcp local. | grep devlight
```

### 4. 接 Claude Code hooks
把仓库里的 `devlight-set` 和 `devlight-stop` 放进 PATH(或用绝对路径),然后合并 hooks 到 `~/.claude/settings.json`(保留已有 hook):
```bash
python3 - <<'PY'
import json
p='/Users/<你>/.claude/settings.json'
SET='/绝对路径/devlight-set'      # 状态脚本(working/confirm/idle)
STOP='/绝对路径/devlight-stop'    # 回合结束智能判色脚本
d=json.load(open(p)); hooks=d.setdefault('hooks',{})
def ensure(ev,cmd):
    arr=hooks.setdefault(ev,[])
    for g in arr:
        for h in g.get('hooks',[]):
            if 'devlight' in h.get('command',''): return
    arr.append({"hooks":[{"type":"command","command":cmd}]})
ensure('UserPromptSubmit', f"{SET} claude working")
ensure('PostToolUse',      f"{SET} claude working")
ensure('Notification',     f"{SET} claude confirm")
ensure('Stop',             STOP)
ensure('SessionEnd',       f"{SET} claude idle")
json.dump(d,open(p,'w'),indent=2,ensure_ascii=False); print('done')
PY
```
新开一个 Claude Code 会话生效。

> **Stop 用 `devlight-stop` 智能判色**:回合结束时它读会话记录,看我最后一句——以问号结尾(在等你回话)或用了 AskUserQuestion → 红(球在你那);否则 → 绿(收工)。于是「等你」亮红、「做完」亮绿,两种"停下来"区分开了。
> **PostToolUse→working** 是配套:每执行完一个工具就把灯切回黄,批准权限后会立刻回黄(我在干活),不会僵在红。
> 想要最简单的版本(不分"等你/做完",停下即绿),把 `Stop` 直接设成 `devlight-set claude idle` 即可。

## 文件

| 文件 | 作用 | 入库 |
|---|---|---|
| `esp32-devlight.ino` / `devlight_types.h` | 固件 | ✅ |
| `devlight-set` | Mac 侧脚本:`devlight-set <agent> <state>`,目标读 `~/.config/devlight/url` | ✅ |
| `devlight-stop` | Stop 钩子脚本:读会话记录判断「回合结束是在等你还是收工」→ 红/绿 | ✅ |
| `devlight-codex-notify.sh` | (可选)Codex notify 脚本,支持链式转发下游 | ✅ |
| `secrets.h` / `~/.config/devlight/url` / `~/.config/devlight/codex-chain.json` | 本机配置/凭据 | ❌ 不入库 |

## 调试

```bash
curl -4 --noproxy '*' http://devlight-<名>.local/      # 看 claude/codex 子状态 + 聚合
curl -4 --noproxy '*' "http://devlight-<名>.local/set?agent=claude&state=working"   # 手动设状态
```
> `curl -4` 强制 IPv4 —— 否则 `.local` 的 IPv6(AAAA)mDNS 查询会挂起约 5 秒。`devlight-set` 已内置 `-4` + 后台 fire-and-forget(不阻塞 hook)。

## Codex(可选,默认不接)

`devlight-codex-notify.sh` 已就绪,可把 Codex 状态也接进来。Codex 的 `notify` **只能配一个程序**:若你已有别的(如 Codex Computer Use),把它的命令数组写进 `~/.config/devlight/codex-chain.json`,本脚本会在点灯后**原样转发**给它,两者不耽误。
> 实测:Codex 的 notify 主要 emit `agent-turn-complete`(回合结束),信息很全但**没有"回合开始"事件**,所以 Codex 基本只能表达 idle(绿),难像 Claude 那样在干活时亮黄。本期未默认接入。

## 已知限制

- 回合结束的红/绿靠 `devlight-stop` 猜:以"最后一句是否问号结尾"判断,约八九成准,偶尔会错(在等你但没用问号→误判绿;陈述句里带问号→误判红)。
- `Notification` 在 Claude 闲置提醒时也会触发(→红)。
- 多个 Claude Code 窗口同时跑会互相覆盖 `claude` 子状态(本期按单会话设计)。
- 仅支持 2.4GHz WiFi(ESP32 限制),不支持企业级 802.1X。

## 安全

`secrets.h`、`~/.config/devlight/url`、`~/.config/devlight/codex-chain.json` 均**不入库**;WiFi 密码走配网或本地 `secrets.h`,绝不进 git。

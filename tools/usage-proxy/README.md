# AI Usage Proxy

给 Waveshare ESP32 的 **AI Usage** 页面提供只读额度数据。

ESP32 不保存 Cursor / ChatGPT 登录凭据。本进程负责访问 Provider 内部接口，再输出固件使用的稳定 JSON。

## 启动

本机试跑：

```bash
cd tools/usage-proxy
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
# 编辑 .env，填入下面四个变量
uvicorn app:app --host 0.0.0.0 --port 8765
```

## 服务器部署

固件默认请求 `http://38.49.54.174:8765/api/v1/ai-usage`。把本目录拷到那台机器后：

```bash
# 在服务器上
cd /opt/usage-proxy   # 或你放的路径
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
nano .env             # 填四个变量，见下
```

用 systemd 保活（Ubuntu / Debian）：

```bash
sudo tee /etc/systemd/system/usage-proxy.service >/dev/null <<'EOF'
[Unit]
Description=AI Usage Proxy for ESP32
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/usage-proxy
EnvironmentFile=/opt/usage-proxy/.env
ExecStart=/opt/usage-proxy/.venv/bin/uvicorn app:app --host 0.0.0.0 --port 8765
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now usage-proxy
```

防火墙至少放开 8765。更稳妥是只放行家里出口 IP：

```bash
sudo ufw allow 8765/tcp
# 或：sudo ufw allow from <你家公网IP> to any port 8765 proto tcp
```

本机验证：

```bash
curl -sS http://127.0.0.1:8765/healthz
curl -sS -H "Authorization: Bearer <DEVICE_TOKEN>" \
  http://127.0.0.1:8765/api/v1/ai-usage
```

外网再测一次 `http://38.49.54.174:8765/healthz`。ESP32 的 `AI_USAGE_API_TOKEN` 必须和 `.env` 里的 `DEVICE_TOKEN` 一致。

## 环境变量

服务器部署时建议四个都手填，不必拷贝本机登录文件：

| 变量 | 作用 |
|---|---|
| `DEVICE_TOKEN` | ESP32 调用 `/api/v1/ai-usage` 时的 Bearer token |
| `CURSOR_SESSION_TOKEN` | Cursor `WorkosCursorSessionToken` Cookie 值 |
| `CODEX_ACCESS_TOKEN` | ChatGPT / Codex 的 `access_token`，请求头 `Authorization: Bearer ...` |
| `CHATGPT_ACCOUNT_ID` | 请求头 `ChatGPT-Account-Id`，来自 Codex auth 里的 `account_id` |

本机开发可选：

| 变量 | 作用 |
|---|---|
| `CODEX_AUTH_FILE` | 不填上面两个 GPT 变量时，每次刷新重读这个文件，默认 `~/.codex/auth.json` |

怎么填这四个值：

1. `DEVICE_TOKEN`：自己生成一串随机字符，例如 `openssl rand -hex 16`。同时写进服务器 `.env` 和板子的 `secret_config.h`。
2. `CURSOR_SESSION_TOKEN`：浏览器登录 [cursor.com](https://cursor.com) → 开发者工具 → Application → Cookies → `WorkosCursorSessionToken` 的值（不要带名字和分号）。
3. `CODEX_ACCESS_TOKEN` / `CHATGPT_ACCOUNT_ID`：在已登录 Codex 的电脑上打开 `~/.codex/auth.json`，分别取 `tokens.access_token` 和 `tokens.account_id`。

不要把真实 token 提交到 Git。`.env` 已被忽略。

本进程不实现 OAuth refresh。GPT 返回 401 时，`gpt.error_code` 为 `gpt_auth_expired`，更新 token 后再请求即可。

## 接口

### `GET /healthz`

不访问 Cursor / OpenAI。只表示进程还活着。

### `GET /api/v1/ai-usage`

```bash
curl -sS \
  -H "Authorization: Bearer <device-token>" \
  http://127.0.0.1:8765/api/v1/ai-usage | jq
```

双击刷新会带 `X-Refresh: 1`，proxy 会跳过 60 秒 cache。手动验证强制刷新：

```bash
curl -sS \
  -H "Authorization: Bearer <device-token>" \
  -H "X-Refresh: 1" \
  http://127.0.0.1:8765/api/v1/ai-usage | jq
```

响应协议第一版：

```json
{
  "schema_version": 1,
  "generated_at": 1789000000,
  "gpt": {
    "status": "ok",
    "last_success_at": 1789000000,
    "plan": "plus",
    "allowed": true,
    "limit_reached": false,
    "windows": [
      {
        "id": "5h",
        "window_seconds": 18000,
        "used_percent": 0.0,
        "reset_at": 1789027891
      },
      {
        "id": "7d",
        "window_seconds": 604800,
        "used_percent": 0.0,
        "reset_at": 1789614691
      }
    ],
    "credits_balance": 0.0,
    "reset_credits": 2
  },
  "cursor": {
    "status": "ok",
    "last_success_at": 1789000000,
    "plan": "pro_plus",
    "total_used_percent": 21.0446,
    "api_used_percent": 11.3636,
    "on_demand": {
      "enabled": false,
      "used": 0.0,
      "limit": null,
      "remaining": null
    },
    "cycle_start": 1787990160,
    "cycle_end": 1790668560,
    "grok_bot": {
      "available": true,
      "used_percent": 40.0,
      "reset_at": 1789614691
    }
  }
}
```

所有时间都是 Unix epoch 秒（UTC）。时区显示由 ESP32 的 `TZ` / `localtime_r()` 完成。

Provider `status`：

- `ok`：这次请求成功
- `stale`：这次失败，但还留着上次成功的数据
- `error`：启动以来从未成功过

一个 Provider 失败不会改另一个 Provider 的结果，HTTP 仍返回 200。

60 秒内的重复请求直接走 cache，不会再次访问 Cursor / OpenAI。带 `X-Refresh: 1` 或 `Cache-Control: no-cache` 时跳过 cache。单个 Provider 超时 10 秒。

Grok Bot 不是 `usage-summary` 里的字段。proxy 会另外请求 `POST /api/dashboard/get-sand-usage-status`（同一条 Cursor Cookie）。这个请求失败不影响 Total / API；账号没有 Bot 额度时 `grok_bot.available` 为 false，固件隐藏这一行。

响应里不会出现 `email` / `user_id` / `account_id` / `access_token` / `refresh_token` / `WorkosCursorSessionToken`。

## ESP32 配置

复制板级模板并填写：

```bash
cp main/boards/waveshare-s3-rlcd-4.2/secret_config.h.example \
   main/boards/waveshare-s3-rlcd-4.2/secret_config.h
```

```cpp
#define AI_USAGE_API_URL \
    "http://38.49.54.174:8765/api/v1/ai-usage"

#define AI_USAGE_API_TOKEN \
    "replace-with-device-token"
```

`AI_USAGE_API_TOKEN` 必须和 proxy 的 `DEVICE_TOKEN` 一致。`secret_config.h` 已被 gitignore。

## 安全

- 日志不会打印 Authorization、Cookie、token、email、account_id。
- 公网 HTTP 是明文。至少用防火墙限制 8765，有条件再在前面加 HTTPS。
- Cursor / Codex token 只放服务器 `.env`，不要进 Git。

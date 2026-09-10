# AI Usage Proxy

给 Waveshare ESP32 的 **AI Usage** 页面提供只读额度数据。

ESP32 不保存 Cursor / ChatGPT 登录凭据。本进程跑在 Mac / 局域网机器上，负责访问 Provider 内部接口，再输出固件使用的稳定 JSON。

## 启动

```bash
cd tools/usage-proxy
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
# 编辑 .env，填入 DEVICE_TOKEN 和 CURSOR_SESSION_TOKEN
uvicorn app:app --host 0.0.0.0 --port 8765
```

`0.0.0.0:8765` 方便同一局域网里的 ESP32 访问。

## 环境变量

| 变量 | 作用 |
|---|---|
| `DEVICE_TOKEN` | ESP32 调用 `/api/v1/ai-usage` 时的 Bearer token |
| `CURSOR_SESSION_TOKEN` | Cursor `WorkosCursorSessionToken` Cookie 值 |
| `CODEX_AUTH_FILE` | Codex 登录文件，默认 `~/.codex/auth.json` |

不要把真实 token 提交到 Git。`.env` 已被忽略。

GPT / Codex 每次真正刷新时都会重新读取 `auth.json`。执行 `codex logout` / `codex login` 后不必重启 proxy。

本进程不实现 OAuth refresh。GPT 返回 401 时，响应里的 `gpt.error_code` 为 `gpt_auth_expired`，重新 `codex login` 即可。

## 接口

### `GET /healthz`

不访问 Cursor / OpenAI。只表示进程还活着。

### `GET /api/v1/ai-usage`

```bash
curl -sS \
  -H "Authorization: Bearer <device-token>" \
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
    "cycle_end": 1790668560
  }
}
```

所有时间都是 Unix epoch 秒（UTC）。时区显示由 ESP32 的 `TZ` / `localtime_r()` 完成。

Provider `status`：

- `ok`：这次请求成功
- `stale`：这次失败，但还留着上次成功的数据
- `error`：启动以来从未成功过

一个 Provider 失败不会改另一个 Provider 的结果，HTTP 仍返回 200。

60 秒内的重复请求直接走 cache，不会再次访问 Cursor / OpenAI。单个 Provider 超时 10 秒。

响应里不会出现 `email` / `user_id` / `account_id` / `access_token` / `refresh_token` / `WorkosCursorSessionToken`。

## ESP32 配置

复制板级模板并填写：

```bash
cp main/boards/waveshare-s3-rlcd-4.2/secret_config.h.example \
   main/boards/waveshare-s3-rlcd-4.2/secret_config.h
```

```cpp
#define AI_USAGE_API_URL \
    "http://192.168.1.10:8765/api/v1/ai-usage"

#define AI_USAGE_API_TOKEN \
    "replace-with-device-token"
```

`AI_USAGE_API_TOKEN` 必须和 proxy 的 `DEVICE_TOKEN` 一致。`secret_config.h` 已被 gitignore。

## 安全

- 不要把 Cursor / Codex token 部署到公网 VPS。
- 日志不会打印 Authorization、Cookie、token、email、account_id。
- Phase 2 才会做 Cursor `state.vscdb` 自动读 token，不和这一版混在一起。

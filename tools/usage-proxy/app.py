"""Local AI usage proxy.

Reads Cursor / Codex credentials on this machine and exposes a stable
normalized JSON for ESP32. Provider schemas never leave this process.
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

load_dotenv()

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
logger = logging.getLogger("usage-proxy")

CURSOR_USAGE_URL = "https://cursor.com/api/usage-summary"
CURSOR_GROK_URL = "https://cursor.com/api/dashboard/get-sand-usage-status"
GPT_USAGE_URL = "https://chatgpt.com/backend-api/wham/usage"
PROVIDER_TIMEOUT_S = 10.0
CACHE_TTL_S = 60

FORBIDDEN_RESPONSE_KEYS = {
    "email",
    "user_id",
    "userid",
    "account_id",
    "accountid",
    "access_token",
    "accesstoken",
    "refresh_token",
    "refreshtoken",
    "workoscursorsessiontoken",
}

BROWSER_HEADERS = {
    "Accept": "application/json",
    "User-Agent": (
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36"
    ),
}

app = FastAPI(title="AI Usage Proxy", docs_url=None, redoc_url=None)
_cache_lock = asyncio.Lock()
_cached_body: dict[str, Any] | None = None
_cached_at = 0.0
_gpt_last_good: dict[str, Any] | None = None
_cursor_last_good: dict[str, Any] | None = None


def _now_epoch() -> int:
    return int(time.time())


def _expand_path(raw: str) -> Path:
    return Path(os.path.expanduser(os.path.expandvars(raw or ""))).expanduser()


def _as_float(value: Any) -> float | None:
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        text = value.strip()
        if not text:
            return None
        try:
            return float(text)
        except ValueError:
            return None
    return None


def _as_int(value: Any) -> int | None:
    number = _as_float(value)
    if number is None:
        return None
    return int(number)


def _as_bool(value: Any) -> bool | None:
    if isinstance(value, bool):
        return value
    return None


def _as_str(value: Any) -> str | None:
    if isinstance(value, str):
        text = value.strip()
        return text or None
    return None


def parse_epoch(value: Any) -> int | None:
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        epoch = int(value)
        if epoch > 10_000_000_000:
            epoch //= 1000
        return epoch if epoch > 0 else None
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    if text.isdigit():
        return parse_epoch(int(text))
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        parsed = datetime.fromisoformat(text)
    except ValueError:
        return None
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=timezone.utc)
    return int(parsed.timestamp())


def window_id_from_seconds(seconds: int) -> str:
    if seconds <= 0:
        return "unknown"
    known = {
        18000: "5h",
        86400: "24h",
        604800: "7d",
        2592000: "30d",
    }
    if seconds in known:
        return known[seconds]
    if seconds % 86400 == 0:
        return f"{seconds // 86400}d"
    if seconds % 3600 == 0:
        return f"{seconds // 3600}h"
    return f"{seconds}s"


def _contains_forbidden_keys(payload: Any) -> list[str]:
    found: list[str] = []

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for key, child in node.items():
                if str(key).replace("_", "").lower() in FORBIDDEN_RESPONSE_KEYS:
                    found.append(str(key))
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(payload)
    return found


def resolve_gpt_credentials() -> tuple[str | None, str | None, str | None]:
    env_token = _as_str(os.getenv("CODEX_ACCESS_TOKEN") or os.getenv("CHATGPT_ACCESS_TOKEN"))
    env_account = _as_str(os.getenv("CHATGPT_ACCOUNT_ID") or os.getenv("CODEX_ACCOUNT_ID"))
    file_token = None
    file_account = None
    file_error = None
    if not env_token or not env_account:
        auth_file = os.getenv("CODEX_AUTH_FILE", "~/.codex/auth.json")
        file_token, file_account, file_error = load_codex_auth(auth_file)
    access_token = env_token or file_token
    account_id = env_account or file_account
    if not access_token:
        return None, None, file_error or "gpt_auth_missing"
    return access_token, account_id, None


def load_codex_auth(auth_file: str) -> tuple[str | None, str | None, str | None]:
    path = _expand_path(auth_file)
    try:
        raw = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None, None, "gpt_auth_missing"
    except OSError:
        logger.warning("failed to read Codex auth file")
        return None, None, "gpt_auth_missing"

    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        logger.warning("Codex auth file is not valid JSON")
        return None, None, "gpt_auth_missing"

    tokens = data.get("tokens") if isinstance(data.get("tokens"), dict) else {}
    access_token = _as_str(tokens.get("access_token")) or _as_str(data.get("access_token"))
    account_id = _as_str(tokens.get("account_id")) or _as_str(data.get("account_id"))
    if not access_token:
        return None, None, "gpt_auth_missing"
    return access_token, account_id, None


def normalize_gpt_window(window: Any) -> dict[str, Any] | None:
    if not isinstance(window, dict):
        return None
    seconds = _as_int(window.get("limit_window_seconds"))
    if seconds is None or seconds <= 0:
        return None
    used = _as_float(window.get("used_percent"))
    reset_at = parse_epoch(window.get("reset_at"))
    if reset_at is None:
        reset_after = _as_int(window.get("reset_after_seconds"))
        if reset_after is not None and reset_after >= 0:
            reset_at = _now_epoch() + reset_after
    return {
        "id": window_id_from_seconds(seconds),
        "window_seconds": seconds,
        "used_percent": used if used is not None else 0.0,
        "reset_at": reset_at or 0,
    }


def collect_gpt_windows(rate_limit: dict[str, Any]) -> list[dict[str, Any]]:
    windows: list[dict[str, Any]] = []
    seen: set[tuple[str, int]] = set()

    def add(raw: Any) -> None:
        parsed = normalize_gpt_window(raw)
        if parsed is None:
            return
        key = (parsed["id"], parsed["window_seconds"])
        if key in seen:
            return
        seen.add(key)
        windows.append(parsed)

    add(rate_limit.get("primary_window"))
    add(rate_limit.get("secondary_window"))

    extra = rate_limit.get("additional_rate_limits")
    if isinstance(extra, list):
        for item in extra:
            if isinstance(item, dict):
                add(item.get("window") or item)
    return windows


def normalize_gpt_payload(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError("gpt payload is not an object")

    rate_limit = payload.get("rate_limit") if isinstance(payload.get("rate_limit"), dict) else {}
    credits = payload.get("credits") if isinstance(payload.get("credits"), dict) else {}
    reset_credits = (
        payload.get("rate_limit_reset_credits")
        if isinstance(payload.get("rate_limit_reset_credits"), dict)
        else {}
    )

    plan = _as_str(payload.get("plan_type")) or "unknown"
    allowed = _as_bool(rate_limit.get("allowed"))
    limit_reached = _as_bool(rate_limit.get("limit_reached"))
    balance = _as_float(credits.get("balance"))
    reset_count = _as_int(reset_credits.get("available_count"))

    return {
        "plan": plan,
        "allowed": True if allowed is None else allowed,
        "limit_reached": False if limit_reached is None else limit_reached,
        "windows": collect_gpt_windows(rate_limit),
        "credits_balance": balance if balance is not None else 0.0,
        "reset_credits": reset_count if reset_count is not None else 0,
    }


def normalize_cursor_payload(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError("cursor payload is not an object")

    individual = payload.get("individualUsage")
    if not isinstance(individual, dict):
        individual = {}
    plan = individual.get("plan") if isinstance(individual.get("plan"), dict) else {}
    on_demand = individual.get("onDemand")
    if not isinstance(on_demand, dict):
        on_demand = individual.get("on_demand") if isinstance(individual.get("on_demand"), dict) else {}

    total_used = _as_float(plan.get("totalPercentUsed"))
    api_used = _as_float(plan.get("apiPercentUsed"))
    membership = _as_str(payload.get("membershipType")) or "unknown"

    return {
        "plan": membership,
        "total_used_percent": total_used if total_used is not None else 0.0,
        "api_used_percent": api_used if api_used is not None else 0.0,
        "on_demand": {
            "enabled": bool(_as_bool(on_demand.get("enabled"))),
            "used": _as_float(on_demand.get("used")) or 0.0,
            "limit": _as_float(on_demand.get("limit")),
            "remaining": _as_float(on_demand.get("remaining")),
        },
        "cycle_start": parse_epoch(payload.get("billingCycleStart")) or 0,
        "cycle_end": parse_epoch(payload.get("billingCycleEnd")) or 0,
        "grok_bot": _empty_grok_bot(),
    }


def normalize_grok_bot(payload: Any) -> dict[str, Any] | None:
    if not isinstance(payload, dict):
        return None
    has_limit = payload.get("hasNonZeroIncludedLimit")
    used = _as_float(
        payload.get("usagePercent")
        or payload.get("usedPercent")
        or payload.get("percentUsed")
        or payload.get("includedUsagePercent")
    )
    reset_at = parse_epoch(
        payload.get("nextResetTimestampUtc")
        or payload.get("nextResetAt")
        or payload.get("resetAt")
        or payload.get("reset_at")
    )
    if has_limit is False:
        return _empty_grok_bot()
    if used is None and has_limit is not True:
        return None
    return {
        "available": True,
        "used_percent": used if used is not None else 0.0,
        "reset_at": reset_at or 0,
    }


def _with_provider_status(
    data: dict[str, Any] | None,
    status: str,
    last_success_at: int | None,
    error_code: str | None = None,
) -> dict[str, Any]:
    body: dict[str, Any] = dict(data or {})
    body["status"] = status
    if last_success_at:
        body["last_success_at"] = last_success_at
    if error_code:
        body["error_code"] = error_code
    return body


def _empty_gpt() -> dict[str, Any]:
    return {
        "plan": "",
        "allowed": False,
        "limit_reached": False,
        "windows": [],
        "credits_balance": 0.0,
        "reset_credits": 0,
    }


def _empty_grok_bot() -> dict[str, Any]:
    return {
        "available": False,
        "used_percent": 0.0,
        "reset_at": 0,
    }


def _empty_cursor() -> dict[str, Any]:
    return {
        "plan": "",
        "total_used_percent": 0.0,
        "api_used_percent": 0.0,
        "on_demand": {
            "enabled": False,
            "used": 0.0,
            "limit": None,
            "remaining": None,
        },
        "cycle_start": 0,
        "cycle_end": 0,
        "grok_bot": _empty_grok_bot(),
    }


def _cursor_session_token() -> str:
    token = (os.getenv("CURSOR_SESSION_TOKEN") or "").strip()
    if token.startswith("WorkosCursorSessionToken="):
        token = token.split("=", 1)[1]
    return token


async def fetch_gpt() -> tuple[dict[str, Any] | None, str | None]:
    access_token, account_id, error = resolve_gpt_credentials()
    if error:
        return None, error

    headers = {
        **BROWSER_HEADERS,
        "Authorization": f"Bearer {access_token}",
        "Referer": "https://chatgpt.com/",
    }
    if account_id:
        headers["ChatGPT-Account-Id"] = account_id

    try:
        async with httpx.AsyncClient(timeout=PROVIDER_TIMEOUT_S, follow_redirects=True) as client:
            response = await client.get(GPT_USAGE_URL, headers=headers)
    except httpx.TimeoutException:
        logger.warning("gpt provider timeout")
        return None, "gpt_request_failed"
    except httpx.HTTPError:
        logger.warning("gpt provider request failed")
        return None, "gpt_request_failed"

    if response.status_code in (401, 403):
        logger.warning("gpt auth expired")
        return None, "gpt_auth_expired"
    if response.status_code != 200:
        logger.warning("gpt provider http %s", response.status_code)
        return None, "gpt_request_failed"

    try:
        payload = response.json()
        return normalize_gpt_payload(payload), None
    except (ValueError, json.JSONDecodeError, TypeError):
        logger.warning("gpt provider parse failed")
        return None, "gpt_parse_error"


async def fetch_cursor() -> tuple[dict[str, Any] | None, str | None]:
    token = _cursor_session_token()
    if not token:
        return None, "cursor_auth_missing"

    headers = {
        **BROWSER_HEADERS,
        "Cookie": f"WorkosCursorSessionToken={token}",
        "Referer": "https://cursor.com/settings",
    }

    try:
        async with httpx.AsyncClient(timeout=PROVIDER_TIMEOUT_S, follow_redirects=True) as client:
            response = await client.get(CURSOR_USAGE_URL, headers=headers)
    except httpx.TimeoutException:
        logger.warning("cursor provider timeout")
        return None, "cursor_request_failed"
    except httpx.HTTPError:
        logger.warning("cursor provider request failed")
        return None, "cursor_request_failed"

    if response.status_code in (401, 403):
        logger.warning("cursor auth expired")
        return None, "cursor_auth_expired"
    if response.status_code != 200:
        logger.warning("cursor provider http %s", response.status_code)
        return None, "cursor_request_failed"

    try:
        payload = response.json()
        return normalize_cursor_payload(payload), None
    except (ValueError, json.JSONDecodeError, TypeError):
        logger.warning("cursor provider parse failed")
        return None, "cursor_parse_error"


async def fetch_cursor_grok() -> tuple[dict[str, Any] | None, str | None]:
    token = _cursor_session_token()
    if not token:
        return None, "cursor_auth_missing"

    headers = {
        **BROWSER_HEADERS,
        "Cookie": f"WorkosCursorSessionToken={token}",
        "Origin": "https://cursor.com",
        "Referer": "https://cursor.com/dashboard",
        "Content-Type": "application/json",
    }

    try:
        async with httpx.AsyncClient(timeout=PROVIDER_TIMEOUT_S, follow_redirects=True) as client:
            response = await client.post(CURSOR_GROK_URL, headers=headers, json={})
    except httpx.TimeoutException:
        logger.warning("grok bot provider timeout")
        return None, "grok_request_failed"
    except httpx.HTTPError:
        logger.warning("grok bot provider request failed")
        return None, "grok_request_failed"

    if response.status_code in (401, 403):
        logger.warning("grok bot auth expired")
        return None, "cursor_auth_expired"
    if response.status_code != 200:
        logger.warning("grok bot provider http %s", response.status_code)
        return None, "grok_request_failed"

    try:
        payload = response.json()
        parsed = normalize_grok_bot(payload)
        if parsed is None:
            logger.warning("grok bot provider parse failed")
            return None, "grok_parse_error"
        return parsed, None
    except (ValueError, json.JSONDecodeError, TypeError):
        logger.warning("grok bot provider parse failed")
        return None, "grok_parse_error"


def _apply_fetch_result(
    last_good: dict[str, Any] | None,
    fetched: dict[str, Any] | None,
    error_code: str | None,
    empty: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any] | None]:
    now = _now_epoch()
    if fetched is not None:
        fetched = dict(fetched)
        fetched["last_success_at"] = now
        return _with_provider_status(fetched, "ok", now), fetched
    if last_good is not None:
        stale = dict(last_good)
        return _with_provider_status(stale, "stale", stale.get("last_success_at"), error_code), last_good
    return _with_provider_status(empty, "error", None, error_code), None


async def build_usage_payload() -> dict[str, Any]:
    global _gpt_last_good, _cursor_last_good

    gpt_result, cursor_result, grok_result = await asyncio.gather(
        fetch_gpt(), fetch_cursor(), fetch_cursor_grok()
    )
    gpt_data, gpt_error = gpt_result
    cursor_data, cursor_error = cursor_result
    grok_data, _grok_error = grok_result

    if cursor_data is not None:
        if grok_data is not None:
            cursor_data["grok_bot"] = grok_data
        elif isinstance(_cursor_last_good, dict) and isinstance(
            _cursor_last_good.get("grok_bot"), dict
        ):
            cursor_data["grok_bot"] = _cursor_last_good["grok_bot"]

    gpt_body, _gpt_last_good = _apply_fetch_result(
        _gpt_last_good, gpt_data, gpt_error, _empty_gpt()
    )
    cursor_body, _cursor_last_good = _apply_fetch_result(
        _cursor_last_good, cursor_data, cursor_error, _empty_cursor()
    )

    payload = {
        "schema_version": 1,
        "generated_at": _now_epoch(),
        "gpt": gpt_body,
        "cursor": cursor_body,
    }
    leaked = _contains_forbidden_keys(payload)
    if leaked:
        logger.error("refusing to return forbidden keys")
        raise HTTPException(status_code=500, detail="sanitizer rejected payload")
    return payload


def _extract_bearer(header_value: str) -> str:
    if not header_value:
        return ""
    parts = header_value.split(None, 1)
    if len(parts) != 2 or parts[0].lower() != "bearer":
        return ""
    return parts[1].strip()


def _require_device_token(request: Request) -> None:
    expected = (os.getenv("DEVICE_TOKEN") or "").strip()
    provided = _extract_bearer(request.headers.get("Authorization", ""))
    if not expected:
        raise HTTPException(status_code=500, detail="device token not configured")
    same_len = len(provided) == len(expected)
    token_ok = same_len and hmac.compare_digest(provided, expected)
    if not provided or not token_ok:
        raise HTTPException(status_code=401, detail="unauthorized")


@app.get("/healthz")
async def healthz() -> dict[str, bool]:
    return {"ok": True}


def _wants_fresh(request: Request) -> bool:
    refresh = (request.headers.get("x-refresh") or "").strip()
    cache_control = (request.headers.get("cache-control") or "").lower()
    return refresh == "1" or "no-cache" in cache_control


@app.get("/api/v1/ai-usage")
async def ai_usage(request: Request) -> JSONResponse:
    _require_device_token(request)

    global _cached_body, _cached_at
    async with _cache_lock:
        now = time.time()
        if (
            not _wants_fresh(request)
            and _cached_body is not None
            and (now - _cached_at) < CACHE_TTL_S
        ):
            return JSONResponse(_cached_body)

        payload = await build_usage_payload()
        _cached_body = payload
        _cached_at = now
        grok = payload["cursor"].get("grok_bot") or {}
        logger.info(
            "usage refreshed gpt=%s cursor=%s grok=%s",
            payload["gpt"].get("status"),
            payload["cursor"].get("status"),
            "yes" if grok.get("available") else "no",
        )
        return JSONResponse(payload)

import asyncio
import base64
import json
from datetime import datetime, timezone
from time import monotonic

import httpx
from pydantic import SecretStr, ValidationError

from app.db import Database
from app.events import ModelObservation


class InferenceError(RuntimeError):
    pass


class ModelBestClient:
    def __init__(
        self,
        api_key: str | SecretStr | None,
        fast_model: str,
        accurate_model: str,
        base_url: str = "https://api.modelbest.cn/v1",
        transport: httpx.AsyncBaseTransport | None = None,
        db: Database | None = None,
    ):
        self.api_key = api_key.get_secret_value() if isinstance(api_key, SecretStr) else api_key
        self.models = {"fast": fast_model, "accurate": accurate_model}
        self.base_url = base_url.rstrip("/")
        self.transport = transport
        self.db = db

    async def infer(self, images: list[bytes], prompt: str, tier: str = "fast", event_id: str | None = None) -> ModelObservation:
        return await self._infer(images, prompt, tier, event_id, self.parse_observation)

    async def infer_many(self, images: list[bytes], prompt: str, tier: str = "fast", event_id: str | None = None) -> list[ModelObservation]:
        return await self._infer(images, prompt, tier, event_id, self.parse_observations, max_tokens=600)

    async def _infer(self, images, prompt, tier, event_id, parser, max_tokens=300):
        if not self.api_key:
            raise InferenceError("MODELBEST_API_KEY is not configured")
        if tier not in self.models:
            raise ValueError("tier must be fast or accurate")
        if not images:
            raise ValueError("at least one image is required")
        content: list[dict[str, object]] = [{"type": "text", "text": prompt}]
        for image in images:
            encoded = base64.b64encode(image).decode("ascii")
            content.append({"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{encoded}"}})
        payload = {
            "model": self.models[tier],
            "messages": [{"role": "user", "content": content}],
            "stream": False,
            "max_tokens": max_tokens,
        }
        last_error = ""
        for attempt in range(2):
            started = monotonic()
            try:
                timeout = httpx.Timeout(60, connect=10)
                async with httpx.AsyncClient(transport=self.transport, timeout=timeout) as client:
                    response = await client.post(
                        f"{self.base_url}/chat/completions",
                        headers={"Authorization": f"Bearer {self.api_key}"},
                        json=payload,
                    )
                response.raise_for_status()
                body = response.json()
                text = body["choices"][0]["message"]["content"]
                observation = parser(text)
                usage = body.get("usage", {})
                self._record(event_id, tier, "ok", started, usage=usage)
                return observation
            except (httpx.HTTPError, KeyError, IndexError, TypeError, ValueError, ValidationError, json.JSONDecodeError) as exc:
                last_error = f"{type(exc).__name__}: {exc}"
                self._record(event_id, tier, "error", started, error=last_error)
                if attempt == 0:
                    payload["messages"][0]["content"][0] = {
                        "type": "text",
                        "text": f"{prompt}\n只返回一个符合要求的 JSON 对象，不要 Markdown 或解释。",
                    }
                    retry_delay = 0.5
                    if isinstance(exc, httpx.HTTPStatusError) and exc.response.status_code == 429:
                        retry_delay = 2.0
                        retry_after = exc.response.headers.get("Retry-After")
                        try:
                            if retry_after is not None:
                                retry_delay = min(30.0, max(2.0, float(retry_after)))
                        except (TypeError, ValueError):
                            retry_delay = 2.0
                    await asyncio.sleep(retry_delay)
                elif tier == "fast" and isinstance(exc, httpx.HTTPStatusError) and exc.response.status_code == 409:
                    try:
                        error_body = exc.response.json()
                    except ValueError:
                        error_body = {}
                    if error_body.get("error", {}).get("type") == "lis_route_denied":
                        return await self._infer(images, prompt, "accurate", event_id, parser, max_tokens=max_tokens)
        raise InferenceError(last_error)

    @staticmethod
    def _parse_json(content: str):
        stripped = content.strip()
        if stripped.startswith("```") and stripped.endswith("```"):
            lines = stripped.splitlines()
            stripped = "\n".join(lines[1:-1]).strip()
        return json.loads(stripped)

    @staticmethod
    def parse_observation(content: str) -> ModelObservation:
        return ModelObservation.model_validate(ModelBestClient._parse_json(content))

    @staticmethod
    def parse_observations(content: str) -> list[ModelObservation]:
        payload = ModelBestClient._parse_json(content)
        if isinstance(payload, list):
            raw_observations = payload
            empty_box_ids = []
        elif isinstance(payload, dict):
            raw_observations = payload.get("observations", payload.get("cats", []))
            empty_box_ids = payload.get("empty_box_ids", [])
        else:
            raise ValueError("multi-cat response must be an object or list")
        if not isinstance(raw_observations, list) or not isinstance(empty_box_ids, list):
            raise ValueError("observations and empty_box_ids must be lists")
        observations = []
        for item in raw_observations:
            try:
                observations.append(ModelObservation.model_validate(item))
            except (ValidationError, TypeError, ValueError):
                continue
        for box_id in empty_box_ids:
            try:
                observations.append(
                    ModelObservation(
                        cat_present=False,
                        box_id=box_id,
                        action="none",
                        subject_id=None,
                        cat_center=None,
                    )
                )
            except (ValidationError, TypeError, ValueError):
                continue
        return observations

    def _record(self, event_id: str | None, tier: str, status: str, started: float, usage: dict | None = None, error: str = "") -> None:
        if self.db is None:
            return
        usage = usage or {}
        with self.db.connect() as connection:
            connection.execute(
                "INSERT INTO model_calls(event_id,tier,model,status,latency_ms,prompt_tokens,completion_tokens,error,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                (
                    event_id,
                    tier,
                    self.models[tier],
                    status,
                    round((monotonic() - started) * 1000),
                    usage.get("prompt_tokens"),
                    usage.get("completion_tokens"),
                    error,
                    datetime.now(timezone.utc).isoformat(),
                ),
            )

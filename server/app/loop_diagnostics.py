import asyncio
import json
from pathlib import Path
from time import monotonic, time_ns


def record_lag(directory: Path, lag_seconds: float) -> bool:
    if lag_seconds < 0.2:
        return False
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "event-loop.jsonl").open("a") as log:
        log.write(json.dumps({"time_ns": time_ns(), "lag_ms": round(lag_seconds * 1000, 3)}) + "\n")
    return True


async def monitor_event_loop(directory: Path) -> None:
    previous = monotonic()
    while True:
        await asyncio.sleep(0.05)
        current = monotonic()
        record_lag(directory, current - previous)
        previous = current

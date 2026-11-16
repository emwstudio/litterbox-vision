import json
from collections import Counter
from datetime import date, datetime, time, timedelta, timezone
from pathlib import Path
from zoneinfo import ZoneInfo

from app.db import Database


class StatsService:
    def __init__(self, db: Database, timezone_info: ZoneInfo, demo_path: Path | None = None):
        self.db = db
        self.timezone = timezone_info
        self.demo_path = demo_path

    def daily(self, day: str) -> dict[str, object]:
        selected = date.fromisoformat(day)
        start = datetime.combine(selected, time.min, tzinfo=self.timezone)
        end = start + timedelta(days=1)
        with self.db.connect() as connection:
            cats = connection.execute("SELECT * FROM cats ORDER BY created_at, id").fetchall()
        demo_counts = self._demo_counts(day, cats)
        is_demo = demo_counts is not None
        per_cat: dict[str, Counter] = demo_counts or {}
        last_confidence: dict[str, float] = {}
        box_totals: Counter = Counter()
        unknown_count = 0
        if is_demo:
            for cat_id, counts in per_cat.items():
                box_totals.update(counts)
                last_confidence[cat_id] = 1.0
        else:
            with self.db.connect() as connection:
                events = connection.execute(
                    "SELECT * FROM events WHERE status='counted' AND started_at >= ? AND started_at < ? ORDER BY started_at",
                    (start.astimezone(timezone.utc).isoformat(), end.astimezone(timezone.utc).isoformat()),
                ).fetchall()
            for event in events:
                box_totals[event["box_id"]] += 1
                if event["cat_id"] == "unknown":
                    unknown_count += 1
                    continue
                per_cat.setdefault(event["cat_id"], Counter())[event["box_id"]] += 1
                last_confidence[event["cat_id"]] = event["confidence"]
        result = []
        for cat in cats:
            counts = per_cat.get(cat["id"], Counter())
            total = sum(counts.values())
            highest = max(counts.values(), default=0)
            favorites = sorted(box for box, count in counts.items() if count == highest and highest > 0)
            result.append(
                {
                    "cat_id": cat["id"],
                    "name": cat["name"],
                    "avatar_url": f"/api/media/{cat['avatar_path']}" if cat["avatar_path"] else "",
                    "today_count": total,
                    "box_counts": [{"box_id": box, "count": count} for box, count in sorted(counts.items())],
                    "favorite_box_ids": favorites,
                    "favorite_badges": [f"/api/media/badges/{box}.png" for box in favorites],
                    "last_event_confidence": last_confidence.get(cat["id"]),
                }
            )
        return {
            "date": day,
            "frame_url": "/api/display/frame.jpg?target=stats&overlay=false",
            "updated_at": datetime.now(timezone.utc).isoformat(),
            "cats": result,
            "box_totals": dict(box_totals),
            "unknown_count": unknown_count,
            "demo": is_demo,
        }

    def _demo_counts(self, day: str, cats) -> dict[str, Counter] | None:
        if self.demo_path is None or not self.demo_path.is_file():
            return None
        document = json.loads(self.demo_path.read_text())
        if document.get("date") != day:
            return None
        configured = document.get("counts")
        if not isinstance(configured, dict):
            raise ValueError("demo counts must be an object keyed by cat name")
        cat_ids = {cat["name"]: cat["id"] for cat in cats}
        per_cat = {}
        for name, counts in configured.items():
            if name not in cat_ids:
                raise ValueError(f"unknown demo cat: {name}")
            if not isinstance(counts, dict):
                raise ValueError(f"demo counts for {name} must be an object")
            counter = Counter()
            for box, count in counts.items():
                if box not in {"box-1", "box-2", "box-3", "box-4"}:
                    raise ValueError(f"invalid demo box: {box}")
                if not isinstance(count, int) or isinstance(count, bool) or count < 0:
                    raise ValueError(f"invalid demo count for {name}/{box}")
                counter[box] = count
            per_cat[cat_ids[name]] = counter
        return per_cat

    def events(self, day: str) -> list[dict[str, object]]:
        selected = date.fromisoformat(day)
        start = datetime.combine(selected, time.min, tzinfo=self.timezone)
        end = start + timedelta(days=1)
        with self.db.connect() as connection:
            rows = connection.execute(
                "SELECT * FROM events WHERE started_at >= ? AND started_at < ? ORDER BY started_at DESC",
                (start.astimezone(timezone.utc).isoformat(), end.astimezone(timezone.utc).isoformat()),
            ).fetchall()
        return [dict(row) for row in rows]

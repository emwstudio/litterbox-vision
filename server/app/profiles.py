from __future__ import annotations

import json
from pathlib import Path

import cv2

from app.db import Database
from app.domain import CatProfile, new_id, utc_now
from app.images import decode_jpeg, encode_jpeg


class CatService:
    def __init__(self, db: Database, data_dir: Path):
        self.db = db
        self.data_dir = data_dir

    def list(self) -> list[CatProfile]:
        with self.db.connect() as connection:
            rows = connection.execute("SELECT * FROM cats ORDER BY created_at, id").fetchall()
        return [self._from_row(row) for row in rows]

    def get(self, cat_id: str) -> CatProfile | None:
        with self.db.connect() as connection:
            row = connection.execute("SELECT * FROM cats WHERE id=?", (cat_id,)).fetchone()
        return self._from_row(row) if row else None

    def create(self, name: str, avatar: bytes, references: list[bytes]) -> CatProfile:
        if not 5 <= len(references) <= 10:
            raise ValueError("each cat requires 5 to 10 reference images")
        cat_id = new_id()
        directory = self.data_dir / "cats" / cat_id
        directory.mkdir(parents=True, exist_ok=False)
        avatar_path = directory / "avatar.jpg"
        avatar_path.write_bytes(self._avatar_jpeg(avatar))
        reference_paths = []
        for index, reference in enumerate(references, start=1):
            path = directory / f"reference-{index}.jpg"
            path.write_bytes(self._validated_jpeg(reference))
            reference_paths.append(str(path.relative_to(self.data_dir)))
        profile = CatProfile(id=cat_id, name=name.strip(), avatar_path=str(avatar_path.relative_to(self.data_dir)), reference_paths=reference_paths)
        with self.db.connect() as connection:
            connection.execute(
                "INSERT INTO cats(id,name,avatar_path,reference_paths_json,created_at) VALUES(?,?,?,?,?)",
                (profile.id, profile.name, profile.avatar_path, json.dumps(profile.reference_paths), profile.created_at.isoformat()),
            )
        return profile

    def add_references(self, cat_id: str, references: list[bytes]) -> CatProfile:
        profile = self.get(cat_id)
        if profile is None:
            raise KeyError("cat not found")
        if len(profile.reference_paths) + len(references) > 10 or not references:
            raise ValueError("a cat can have at most 10 reference images")
        for reference in references:
            index = len(profile.reference_paths) + 1
            path = self.data_dir / "cats" / cat_id / f"reference-{index}.jpg"
            path.write_bytes(self._validated_jpeg(reference))
            profile.reference_paths.append(str(path.relative_to(self.data_dir)))
        self._save(profile)
        return profile

    def reference_images(self) -> tuple[list[dict[str, str]], list[bytes]]:
        labels = []
        images = []
        for cat in self.list():
            if not cat.reference_paths:
                continue
            path = self.data_dir / cat.reference_paths[0]
            if not path.is_file():
                continue
            labels.append({"cat_id": cat.id, "name": cat.name})
            images.append(path.read_bytes())
        return labels, images

    def _save(self, profile: CatProfile) -> None:
        with self.db.connect() as connection:
            connection.execute(
                "UPDATE cats SET name=?,avatar_path=?,reference_paths_json=? WHERE id=?",
                (profile.name, profile.avatar_path, json.dumps(profile.reference_paths), profile.id),
            )

    @staticmethod
    def _validated_jpeg(data: bytes) -> bytes:
        return encode_jpeg(decode_jpeg(data), quality=90)

    @staticmethod
    def _avatar_jpeg(data: bytes) -> bytes:
        image = decode_jpeg(data)
        height, width = image.shape[:2]
        side = min(height, width)
        left = (width - side) // 2
        top = (height - side) // 2
        thumbnail = cv2.resize(image[top : top + side, left : left + side], (64, 64), interpolation=cv2.INTER_AREA)
        return encode_jpeg(thumbnail, quality=90)

    @staticmethod
    def _from_row(row) -> CatProfile:
        stored = json.loads(row["reference_paths_json"])
        references = stored.get("reference_paths", []) if isinstance(stored, dict) else stored
        return CatProfile(
            id=row["id"],
            name=row["name"],
            avatar_path=row["avatar_path"],
            reference_paths=references,
            created_at=utc_now() if not row["created_at"] else row["created_at"],
        )

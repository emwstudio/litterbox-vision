import re
from datetime import datetime, timezone
from enum import Enum
from uuid import uuid4

from pydantic import BaseModel, Field, field_validator, model_validator


def new_id() -> str:
    return str(uuid4())


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


class Rect(BaseModel):
    x: float = Field(ge=0, le=1)
    y: float = Field(ge=0, le=1)
    width: float = Field(gt=0, le=1)
    height: float = Field(gt=0, le=1)

    @model_validator(mode="after")
    def fits_frame(self):
        if self.x + self.width > 1.000001 or self.y + self.height > 1.000001:
            raise ValueError("rectangle must fit inside the frame")
        return self


class BoxKind(str, Enum):
    OPEN = "open"
    COVERED = "covered"


class BoxRegion(BaseModel):
    id: str = Field(default_factory=new_id)
    name: str = Field(min_length=1, max_length=40)
    kind: BoxKind
    color: str
    bounds: Rect
    entry: Rect | None = None
    exit_top: Rect | None = None

    @field_validator("id")
    @classmethod
    def safe_id(cls, value: str) -> str:
        if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value) is None:
            raise ValueError("box id must be a safe slug")
        return value

    @field_validator("color")
    @classmethod
    def valid_color(cls, value: str) -> str:
        if len(value) != 7 or value[0] != "#" or any(c not in "0123456789abcdefABCDEF" for c in value[1:]):
            raise ValueError("color must be #RRGGBB")
        return value.upper()

    @model_validator(mode="after")
    def covered_has_transition_regions(self):
        if self.kind == BoxKind.COVERED and (self.entry is None or self.exit_top is None):
            raise ValueError("covered boxes require entry and exit_top regions")
        return self


class Layout(BaseModel):
    frame_width: int = Field(gt=0)
    frame_height: int = Field(gt=0)
    rotation: int = 0
    boxes: list[BoxRegion] = Field(min_length=4, max_length=4)

    @field_validator("rotation")
    @classmethod
    def valid_rotation(cls, value: int) -> int:
        if value not in (0, 90, 180, 270):
            raise ValueError("rotation must be 0, 90, 180 or 270")
        return value

    @model_validator(mode="after")
    def unique_boxes(self):
        ids = [box.id for box in self.boxes]
        colors = [box.color for box in self.boxes]
        if len(set(ids)) != len(ids):
            raise ValueError("box ids must be unique")
        if len(set(colors)) != len(colors):
            raise ValueError("box colors must be distinct")
        return self


class CatProfile(BaseModel):
    id: str = Field(default_factory=new_id)
    name: str = Field(min_length=1, max_length=40)
    avatar_path: str = ""
    reference_paths: list[str] = Field(default_factory=list, max_length=10)
    created_at: datetime = Field(default_factory=utc_now)


class EventStatus(str, Enum):
    ACTIVE = "active"
    COUNTED = "counted"
    DISCARDED = "discarded"
    ERROR = "error"


class VisitEvent(BaseModel):
    id: str = Field(default_factory=new_id)
    cat_id: str = "unknown"
    box_id: str
    started_at: datetime
    ended_at: datetime | None = None
    confidence: float = Field(default=0, ge=0, le=1)
    inferred: bool = True
    status: EventStatus = EventStatus.ACTIVE
    clip_path: str = ""
    model_error: str = ""


class DailyCatStats(BaseModel):
    cat_id: str
    name: str
    avatar_url: str = ""
    today_count: int = Field(ge=0)
    favorite_box_ids: list[str] = Field(default_factory=list)
    last_event_confidence: float | None = Field(default=None, ge=0, le=1)

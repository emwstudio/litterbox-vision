from dataclasses import dataclass
from datetime import datetime, timezone
from enum import Enum
from typing import Annotated

from pydantic import BaseModel, Field, field_validator

from app.domain import BoxKind, EventStatus, VisitEvent, new_id


class Action(str, Enum):
    ENTER = "enter"
    INSIDE_TOILET = "inside_toilet"
    EXIT_TOP = "exit_top"
    LEAVE = "leave"
    NONE = "none"


Coordinate = Annotated[float, Field(ge=0, le=1)]


class ModelObservation(BaseModel):
    cat_present: bool
    box_id: str | None = None
    action: Action
    confidence: float = Field(default=0.0, ge=0, le=1)
    cat_id: str = "unknown"
    sequence_consistent: bool = True
    event_toilet_observed: bool = False
    subject_id: str | None = None
    cat_center: tuple[Coordinate, Coordinate] | None = None

    @field_validator("box_id", mode="before")
    @classmethod
    def normalize_box_id(cls, value):
        if isinstance(value, bool):
            return None
        if isinstance(value, int):
            return f"box-{value}" if 1 <= value <= 4 else None
        return value


@dataclass(slots=True)
class Transition:
    started: VisitEvent | None = None
    finished: VisitEvent | None = None


@dataclass(slots=True)
class _ActiveVisit:
    event: VisitEvent
    kind: BoxKind
    first_inside_ns: int | None = None
    last_inside_ns: int | None = None
    empty_since_ns: int | None = None


class VisitStateMachine:
    def __init__(
        self,
        box_kinds: dict[str, BoxKind] | None = None,
        min_visit_seconds: int = 8,
        max_visit_seconds: int = 1800,
        empty_grace_seconds: int = 10,
    ):
        self.box_kinds = box_kinds or {}
        self.min_visit_ns = min_visit_seconds * 1_000_000_000
        self.max_visit_ns = max_visit_seconds * 1_000_000_000
        self.empty_grace_ns = empty_grace_seconds * 1_000_000_000
        self._active: dict[str, _ActiveVisit] = {}

    @property
    def active(self) -> dict[str, _ActiveVisit]:
        return self._active

    def apply(self, observation: ModelObservation, observed_at_ns: int) -> Transition:
        active = self._active.get(observation.box_id)
        if active is None:
            if not observation.cat_present:
                return Transition()
            kind = self.box_kinds.get(observation.box_id, BoxKind.COVERED)
            if kind == BoxKind.OPEN:
                if observation.action != Action.INSIDE_TOILET:
                    return Transition()
            elif observation.action != Action.ENTER:
                return Transition()
            event = VisitEvent(
                id=new_id(),
                box_id=observation.box_id,
                started_at=self._time(observed_at_ns),
                confidence=observation.confidence,
            )
            active = _ActiveVisit(event=event, kind=kind)
            if kind == BoxKind.OPEN:
                active.first_inside_ns = observed_at_ns
                active.last_inside_ns = observed_at_ns
            self._active[observation.box_id] = active
            return Transition(started=event)

        event = active.event
        elapsed = observed_at_ns - int(event.started_at.timestamp() * 1_000_000_000)
        if elapsed > self.max_visit_ns:
            return Transition(finished=self._finish(observation.box_id, EventStatus.ERROR, observed_at_ns, "visit exceeded maximum duration"))
        if observation.cat_present:
            active.empty_since_ns = None
        elif active.empty_since_ns is None:
            active.empty_since_ns = observed_at_ns
        if observation.cat_present and (observation.action == Action.INSIDE_TOILET or observation.event_toilet_observed):
            if active.first_inside_ns is None:
                active.first_inside_ns = observed_at_ns
            active.last_inside_ns = observed_at_ns
            event.confidence = observation.confidence
        if active.kind == BoxKind.COVERED and (observation.action == Action.EXIT_TOP or self.empty_expired(active, observed_at_ns)):
            event.cat_id = observation.cat_id if observation.sequence_consistent else "unknown"
            event.confidence = observation.confidence
            status = self._completion_status(
                observation,
                elapsed,
                require_inside=False,
                empty_expired=self.empty_expired(active, observed_at_ns),
            )
            return Transition(finished=self._finish(observation.box_id, status, observed_at_ns))
        if active.kind == BoxKind.OPEN and (observation.action == Action.LEAVE or self.empty_expired(active, observed_at_ns)):
            event.cat_id = observation.cat_id if observation.sequence_consistent else "unknown"
            event.confidence = observation.confidence
            status = self._completion_status(
                observation,
                elapsed,
                require_inside=True,
                first_inside_ns=active.first_inside_ns,
                last_inside_ns=active.last_inside_ns,
            )
            return Transition(finished=self._finish(observation.box_id, status, observed_at_ns))
        if observation.action == Action.LEAVE:
            event.cat_id = observation.cat_id if observation.sequence_consistent else "unknown"
            event.confidence = observation.confidence
            return Transition(finished=self._finish(observation.box_id, EventStatus.DISCARDED, observed_at_ns))
        return Transition()

    def empty_expired(self, active: _ActiveVisit, observed_at_ns: int) -> bool:
        return active.empty_since_ns is not None and observed_at_ns - active.empty_since_ns >= self.empty_grace_ns

    def expire(self, observed_at_ns: int) -> list[VisitEvent]:
        finished = []
        for box_id, active in list(self._active.items()):
            started_ns = int(active.event.started_at.timestamp() * 1_000_000_000)
            if observed_at_ns - started_ns > self.max_visit_ns:
                finished.append(self._finish(box_id, EventStatus.ERROR, observed_at_ns, "visit exceeded maximum duration"))
        return finished

    def fail(self, box_id: str, error: str, observed_at_ns: int) -> VisitEvent | None:
        if box_id not in self._active:
            return None
        return self._finish(box_id, EventStatus.ERROR, observed_at_ns, error)

    def _completion_status(
        self,
        observation: ModelObservation,
        elapsed: int,
        require_inside: bool,
        first_inside_ns: int | None = None,
        last_inside_ns: int | None = None,
        empty_expired: bool = False,
    ) -> EventStatus:
        if elapsed < self.min_visit_ns:
            return EventStatus.DISCARDED
        if require_inside:
            if first_inside_ns is None or last_inside_ns is None or last_inside_ns - first_inside_ns < self.min_visit_ns:
                return EventStatus.DISCARDED
        elif not observation.cat_present and not empty_expired:
            return EventStatus.DISCARDED
        return EventStatus.COUNTED

    def _finish(self, box_id: str, status: EventStatus, observed_at_ns: int, error: str = "") -> VisitEvent:
        active = self._active.pop(box_id)
        active.event.status = status
        active.event.ended_at = self._time(observed_at_ns)
        active.event.model_error = error
        return active.event

    @staticmethod
    def _time(timestamp_ns: int) -> datetime:
        return datetime.fromtimestamp(timestamp_ns / 1_000_000_000, tz=timezone.utc)

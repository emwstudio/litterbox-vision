from math import hypot

from app.domain import BoxRegion, Layout
from app.events import Action, ModelObservation

POSITION_TOLERANCE = 0.02
SUBJECT_DISTANCE = 0.08


def _position_valid(observation: ModelObservation, box: BoxRegion) -> bool:
    if not observation.cat_present or observation.action == Action.LEAVE:
        return True
    if observation.cat_center is None:
        return False
    if observation.action == Action.ENTER and box.entry is not None:
        region = box.entry
    elif observation.action == Action.EXIT_TOP and box.exit_top is not None:
        region = box.exit_top
    else:
        region = box.bounds
    x, y = observation.cat_center
    return (
        region.x - POSITION_TOLERANCE <= x <= region.x + region.width + POSITION_TOLERANCE
        and region.y - POSITION_TOLERANCE <= y <= region.y + region.height + POSITION_TOLERANCE
    )


def _distance_to_box_center(observation: ModelObservation, box: BoxRegion) -> float:
    if observation.cat_center is None:
        return float("inf")
    x, y = observation.cat_center
    return hypot(x - (box.bounds.x + box.bounds.width / 2), y - (box.bounds.y + box.bounds.height / 2))


def _prefer(candidate: ModelObservation, current: ModelObservation, boxes: dict[str, BoxRegion]) -> bool:
    candidate_box = boxes[candidate.box_id]
    current_box = boxes[current.box_id]
    candidate_valid = _position_valid(candidate, candidate_box)
    current_valid = _position_valid(current, current_box)
    if candidate_valid != current_valid:
        return candidate_valid
    if candidate.confidence != current.confidence:
        return candidate.confidence > current.confidence
    candidate_distance = _distance_to_box_center(candidate, candidate_box)
    current_distance = _distance_to_box_center(current, current_box)
    if candidate_distance != current_distance:
        return candidate_distance < current_distance
    return candidate.box_id < current.box_id


def deduplicate_observations(observations: list[ModelObservation], layout: Layout) -> list[ModelObservation]:
    boxes = {box.id: box for box in layout.boxes}
    subjects: list[ModelObservation] = []
    empty: list[ModelObservation] = []
    for observation in observations:
        box = boxes.get(observation.box_id)
        if box is None:
            continue
        if not observation.cat_present:
            empty.append(observation)
            continue
        if observation.cat_center is None or not _position_valid(observation, box):
            continue
        duplicate = None
        if observation.subject_id:
            for index, existing in enumerate(subjects):
                if existing.subject_id != observation.subject_id:
                    continue
                distance = hypot(
                    observation.cat_center[0] - existing.cat_center[0],
                    observation.cat_center[1] - existing.cat_center[1],
                )
                if distance < SUBJECT_DISTANCE:
                    duplicate = index
                    break
        if duplicate is None:
            subjects.append(observation)
        elif _prefer(observation, subjects[duplicate], boxes):
            subjects[duplicate] = observation

    per_box: dict[str, ModelObservation] = {}
    for observation in subjects + empty:
        current = per_box.get(observation.box_id)
        if current is None or _prefer(observation, current, boxes):
            per_box[observation.box_id] = observation
    order = {box.id: index for index, box in enumerate(layout.boxes)}
    return sorted(per_box.values(), key=lambda observation: order[observation.box_id])

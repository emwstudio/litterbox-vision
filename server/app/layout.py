from pathlib import Path
from threading import Lock

from app.domain import BoxKind, BoxRegion, Layout, Rect


class LayoutStore:
    def __init__(self, path: Path):
        self.path = path
        self._lock = Lock()

    def load(self) -> Layout:
        with self._lock:
            if not self.path.exists():
                return default_layout()
            return Layout.model_validate_json(self.path.read_text(encoding="utf-8"))

    def save(self, layout: Layout) -> None:
        layout = Layout.model_validate(layout.model_dump())
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.path.with_suffix(".tmp")
            temporary.write_text(layout.model_dump_json(indent=2), encoding="utf-8")
            temporary.replace(self.path)

    def sync_frame_dimensions(self, width: int, height: int) -> Layout:
        layout = self.load()
        rotated = layout.rotation in (90, 270)
        expected_width, expected_height = (height, width) if rotated else (width, height)
        if (layout.frame_width, layout.frame_height) != (expected_width, expected_height):
            layout.frame_width = expected_width
            layout.frame_height = expected_height
            self.save(layout)
        return layout


def default_layout() -> Layout:
    colors = ("#FF6B6B", "#4ECDC4", "#FFD166", "#A78BFA")
    boxes = []
    for index in range(4):
        covered = index < 2
        boxes.append(
            BoxRegion(
                id=f"box-{index + 1}",
                name=f"猫砂盆 {index + 1}",
                kind=BoxKind.COVERED if covered else BoxKind.OPEN,
                color=colors[index],
                bounds=Rect(x=0.03 + index * 0.24, y=0.45, width=0.21, height=0.45),
                entry=Rect(x=0.03 + index * 0.24, y=0.75, width=0.10, height=0.15) if covered else None,
                exit_top=Rect(x=0.08 + index * 0.24, y=0.45, width=0.10, height=0.15) if covered else None,
            )
        )
    return Layout(frame_width=800, frame_height=600, boxes=boxes)

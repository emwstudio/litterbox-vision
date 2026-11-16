import cv2
import numpy as np

from app.domain import Layout
from app.images import decode_jpeg, rect_pixels, rotate_image


class MotionDetector:
    def __init__(self, threshold: float = 8, cooldown_ns: int = 1_000_000_000):
        self.threshold = threshold
        self.cooldown_ns = cooldown_ns
        self._previous: np.ndarray | None = None
        self._last_frame_ns = 0
        self._last_trigger: dict[str, int] = {}

    def detect(self, jpeg: bytes, layout: Layout, timestamp_ns: int) -> list[str]:
        if timestamp_ns <= self._last_frame_ns:
            return []
        self._last_frame_ns = timestamp_ns
        image = rotate_image(decode_jpeg(jpeg), layout.rotation)
        gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
        gray = cv2.resize(gray, (320, 320), interpolation=cv2.INTER_AREA)
        if self._previous is None:
            self._previous = gray
            return []
        difference = cv2.absdiff(self._previous, gray)
        self._previous = gray
        candidates = []
        for box in layout.boxes:
            regions = [box.bounds] + [rect for rect in (box.entry, box.exit_top) if rect is not None]
            scores = []
            for rect in regions:
                left, top, right, bottom = rect_pixels(rect, 320, 320)
                crop = difference[top:bottom, left:right]
                if crop.size:
                    scores.append(float(np.mean(crop)))
            if scores and max(scores) >= self.threshold and timestamp_ns - self._last_trigger.get(box.id, 0) >= self.cooldown_ns:
                self._last_trigger[box.id] = timestamp_ns
                candidates.append(box.id)
        return candidates

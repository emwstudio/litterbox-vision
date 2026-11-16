import asyncio
import logging
from datetime import datetime, timezone
from time import time_ns

from app.clips import ClipError, ClipWriter
from app.db import Database
from app.domain import BoxKind, EventStatus, Layout, VisitEvent, new_id
from app.events import Action, ModelObservation, Transition, VisitStateMachine
from app.images import encode_jpeg, overlay_layout, decode_jpeg, rotate_image
from app.layout import LayoutStore
from app.modelbest import InferenceError, ModelBestClient
from app.motion import MotionDetector
from app.multicat import deduplicate_observations
from app.profiles import CatService
from app.recorder import EventRecorder
from app.ring_buffer import FrameRingBuffer

logger = logging.getLogger(__name__)


class EventEngine:
    def __init__(
        self,
        ring: FrameRingBuffer,
        layout: LayoutStore,
        db: Database,
        models: ModelBestClient,
        clips: ClipWriter,
        cats: CatService,
        pre_seconds: int = 15,
        post_seconds: int = 15,
        recorder: EventRecorder | None = None,
        sample_seconds: int = 5,
        audio_ring=None,
    ):
        self.ring = ring
        self.layout = layout
        self.db = db
        self.models = models
        self.clips = clips
        self.cats = cats
        self.pre_seconds = pre_seconds
        self.post_seconds = post_seconds
        self.recorder = recorder
        self.sample_ns = sample_seconds * 1_000_000_000
        self.audio_ring = audio_ring
        current = layout.load()
        self.machine = VisitStateMachine({box.id: box.kind for box in current.boxes})
        self.motion = MotionDetector()
        self._tasks: set[asyncio.Task] = set()
        self._main_task: asyncio.Task | None = None
        self._running = False
        self._frame_inflight = False
        self._identity_frames: dict[str, list[bytes]] = {}
        self._last_sample: dict[str, int] = {}
        with self.db.connect() as connection:
            connection.execute(
                "UPDATE events SET status='error',ended_at=?,model_error=? WHERE status='active'",
                (datetime.now(timezone.utc).isoformat(), "hub restarted while visit was active"),
            )

    @property
    def running(self) -> bool:
        return self._running and self._main_task is not None and not self._main_task.done()

    def start(self) -> None:
        if self.running:
            return
        self._running = True
        self._main_task = self._spawn(self._run())

    async def stop(self) -> None:
        self._running = False
        tasks = list(self._tasks)
        for task in tasks:
            task.cancel()
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        if self.recorder is not None:
            self.recorder.close()

    def apply(self, observation: ModelObservation, observed_at_ns: int) -> Transition:
        transition = self.machine.apply(observation, observed_at_ns)
        self._persist(transition)
        return transition

    def sampling_due(self, box_id: str, observed_at_ns: int) -> bool:
        return box_id in self.machine.active and observed_at_ns - self._last_sample.get(box_id, 0) >= self.sample_ns

    async def analyze_frame(self, observed_at_ns: int) -> list[Transition]:
        latest = self.ring.latest()
        if latest is None:
            return []
        layout = self.layout.load()
        self.machine.box_kinds = {box.id: box.kind for box in layout.boxes}
        image = self._annotated(latest.jpeg, layout)
        try:
            observations = await self.models.infer_many(
                [image],
                self._frame_prompt(layout),
                tier="accurate",
                event_id=f"frame-{observed_at_ns}",
            )
        except InferenceError as exc:
            logger.warning("Frame inference failed; retaining active visits: %s", exc)
            return []
        observations = deduplicate_observations(observations, layout)
        boxes = {box.id: box for box in layout.boxes}
        transitions = []
        for observation in observations:
            transition = await self._apply_observation(observation, boxes[observation.box_id], latest, observed_at_ns)
            transitions.append(transition)
        return transitions

    async def _apply_observation(self, observation: ModelObservation, box, latest, observed_at_ns: int) -> Transition:
        box_id = box.id
        active = self.machine.active.get(box_id)
        tracking_id = active.event.id if active else new_id()
        if observation.cat_present and observation.action in (Action.ENTER, Action.INSIDE_TOILET, Action.EXIT_TOP):
            identity_frames = self._identity_frames.setdefault(box_id, [])
            if len(identity_frames) < 3:
                identity_frames.append(latest.jpeg)
        review_identity = False
        if active is not None and box.kind == BoxKind.COVERED:
            empty_finished = (
                not observation.cat_present
                and active.empty_since_ns is not None
                and observed_at_ns - active.empty_since_ns >= self.machine.empty_grace_ns
            )
            review_identity = observation.action == Action.EXIT_TOP or empty_finished
        elif active is not None:
            sustained_inside = (
                active.first_inside_ns is not None
                and active.last_inside_ns is not None
                and active.last_inside_ns - active.first_inside_ns >= self.machine.min_visit_ns
            )
            empty_finished = (
                not observation.cat_present
                and active.empty_since_ns is not None
                and observed_at_ns - active.empty_since_ns >= self.machine.empty_grace_ns
            )
            review_identity = sustained_inside and (observation.action == Action.LEAVE or empty_finished)
        if review_identity:
            try:
                observation = await self._accurate(observation, tracking_id, self._identity_frames.get(box_id, []))
            except InferenceError as exc:
                logger.warning("Identity review failed for %s; using unknown identity: %s", tracking_id, exc)
                observation.cat_id = "unknown"
                observation.sequence_consistent = False
        known_cat_ids = {cat.id for cat in self.cats.list()}
        if observation.cat_id not in known_cat_ids:
            observation.cat_id = "unknown"
        transition = self.machine.apply(observation, observed_at_ns)
        if transition.started is not None:
            transition.started.id = tracking_id
        self._persist(transition)
        return transition

    def _persist(self, transition: Transition) -> None:
        if transition.started is not None:
            event = transition.started
            with self.db.connect() as connection:
                connection.execute(
                    "INSERT INTO events(id,cat_id,box_id,started_at,confidence,inferred,status) VALUES(?,?,?,?,?,?,?)",
                    (event.id, event.cat_id, event.box_id, event.started_at.isoformat(), event.confidence, event.inferred, event.status.value),
                )
            self._last_sample[event.box_id] = int(event.started_at.timestamp() * 1_000_000_000)
            if self.recorder is not None:
                start_ns = int(event.started_at.timestamp() * 1_000_000_000)
                recording_started_ns = time_ns()
                pre_frames = self.ring.since(start_ns - self.pre_seconds * 1_000_000_000, recording_started_ns)
                pre_audio = self.audio_ring.since(pre_frames[0].captured_at_ns, recording_started_ns) if self.audio_ring is not None and pre_frames else []
                self.recorder.start(event.id, pre_frames, pre_audio)
        if transition.finished is not None:
            event = transition.finished
            self._identity_frames.pop(event.box_id, None)
            with self.db.connect() as connection:
                connection.execute(
                    "UPDATE events SET cat_id=?,ended_at=?,confidence=?,status=?,model_error=? WHERE id=?",
                    (event.cat_id, event.ended_at.isoformat(), event.confidence, event.status.value, event.model_error, event.id),
                )
            self._last_sample.pop(event.box_id, None)
            self._spawn(self._finish_clip(event))

    async def _accurate(self, fast: ModelObservation, tracking_id: str, frames: list[bytes]) -> ModelObservation:
        labels, references = self.cats.reference_images()
        selected = frames[:3]
        prompt = self._accurate_prompt(labels, len(selected))
        review = await self.models.infer(references + selected, prompt, tier="accurate", event_id=tracking_id)
        return fast.model_copy(
            update={
                "cat_id": review.cat_id,
                "confidence": review.confidence,
                "sequence_consistent": review.sequence_consistent,
            }
        )

    async def _finish_clip(self, event: VisitEvent) -> None:
        end_ns = int(event.ended_at.timestamp() * 1_000_000_000) + self.post_seconds * 1_000_000_000
        delay = max(0, (end_ns - time_ns()) / 1_000_000_000)
        try:
            await asyncio.sleep(delay)
            if self.recorder is not None:
                recorded = await asyncio.to_thread(self.recorder.finish, event.id)
                if recorded.audio is None:
                    logger.warning("Event %s has no microphone audio; writing a video-only clip", event.id)
                video, cover = await asyncio.to_thread(self.clips.write_frames, event.id, recorded.frames, recorded.audio)
            else:
                start_ns = int(event.started_at.timestamp() * 1_000_000_000) - self.pre_seconds * 1_000_000_000
                video, cover = await asyncio.to_thread(self.clips.write, event.id, start_ns, end_ns)
            with self.db.connect() as connection:
                connection.execute("UPDATE events SET clip_path=?,cover_path=? WHERE id=?", (video, cover, event.id))
        except (ClipError, OSError, ValueError) as exc:
            logger.error("Clip failed for %s: %s", event.id, exc)
            with self.db.connect() as connection:
                connection.execute("UPDATE events SET status='error',model_error=? WHERE id=?", (str(exc), event.id))
        finally:
            if self.recorder is not None:
                self.recorder.discard(event.id)

    def _spawn(self, coroutine) -> asyncio.Task:
        task = asyncio.create_task(coroutine)
        self._tasks.add(task)
        task.add_done_callback(self._task_done)
        return task

    def _task_done(self, task: asyncio.Task) -> None:
        self._tasks.discard(task)
        if not task.cancelled():
            error = task.exception()
            if error is not None:
                logger.error("Background task failed: %s", error, exc_info=(type(error), error, error.__traceback__))

    async def _run(self) -> None:
        while self._running:
            try:
                latest = self.ring.latest()
                if latest is not None:
                    layout = self.layout.load()
                    candidates = set(self.motion.detect(latest.jpeg, layout, latest.captured_at_ns))
                    for box_id in self.machine.active:
                        if self.sampling_due(box_id, latest.captured_at_ns):
                            candidates.add(box_id)
                    if candidates and not self._frame_inflight:
                        self._frame_inflight = True
                        for box_id in candidates:
                            self._last_sample[box_id] = latest.captured_at_ns
                        self._spawn(self._analyze_frame_and_release(latest.captured_at_ns))
                for event in self.machine.expire(time_ns()):
                    self._persist(Transition(finished=event))
            except Exception as exc:
                logger.error("Event engine iteration failed: %s", exc, exc_info=True)
            await asyncio.sleep(0.5)

    async def _analyze_frame_and_release(self, observed_at_ns: int) -> None:
        try:
            await self.analyze_frame(observed_at_ns)
        finally:
            self._frame_inflight = False

    @staticmethod
    def _annotated(jpeg: bytes, layout: Layout) -> bytes:
        return encode_jpeg(overlay_layout(rotate_image(decode_jpeg(jpeg), layout.rotation), layout), quality=90)

    @staticmethod
    def _frame_prompt(layout: Layout) -> str:
        regions = "；".join(f"{box.id}={box.name}/{box.kind.value}" for box in layout.boxes)
        return (
            "观察整张猫房画面，先数实际有几只物理猫，再把每只猫分配到唯一一个猫砂盆。"
            f"盆及类型：{regions}。"
            "每只猫使用唯一 subject_id（cat-a、cat-b……），同一物理猫不得重复出现在多个盆，不同猫必须使用不同 subject_id。"
            "cat_center 是猫实际中心相对整张图的归一化 [x,y]，不是盆中心；box_id 是猫当前所在盆。"
            "enter=身体已经进入盆内，inside_toilet=猫当前位于开放盆内，静止也算，不需要蹲姿或排泄证据，"
            "exit_top=从带盖盆顶部离开，leave=猫已经离开开放盆或入口且允许中心在盆外，none=无明确动作。"
            "observations 只包含实际看到的猫，不得为凑满四个盆而编造猫；没有猫的盆只写入 empty_box_ids，不要生成有猫观察。"
            "顶层必须同时有 observations 数组和 empty_box_ids 数组。observations 可以为空，empty_box_ids 必须包含所有确认无猫的盆。"
            "空画面时 observations=[]，四个盆都列入 empty_box_ids，不得因为提示或前一张图而延续有猫。"
            "有猫项字段为 subject_id,cat_center,cat_present,box_id,action,confidence；empty_box_ids 只包含 box-1 至 box-4 的字符串。"
            "无法确定盆时 box_id=null，不确定是否为空也不要列入 empty_box_ids，不得编造猫、位置或动作。"
            "只返回一个 JSON 对象，不要提供或照抄示例值，所有值必须来自当前画面的真实观察。"
        )

    @staticmethod
    def _accurate_prompt(labels: list[dict[str, str]], event_image_count: int) -> str:
        return (
            f"前 {len(labels)} 张是候选猫参考图，顺序为 {labels}；后 {event_image_count} 张是事件中有猫出现的关键帧。"
            "只复核猫身份，不判断或修改事件动作。逐只比较毛色、脸型和花纹；关键帧无猫、看不清、证据不足或前后不是同一只时，"
            "必须 cat_id=unknown、sequence_consistent=false，禁止默认选择第一个候选猫或按图片顺序猜测。"
            "event_toilet_observed 是兼容字段，表示关键帧中是否观察到猫在盆内，不要求如厕姿势或排泄证据。"
            "只返回 JSON，字段为 cat_present,box_id,action,confidence,cat_id,sequence_consistent,event_toilet_observed；action 固定为 none。"
        )

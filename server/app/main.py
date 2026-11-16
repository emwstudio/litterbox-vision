import asyncio
from contextlib import asynccontextmanager
from datetime import datetime
from ipaddress import ip_address
from pathlib import Path
from time import time_ns
from urllib.parse import urlparse
from zoneinfo import ZoneInfo

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from fastapi.responses import Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

from app.api_admin import require_local_action, router as admin_router
from app.api_devices import require_device_token, router as device_router
from app.api_ota import router as ota_router
from app.audio_recording import AudioSegmentWriter
from app.audio_stream import AudioRingBuffer, UdpAudioIngestor
from app.clips import ClipWriter
from app.config import get_settings
from app.db import Database
from app.demo_video import DemoVideoIngestor
from app.domain import Layout, new_id
from app.event_engine import EventEngine
from app.images import TARGET_SIZES, DeltaFrameEncoder, decode_jpeg, display_frame, display_frame_rgb565, display_frame_zlib, write_badge
from app.layout import LayoutStore
from app.live_stream import live_packets
from app.loop_diagnostics import monitor_event_loop
from app.modelbest import ModelBestClient
from app.profiles import CatService
from app.raw_stream import RawLiveStreamServer
from app.recorder import EventRecorder
from app.ring_buffer import FrameRingBuffer, VideoFrame
from app.stats import StatsService
from app.stream import StreamIngestor
from app.udp_live import UdpLivePush
from app.udp_stream import UdpFrameIngestor


@asynccontextmanager
async def lifespan(app: FastAPI):
    settings = get_settings()
    settings.data_dir.mkdir(parents=True, exist_ok=True)
    app.state.settings = settings
    app.state.ring = FrameRingBuffer()
    app.state.delta_encoder = DeltaFrameEncoder()
    app.state.layout = LayoutStore(settings.data_dir / "layout.json")
    app.state.db = Database(settings.data_dir / "catvision.db")
    app.state.db.initialize()
    app.state.cats = CatService(app.state.db, settings.data_dir)
    app.state.stats = StatsService(
        app.state.db,
        ZoneInfo(settings.catvision_timezone),
        demo_path=settings.demo_state_path if settings.app_mode == "demo" else None,
    )
    app.state.ingestor = None
    app.state.live_push = None
    app.state.demo_ingestor = None
    app.state.demo_live_push = None
    app.state.demo_ring = None
    app.state.audio_ring = AudioRingBuffer()
    app.state.audio_ingestor = None
    event_recorder = EventRecorder(settings.data_dir / "spool", diagnostic_dir=settings.frame_diagnostic_dir)
    app.state.ring.on_frame = event_recorder.append
    device_token = settings.device_token.get_secret_value() if settings.device_token else ""
    app.state.raw_stream = RawLiveStreamServer(app.state.ring, app.state.layout, device_token)
    await app.state.raw_stream.start()
    if settings.camera_configured:
        source_host = urlparse(settings.camera_stream_url).hostname
        try:
            source_ip = str(ip_address(source_host))
        except ValueError:
            source_ip = None
        fallback = StreamIngestor(settings.camera_stream_url, app.state.ring, token=device_token)
        app.state.ingestor = UdpFrameIngestor(
            app.state.ring,
            host=settings.camera_udp_host,
            port=settings.camera_udp_port,
            fallback=fallback,
            source_ip=source_ip,
            timeout=settings.camera_udp_timeout,
        )
        await app.state.ingestor.start()
        if settings.camera_audio_enabled:
            app.state.audio_ingestor = UdpAudioIngestor(
                app.state.audio_ring,
                recorder=event_recorder,
                host=settings.camera_udp_host,
                port=settings.camera_audio_port,
                source_ip=source_ip,
            )
            try:
                await app.state.audio_ingestor.start()
            except OSError as exc:
                app.state.audio_ingestor.last_error = str(exc)
        live_targets = {
            "monitor": (settings.monitor_udp_host, settings.display_udp_port),
            "stats_top": (settings.stats_udp_host, settings.display_udp_port),
        }
        if settings.app_mode == "demo" and settings.demo_video_path is not None:
            app.state.demo_ring = FrameRingBuffer()
            app.state.demo_ingestor = DemoVideoIngestor(settings.demo_video_path, app.state.demo_ring)
            app.state.demo_ingestor.start()
            live_targets.pop("stats_top")
            app.state.demo_live_push = UdpLivePush(
                app.state.demo_ring,
                app.state.layout,
                targets={"stats_top": (settings.stats_udp_host, settings.display_udp_port)},
                fps=app.state.demo_ingestor.fps,
            )
            await app.state.demo_live_push.start()
        app.state.live_push = UdpLivePush(
            app.state.ring,
            app.state.layout,
            targets=live_targets,
            fps=settings.display_udp_fps,
        )
        await app.state.live_push.start()
    models = ModelBestClient(
        api_key=settings.modelbest_api_key,
        fast_model=settings.modelbest_fast_model,
        accurate_model=settings.modelbest_accurate_model,
        base_url=settings.modelbest_base_url,
        db=app.state.db,
    )
    app.state.clips = ClipWriter(
        app.state.ring,
        settings.data_dir,
        fps=settings.camera_fps,
        timezone_info=app.state.stats.timezone,
    )
    app.state.engine = EventEngine(
        ring=app.state.ring,
        layout=app.state.layout,
        db=app.state.db,
        models=models,
        clips=app.state.clips,
        cats=app.state.cats,
        pre_seconds=settings.clip_pre_seconds,
        post_seconds=settings.clip_post_seconds,
        recorder=event_recorder,
        audio_ring=app.state.audio_ring,
    )
    if settings.camera_configured and settings.models_configured and settings.app_mode != "demo":
        app.state.engine.start()
    app.state.loop_monitor = None
    if settings.frame_diagnostic_dir is not None:
        app.state.loop_monitor = asyncio.create_task(monitor_event_loop(settings.frame_diagnostic_dir))
    yield
    await app.state.engine.stop()
    if app.state.loop_monitor is not None:
        app.state.loop_monitor.cancel()
        await asyncio.gather(app.state.loop_monitor, return_exceptions=True)
    if app.state.audio_ingestor is not None:
        await app.state.audio_ingestor.stop()
    await app.state.raw_stream.stop()
    if app.state.demo_live_push is not None:
        await app.state.demo_live_push.stop()
    if app.state.live_push is not None:
        await app.state.live_push.stop()
    if app.state.demo_ingestor is not None:
        await app.state.demo_ingestor.stop()
    if app.state.ingestor is not None:
        await app.state.ingestor.stop()


app = FastAPI(title="CatVision local hub", version="0.1.0", lifespan=lifespan)


@app.middleware("http")
async def security_headers(request: Request, call_next):
    response = await call_next(request)
    response.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; media-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'"
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Referrer-Policy"] = "strict-origin-when-cross-origin"
    response.headers["Permissions-Policy"] = "camera=(), geolocation=(), microphone=()"
    return response


def selected_day(day: str | None, request: Request) -> str:
    if day is not None:
        return day
    return datetime.now(request.app.state.stats.timezone).date().isoformat()


def current_frame(request: Request) -> VideoFrame:
    latest = request.app.state.ring.latest()
    if latest is None:
        raise HTTPException(status_code=503, detail="no camera frame available")
    age = (time_ns() - latest.captured_at_ns) / 1_000_000_000
    if age > 3:
        raise HTTPException(status_code=503, detail="camera frame is stale")
    return latest


def sync_layout_from_frame(request: Request, frame: VideoFrame) -> None:
    try:
        image = decode_jpeg(frame.jpeg)
    except ValueError as exc:
        raise HTTPException(status_code=503, detail="invalid camera frame") from exc
    request.app.state.layout.sync_frame_dimensions(image.shape[1], image.shape[0])


def frame_response(frame: VideoFrame, content: bytes, media_type: str = "image/jpeg") -> Response:
    age = max(0, (time_ns() - frame.captured_at_ns) / 1_000_000_000)
    return Response(
        content=content,
        media_type=media_type,
        headers={
            "Cache-Control": "no-store",
            "X-Captured-At-Ns": str(frame.captured_at_ns),
            "X-Frame-Age-Seconds": f"{age:.3f}",
        },
    )


@app.get("/api/health")
def health(request: Request) -> dict[str, object]:
    settings = request.app.state.settings
    ingestor = request.app.state.ingestor
    audio_ingestor = request.app.state.audio_ingestor
    demo_ingestor = request.app.state.demo_ingestor
    return {
        "status": "ok",
        "app_mode": settings.app_mode,
        "camera_configured": settings.camera_configured,
        "camera_online": ingestor.online if ingestor else False,
        "frame_age_seconds": ingestor.frame_age_seconds if ingestor else None,
        "camera_error": ingestor.last_error if ingestor else "camera is not configured",
        "demo_video": None if demo_ingestor is None else {
            "online": demo_ingestor.online,
            "frame_age_seconds": demo_ingestor.frame_age_seconds,
            "error": demo_ingestor.last_error,
            "fps": demo_ingestor.fps,
            "loops": demo_ingestor.loops,
            "source_fps": demo_ingestor.source_fps,
            "source_duration_seconds": demo_ingestor.source_duration_seconds,
        },
        "udp_ingest": ingestor.stats() if ingestor and hasattr(ingestor, "stats") else None,
        "audio_online": audio_ingestor.online if audio_ingestor else False,
        "audio_ingest": audio_ingestor.stats() if audio_ingestor else None,
        "audio_error": audio_ingestor.last_error if audio_ingestor else "audio is not configured",
        "models_configured": settings.models_configured,
        "event_engine_running": request.app.state.engine.running,
        "fast_model": settings.modelbest_fast_model,
        "accurate_model": settings.modelbest_accurate_model,
    }


@app.post("/api/diagnostics/av-check", dependencies=[Depends(require_local_action)])
async def diagnostic_av_check(request: Request, seconds: float = Query(default=10, ge=2, le=30)) -> dict[str, object]:
    audio_ingestor = request.app.state.audio_ingestor
    if audio_ingestor is None or not audio_ingestor.online:
        raise HTTPException(status_code=503, detail="microphone audio is unavailable")
    start_ns = time_ns()
    await asyncio.sleep(seconds)
    end_ns = time_ns()
    frames = request.app.state.ring.since(start_ns, end_ns)
    if not frames:
        raise HTTPException(status_code=503, detail="no camera frames available for audio/video check")
    chunks = request.app.state.audio_ring.since(start_ns, end_ns)
    if not chunks:
        raise HTTPException(status_code=503, detail="no microphone audio available for audio/video check")
    check_id = f"av-check-{new_id()}"
    audio_path = request.app.state.settings.data_dir / "spool" / f"{check_id}.pcm"
    writer = AudioSegmentWriter(audio_path)
    try:
        for chunk in chunks:
            writer.append(chunk)
        audio = writer.finish()
        if audio is None:
            raise HTTPException(status_code=503, detail="empty microphone audio for audio/video check")
        video, cover = await asyncio.to_thread(request.app.state.clips.write_frames, check_id, frames, audio)
    finally:
        audio_path.unlink(missing_ok=True)
    return {
        "video": f"/api/media/videos/{video}",
        "cover": f"/api/media/videos/{cover}",
        "audio_missing_samples": audio.missing_samples,
        "audio_sample_rate": audio.sample_rate,
        "audio_channels": audio.channels,
        "database_event_created": False,
    }


@app.get("/api/layout", dependencies=[Depends(require_device_token)])
def get_layout(request: Request) -> Layout:
    latest = request.app.state.ring.latest()
    if latest is not None:
        sync_layout_from_frame(request, latest)
    return request.app.state.layout.load()


@app.put("/api/layout", dependencies=[Depends(require_device_token)])
def put_layout(layout: Layout, request: Request) -> Layout:
    latest = current_frame(request)
    image = decode_jpeg(latest.jpeg)
    expected_width, expected_height = (image.shape[1], image.shape[0])
    if layout.rotation in (90, 270):
        expected_width, expected_height = expected_height, expected_width
    if (layout.frame_width, layout.frame_height) != (expected_width, expected_height):
        raise HTTPException(status_code=422, detail=f"layout dimensions must match the rotated frame: {expected_width}x{expected_height}")
    badges_root = request.app.state.settings.data_dir.resolve() / "badges"
    try:
        for box in layout.boxes:
            destination = (badges_root / f"{box.id}.png").resolve()
            destination.relative_to(badges_root)
            write_badge(latest.jpeg, layout, box, destination)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    request.app.state.layout.save(layout)
    with request.app.state.db.connect() as connection:
        connection.execute("DELETE FROM boxes")
        for box in layout.boxes:
            connection.execute(
                "INSERT INTO boxes(id, name, kind, color, bounds_json, entry_json, exit_top_json) VALUES (?, ?, ?, ?, ?, ?, ?)",
                (
                    box.id,
                    box.name,
                    box.kind.value,
                    box.color,
                    box.bounds.model_dump_json(),
                    box.entry.model_dump_json() if box.entry else None,
                    box.exit_top.model_dump_json() if box.exit_top else None,
                ),
            )
    return layout


@app.get("/api/frame.jpg", dependencies=[Depends(require_device_token)])
def original_frame(request: Request) -> Response:
    latest = current_frame(request)
    return frame_response(latest, latest.jpeg)


@app.get("/api/display/frame.jpg", dependencies=[Depends(require_device_token)])
def device_frame(
    request: Request,
    target: str = "monitor",
    overlay: bool = Query(default=True),
    format: str = "jpeg",
) -> Response:
    latest = current_frame(request)
    sync_layout_from_frame(request, latest)
    try:
        if format == "rgb565":
            content = display_frame_rgb565(latest.jpeg, request.app.state.layout.load(), target, overlay)
            media_type = "application/octet-stream"
        elif format == "zlib":
            content = display_frame_zlib(latest.jpeg, request.app.state.layout.load(), target, overlay)
            media_type = "application/octet-stream"
        elif format in ("zlib_delta", "zlib_delta_be"):
            try:
                base_crc = int(request.headers.get("x-catvision-frame-crc", "0"))
            except ValueError:
                base_crc = 0
            if not 0 <= base_crc <= 0xffffffff:
                base_crc = 0
            content, frame_crc, is_delta = request.app.state.delta_encoder.encode(
                latest.jpeg, request.app.state.layout.load(), target, overlay, base_crc,
                big_endian=format == "zlib_delta_be"
            )
            media_type = "application/octet-stream"
        elif format == "jpeg":
            content = display_frame(latest.jpeg, request.app.state.layout.load(), target, overlay)
            media_type = "image/jpeg"
        else:
            raise ValueError("unsupported display format")
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    response = frame_response(latest, content, media_type)
    if format in ("zlib_delta", "zlib_delta_be"):
        response.headers["X-CatVision-Delta"] = "1" if is_delta else "0"
        response.headers["X-CatVision-Frame-CRC"] = str(frame_crc)
    return response


@app.get("/api/display/stream.bin", dependencies=[Depends(require_device_token)])
async def device_live_stream(request: Request, target: str = "monitor") -> StreamingResponse:
    if target not in TARGET_SIZES:
        raise HTTPException(status_code=400, detail="unknown display target")
    latest = current_frame(request)
    sync_layout_from_frame(request, latest)
    packets = live_packets(
        request.app.state.ring,
        request.app.state.layout.load(),
        target,
    )
    return StreamingResponse(packets, media_type="application/octet-stream", headers={"Cache-Control": "no-store"})


@app.get("/api/stats/daily")
def daily_stats(request: Request, date: str | None = None) -> dict[str, object]:
    try:
        return request.app.state.stats.daily(selected_day(date, request))
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="date must be YYYY-MM-DD") from exc


@app.get("/api/events")
def daily_events(request: Request, date: str | None = None) -> list[dict[str, object]]:
    try:
        return request.app.state.stats.events(selected_day(date, request))
    except ValueError as exc:
        raise HTTPException(status_code=400, detail="date must be YYYY-MM-DD") from exc


app.include_router(admin_router)
app.include_router(device_router)
app.include_router(ota_router)
app.mount("/", StaticFiles(directory=Path(__file__).with_name("static"), html=True), name="console")

from functools import lru_cache
from pathlib import Path

from pydantic import SecretStr, field_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    app_mode: str = "cat"
    modelbest_api_key: SecretStr | None = None
    modelbest_fast_model: str = "MiniCPM-V-4.7-1B"
    modelbest_accurate_model: str = "MiniCPM-V-4.7-35B-A3B"
    modelbest_base_url: str = "https://api.modelbest.cn/v1"
    camera_stream_url: str = ""
    camera_udp_host: str = "0.0.0.0"
    camera_udp_port: int = 5060
    camera_udp_timeout: float = 5.0
    camera_audio_enabled: bool = True
    camera_audio_port: int = 5061
    monitor_udp_host: str = "catvision-monitor.local"
    stats_udp_host: str = "catvision-stats.local"
    display_udp_port: int = 5060
    display_udp_fps: int = 12
    device_token: SecretStr | None = None
    data_dir: Path = Path("data")
    demo_state_path: Path = Path("data/demo_state.json")
    demo_video_path: Path | None = None
    frame_diagnostic_dir: Path | None = None
    catvision_timezone: str = "Asia/Shanghai"
    camera_width: int = 800
    camera_height: int = 600
    camera_fps: int = 10
    clip_pre_seconds: int = 15
    clip_post_seconds: int = 15
    min_visit_seconds: int = 8
    max_visit_seconds: int = 1800

    @field_validator("demo_video_path", mode="before")
    @classmethod
    def empty_demo_video_path(cls, value):
        return None if value == "" else value

    @property
    def models_configured(self) -> bool:
        return self.modelbest_api_key is not None

    @property
    def camera_configured(self) -> bool:
        return bool(self.camera_stream_url)


@lru_cache
def get_settings() -> Settings:
    return Settings()

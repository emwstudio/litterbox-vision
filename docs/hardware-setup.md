# 硬件安装与固件烧录

## 设备

- [ESP32-S3-CAM-OV5640](https://docs.waveshare.com/ESP32-S3-CAM-OVxxxx?variant=ESP32-S3-CAM-OV5640)：DVP OV5640、8MB PSRAM、16MB Flash。
- [ESP32-S3-Touch-AMOLED-1.75](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75)：466×466、CO5300 + CST9217。
- [ESP32-S3-Touch-AMOLED-2.16](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16)：480×480、CO5300 + CST9220。

三块设备通过 2.4GHz Wi-Fi 与电脑通信，不需要把屏幕接到摄像头排线上。所有设备使用稳定电源；长期监控不建议只依赖电池。

## 摄像头摆放

1. 固定镜头，确保四个盆同时入镜，两个带盖盆的**入口和顶部出口**都清晰可见。
2. 猫在画面中需要有足够像素区分花色、耳形和脸部特征。OV5640 的视场角有限，若四盆无法同时看清，应先调整位置或镜头，而不是期待模型补救遮挡。
3. 夜间需要稳定、柔和的可见光补光。当前固件没有红外夜视控制。
4. 避免阳光直射、强烈逆光、风扇造成的画面抖动和频繁光照变化。
5. 安装后确认 1.75 寸屏持续显示原始实时画面。摄像头一旦移动，应重新检查四个盆的完整视角。

## 工具链

电脑端需要 Python 3.12、uv、FFmpeg。固件使用 ESP-IDF 5.5.2；两块屏使用 Waveshare 官方 BSP 与 LVGL 9.4。

当前两块 CO5300 QSPI 屏的像素时钟配置为 80MHz，由 `CONFIG_CATVISION_LCD_PCLK_HZ` 控制，启动日志会打印实际值。该配置已实机确认画面正常；修改时钟后仍需重新检查画面、像素校验和帧率。

按 [ESP-IDF 官方安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/) 安装后：

```bash
. $IDF_PATH/export.sh
```

也可使用本项目开发时采用的隔离方式：将 ESP-IDF 和 `IDF_TOOLS_PATH` 都放在项目自己的临时工具目录，避免改动其他项目的工具链。

## 配置

先复制对应项目的 `sdkconfig.defaults.example` 为 `sdkconfig.defaults`。示例只包含构建参数和占位符，不包含真实凭据；本地已有的 `sdkconfig.defaults` 不需要覆盖。随后每个固件项目执行 `idf.py menuconfig`：

- 摄像头：在 `Example Connection Configuration` 设置 Wi-Fi，在 `CatVision Network` 设置与电脑一致的设备令牌。
- 两块屏：在 `CatVision Network` 设置 Wi-Fi、电脑地址（例如 `http://你的Mac主机名.local:8000`）和设备令牌。

当前使用 mDNS 主机名避免 DHCP 改址后失联：摄像头为 `catvision-camera.local`，1.75 屏为 `catvision-monitor.local`，2.16 屏为 `catvision-stats.local`。电脑端 `CAMERA_STREAM_URL` 使用摄像头主机名，`MONITOR_UDP_HOST`、`STATS_UDP_HOST` 分别使用两块屏的主机名。摄像头向电脑的 `.local` 主机名发送音视频；推流端每 30 秒刷新一次解析，电脑或屏幕更换 DHCP 地址后无需再写死新 IP。Mac 主机名可通过 `scutil --get LocalHostName` 查看。2.16 屏的 `sdkconfig.defaults` 将 mDNS 任务和动态分配放入 PSRAM，避免挤占 LVGL 所需的内部 RAM。

`sdkconfig` 含本地凭据，已被 `.gitignore` 排除。不要把真实配置截图或文件提交到公开仓库。

## 构建与烧录

```bash
idf.py -C firmware/camera set-target esp32s3
idf.py -C firmware/camera build flash monitor

idf.py -C firmware/monitor_1p75 set-target esp32s3
idf.py -C firmware/monitor_1p75 build flash monitor

idf.py -C firmware/stats_2p16 set-target esp32s3
idf.py -C firmware/stats_2p16 build flash monitor
```

多块设备同时连接时，**每次烧录都必须显式指定 `-p` 串口**（例如 `-p /dev/cu.usbmodem101`），不能让 esptool 自动选择。先通过启动日志核对项目名和 MAC；当前摄像头为 `catvision_camera`、`90:70:69:fe:5f:44`，1.75 为 `catvision_monitor_1p75`、`44:bd:8d:60:85:90`，2.16 为 `catvision_stats_2p16`、`44:bd:8d:60:cb:6c`。重新插拔后端口编号可能变化，不要只凭旧端口写入。不要为了清障而擦除整片 Flash；正常更新不需要擦除。

两块 AMOLED 屏需要手动进入下载模式时，**按住 BOOT 后重新拔插 USB 上电**，再松开 BOOT，使用 `esptool` 的 `--before no_reset` 烧录。不要把点按 PWR 当作通用复位操作。烧录后若串口停在 `waiting for download`，重新拔插一次 USB，不要按任何键，让新固件正常启动；若仍不退出，断开 USB 和电池（如已连接）约 5 秒，再接回 USB 且不按键，确保真正断电重启。

## 屏幕字体

16px 标签使用 `firmware/common/components/catvision_ui/lv_font_catvision_16.c`，由 Noto Sans CJK SC 生成，许可证保存在同目录的 `FONT-LICENSE.txt`。当前仅包含 ASCII 和“球、驴、啵、宝”四个汉字；新增其他中文名字或提示语时，需要扩展字符范围，并用 `lv_font_conv --lv-include lvgl.h` 重新生成该字体，避免缺字占位符。

## 首次使用

1. 启动电脑服务与摄像头，确认控制台能显示实时画面。
2. 在控制台为四只猫分别上传头像和 5–10 张参考图。
3. 确认 1.75 寸屏显示原始实拍画面；2.16 寸屏上半部为居中填满的 LIVE，下半部为四猫统计与实景徽章。
4. 在电脑控制台查看当日事件并播放录像。

## 实机边界

源码构建、烧录、显示驱动、触摸映射和真实猫场景是不同验收层级。只有实际设备上的结果才能证明这些层级通过。统计表示猫砂盆使用次数，带盖盆尤其只能根据外部行为推测，不等同于实际排泄次数。

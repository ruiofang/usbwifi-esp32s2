# ESP32-S2 USB WiFi Bridge

将 ESP32-S2 的 USB-CDC 口透明桥接到 Wi-Fi 网络，并提供实时调试控制台的嵌入式固件。

## 功能特性

- **USB-CDC ↔ 网络透传** — 串口数据直通 TCP Server / TCP Client / UDP，零驱动、即插即用
- **双模 Wi-Fi** — STA（连接路由器）与 AP（内置热点 `192.168.4.1`）可独立开关，同时运行
- **Web 管理界面** — 纯 HTML5 单页应用，无需安装 App，手机/PC 直接配置
- **实时 WebSocket 调试控制台** — 独立接收编码（UTF-8 / GBK / HEX）与发送编码（UTF-8 / HEX），支持定时发送
- **出厂恢复** — 上电时长按 BOOT 键 5 秒，LED 快闪确认后自动擦除配置并重启
- **中英双语界面** — 一键切换，设置持久化至 `localStorage`

## 硬件要求

| 项目 | 说明 |
|------|------|
| 主控 | ESP32-S2（含原生 USB）|
| Flash | 4 MB QIO 80 MHz |
| LED | GPIO 17（推挽输出）|
| BOOT 键 | GPIO 0（内置上拉，低电平有效）|
| USB | 原生 USB-CDC（无需 CH340 等转换芯片）|

## 快速上手

### 1. 环境准备

- [ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s2/get-started/)

### 2. 编译 & 烧录

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p <PORT> flash
```

### 3. 连接管理界面

- **AP 模式**（默认）：手机/PC 连接热点 `ESP32S2-USB-WIFI`，密码 `12345678`，浏览器访问 `http://192.168.4.1`
- **STA 模式**：在 Web 界面配置 Wi-Fi 后，通过路由器分配的 IP 访问

## Web 界面说明

| 标签页 | 功能 |
|--------|------|
| 运行状态 | 查看 STA 连接信息、IP/网关/掩码，保存当前配置 |
| WiFi 设置 | 配置 SSID / 密码，开关 AP / STA 模式，扫描周边热点 |
| 透传设置 | 选择 TCP Server / TCP Client / UDP，设置远端地址与端口，启停引擎，调整波特率 |
| 调试控制台 | 实时显示串口与网络数据（WebSocket），支持发送数据、定时发送、编码切换、清屏 |

## 透传模式

| 模式 | 说明 |
|------|------|
| TCP Server | 监听本地端口，等待客户端连接后双向透传 |
| TCP Client | 主动连接远端 IP:Port，双向透传 |
| UDP | 收发 UDP 数据包，发送目标为配置的远端地址 |

## 出厂恢复

上电或重启时，**在 LED 亮起前长按 BOOT 键（GPIO0）**：

1. LED 以 100 ms 节奏快速闪烁，倒计时 5 秒
2. 期间松手 → 取消，正常启动
3. 坚持 5 秒 → LED 三次急速闪烁 → 擦除 NVS → 重启

恢复后默认配置：AP 开启（`ESP32S2-USB-WIFI` / `12345678`），STA 关闭。

## 项目结构

```
├── main/
│   ├── main.c            # 入口：初始化、BOOT 键检测
│   ├── app_config.c/h    # NVS 配置读写
│   ├── wifi_manager.c/h  # Wi-Fi STA/AP 管理、扫描
│   ├── passthrough.c/h   # TCP/UDP 透传引擎
│   ├── web_server.c/h    # HTTP + WebSocket 服务器
│   ├── led_ctrl.c/h      # LED 状态指示
│   └── index.html        # Web 管理界面（嵌入固件）
├── partitions.csv        # 4 MB 分区表（支持 OTA）
├── sdkconfig.defaults    # 默认 Kconfig 配置
└── CMakeLists.txt
```

## 分区表

| 分区 | 类型 | 大小 |
|------|------|------|
| nvs | NVS | 16 KB |
| factory | App | 1 MB |
| ota_0 / ota_1 | OTA | 各 1 MB |
| storage | FAT | 960 KB |

## License

MIT License © 2024 RUIO — 详见 [LICENSE](LICENSE) 文件。

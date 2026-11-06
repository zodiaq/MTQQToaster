# MQTT Toaster

A system tray resident application written in C++/WinRT that receives JSON messages from an MQTT broker such as Home Assistant (HA) and displays them as standard Windows toast notifications (with image support).

---

## 1. System Overview & Architecture

* **Language / Runtime**: C++23 (MSVC / C++/WinRT)
* **Execution Mode**: System tray resident application
* **MQTT Worker Thread**: Handles broker communication and event loop processing using `libmosquitto`
* **Configuration Watcher**: Monitors changes to `config.json` using `Windows.Storage.Search.StorageFileQueryResult`.

---

## 2. Functional Specifications

### 2.1 Toast Notification Feature

* **WinRT API**: Uses `Windows.UI.Notifications` (`ToastNotificationManager`).
* **Template Switching**:
* Without image: `ToastText01`
* With image: `ToastImageAndText01`


* **Image Display**: Each subscribed topic can have one default image. An image in the MQTT payload takes precedence; otherwise the matching topic's configured image is used. Selected files are copied beside `config.json`, and unreferenced managed images are removed after a successful save.

### 2.2 MQTT Communication Feature

* **Library**: `mosquitto` (C API)
* **Auto-reconnect**: Automatically attempts to reconnect at 5-second intervals upon disconnection or configuration changes.
* **Authentication**: Supports username and password authentication (optional).

### 2.3 Configuration and Hot Reload Feature

* **Settings UI**: The tray menu opens a native Win32 settings dialog for MQTT connection, topics, credentials, and logging.
* **Configuration File**: The UI persists settings to the per-user application data directory. JSON remains an internal persistence format.
* **Legacy Migration**: On first launch, an existing `config.json` beside the executable is copied to the new location. Otherwise `config.sample.json` is used when available.
* **Dynamic Reload**: Immediately detects modifications and saves to `config.json` at the kernel level.
* Automatically reconnects to MQTT if connection info (Host, Port, Topic, etc.) changes.
* Immediately updates log output upon log level changes without restarting.

### 2.4 Log Output & Rotation Feature

* **Library**: `spdlog` (Header-only / `rotating_file_sink`)
* **Output Destination**: `Logs/app.log` in the per-user cache directory
* **Automatic Rotation**: Rotates files and automatically deletes old logs when the set MB size limit is exceeded (preventing disk space exhaustion).
* **Log Levels**: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `NONE`

### 2.5 System Tray (Notification Area) UI
* **Resident Display**: Displays an icon in the system tray (default is the standard Windows app icon).
* **Right-Click Menu**
	* "Add to Startup" Register/unregister startup
	* "Settings..." Open the settings dialog
	* "Exit" Exit the application

---

## 3. Configuration File Specification (`config.json`)

The application manages this file. For unpackaged builds it is stored under `%LOCALAPPDATA%\MTQQToaster`; MSIX builds use the package-managed `LocalFolder`.

```json
{
  "mqtt_url": "mqtt://10.0.0.1:1883",
  "mqtt_topics": [
    { "topic": "windows/notification", "image": "topic-image-1234-5678.png" }
  ],
  "mqtt_user": "",
  "mqtt_pass": "",
  "log_level": "INFO",
  "log_max_size_mb": 5,
  "log_max_files": 3
}

```

### Parameter Details

| Key | Type | Default | Description |
| --- | --- | --- | --- |
| `mqtt_url` | string | `"mqtt://127.0.0.1:1883"` | URL of the MQTT broker |
| `mqtt_topics` | array | `[{"topic":"windows/notification","image":""}]` | Subscriptions and optional application-managed default images. Wildcards are supported |
| `mqtt_user` | string | `""` | MQTT authentication username (empty string if not needed) |
| `mqtt_pass` | string | `""` | MQTT authentication password (empty string if not needed) |
| `log_level` | string | `"INFO"` | Log output level (`TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `NONE`) |
| `log_max_size_mb` | int | `5` | Maximum size limit per log file (MB) |
| `log_max_files` | int | `3` | Maximum number of log file generations to retain |

---

## 4. MQTT Payload Specification

Specification for the Payload sent via `mqtt.publish` from HA, etc.

### 4.1 JSON Format (Recommended)

```json
{
  "title": "Notification Title",
  "message": "This is the body text of the notification.",
  "image": "snapshot.jpg"
}

```

* `title` (mandatory): Title of the notification (defaults to `"MQTT Toaster"` if omitted).
* `message` (mandatory): Body text of the notification.
* `image` (optional): Image filename or supported application URI. Relative filenames resolve against `ToastAssets` first, then the executable directory for backward compatibility.

### 4.2 Plain Text Format

If mandatory parameters	are not set, it will be processed with the title set to `"MQTT Toaster"` and the message body set to the sent string.

---

## 5. Dependencies & Build Conditions

### Development Environment

* **IDE / Compiler**: Visual Studio 2026 Community with Desktop development with C++ (C++23)
* **Subsystem Setting**: `Windows (/SUBSYSTEM:WINDOWS)`
* Bundled CMake 4.2 or later, Windows SDK and vcpkg (see `.vsconfig`)
* `cmake -B build -S .` automatically selects vcpkg and restores the pinned manifest dependencies
* x64 with static CRT (`/MT` for Release, `/MTd` for Debug); see [README.md](README.md) for build and test commands

### External Libraries

1. C++/WinRT
2. libmosquitto
3. nlohmann/json
4. spdlog

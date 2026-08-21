# MQTT Toaster

A system tray resident application written in C++/WinRT that receives JSON messages from an MQTT broker such as Home Assistant (HA) and displays them as standard Windows toast notifications (with image support).

---

## 1. System Overview & Architecture

* **Language / Runtime**: C++23 (MSVC / C++/WinRT)
* **Execution Mode**: System tray resident application
* **MQTT Worker Thread**: Handles broker communication and event loop processing using `libmosquitto`
* **File Watcher Thread**: Monitors changes to `config.json` (hot reload) using `ReadDirectoryChangesW` (Windows Native API)

---

## 2. Functional Specifications

### 2.1 Toast Notification Feature

* **WinRT API**: Uses `Windows.UI.Notifications` (`ToastNotificationManager`).
* **Template Switching**:
* Without image: `ToastText01`
* With image: `ToastImageAndText01`


* **Image Display**: Placing an image file next to the exe and passing the filename in the payload displays it inline within the toast.

### 2.2 MQTT Communication Feature

* **Library**: `mosquitto` (C API)
* **Auto-reconnect**: Automatically attempts to reconnect at 5-second intervals upon disconnection or configuration changes.
* **Authentication**: Supports username and password authentication (optional).

### 2.3 Configuration and Hot Reload Feature

* **Configuration File**: Loaded from `config.json` placed in the same directory as the exe.
* **Dynamic Reload**: Immediately detects modifications and saves to `config.json` at the kernel level.
* Automatically reconnects to MQTT if connection info (Host, Port, Topic, etc.) changes.
* Immediately updates log output upon log level changes without restarting.

### 2.4 Log Output & Rotation Feature

* **Library**: `spdlog` (Header-only / `rotating_file_sink`)
* **Output Destination**: `app.log` in the same directory as the exe
* **Automatic Rotation**: Rotates files and automatically deletes old logs when the set MB size limit is exceeded (preventing disk space exhaustion).
* **Log Levels**: `TRACE`, `DEBUG`, `INFO`, `WARN`, `ERROR`, `NONE`

### 2.5 System Tray (Notification Area) UI
* **Resident Display**: Displays an icon in the system tray (default is the standard Windows app icon).
* **Right-Click Menu**
	* "Add to Startup" Register/unregister startup
	* "Exit" Exit the application

---

## 3. Configuration File Specification (`config.json`)

Place it in the same directory as the exe.

```json
{
  "mqtt_url": "mqtt://10.0.0.1:1883",
  "mqtt_topics": ["windows/notification"],
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
| `mqtt_topics` | array | `["windows/notification", "home/+/temperature" ]` | Array of MQTT topics to subscribe to. Wildcards are supported |
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
* `image` (optional): Image filename (displays text-only toast if omitted). The image file must be placed in the same directory as the exe.

### 4.2 Plain Text Format

If mandatory parameters	are not set, it will be processed with the title set to `"MQTT Toaster"` and the message body set to the sent string.

---

## 5. Dependencies & Build Conditions

### Development Environment

* **IDE / Compiler**: Visual Studio 2026 (`/std:c++23` or higher)
* **Subsystem Setting**: `Windows (/SUBSYSTEM:WINDOWS)`
* CMake
* vcpkg

### External Libraries

1. C++/WinRT
2. libmosquitto
3. nlohmann/json
4. spdlog
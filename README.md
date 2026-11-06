# MTQQToaster

MTQQToaster is a simple MQTT Windows client that allows you to receive messages using the MQTT protocol.
It is designed to be lightweight and easy to use, making it ideal for the scenarios where you want to receive notifications from Home Assistant or other MQTT brokers.

## Features
- Connect to an MQTT broker
 - TLS/SSL support for secure connections
- Subscribe to topics and receive messages
- Pop up toast notification for incoming messages
- Automatically start on system boot

## Installation
To install MTQQToaster, follow these steps:
1. Clone the repository
2. Build the project using Cmake.
3. Run the executable file to start the application.

## Configuration
Edit the `config.json` file to configure the MQTT broker settings, topics to subscribe to, and other options. The configuration file should be placed in the same directory as the executable.
- `mqtt_url`: The URL of the MQTT broker (e.g., mqtt://broker.hivemq.com:1883)
- `mqtt_topics`: A list of topics to subscribe to. Wildcards are supported (e.g., ["home/+/temperature"])
- `mqtt_user`, `mqtt_pass`: Optional username and password for the MQTT broker
- `log_level`: Logging level (e.g., "INFO", "DEBUG", "ERROR")
- `log_max_size_mb`, `log_max_files`: Logging configuration for file size and number of log rotation files

## Dependencies
- Visual Studio 2026
- CMake
- WinRT (for toast notifications and others)
- vcpkg (for managing dependencies)
- mosquitto (MQTT library)
- spdlog (Logging library)
- nlohmann/json (JSON library)

## License
MTQQToaster is licensed under the MIT License. See the LICENSE file for more information.
For the licenses of the dependencies used in this project, please refer to their respective documentation.

## AI-assisted development

[AGENTS.md](AGENTS.md) contains repository instructions for C++23, Win32, and C++/WinRT development.
The repository skill [cpp-winrt-development](.agents/skills/cpp-winrt-development/SKILL.md) covers implementation, troubleshooting, and review, with references for [building and testing](.agents/skills/cpp-winrt-development/references/build-and-test.md) and [Windows runtime behavior](.agents/skills/cpp-winrt-development/references/windows-runtime.md).

Codex discovers project skills in `.agents/skills/`. Invoke it explicitly with `$cpp-winrt-development`, for example:

```text
$cpp-winrt-development 設定監視の終了処理を確認して、必要な修正と検証を行って
```

Start a new session in this repository to load the new repository instructions. If the skill does not appear, restart Codex.
See the official documentation for [AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md) and [skills](https://learn.chatgpt.com/docs/build-skills).

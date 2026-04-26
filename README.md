WatchTower – Smart LED Wall Clock (ESP32-S3 + WS2812B)
======================================================

A smart, WiFi-enabled digital wall clock built using the ESP32-S3 and WS2812B addressable LEDs, designed to go beyond basic timekeeping. WatchTower transforms a simple clock into an interactive embedded system with real-time monitoring, remote control via a web dashboard, and intelligent network connectivity. From dynamic LED animations to live system diagnostics and NTP-synchronized time, this project demonstrates how embedded devices can be both functional and connected in modern IoT environments.


PROJECT OVERVIEW
----------------
WatchTower is a connected embedded system designed as a highly customizable digital wall clock with robust networking, real-time monitoring, and remote management capabilities. It combines precise LED-based time visualization with a responsive web interface, enabling users to control device behavior, monitor system performance, and perform firmware updates over the air. With simultaneous AP + STA WiFi operation, automatic time synchronization via NTP, and live system observability, the project demonstrates a practical, deployment-ready approach to modern IoT device design.

It highlights:
- Embedded firmware development using ESP-IDF
- Simultaneous dual-mode WiFi (AP + STA)
- Real-time LED control
- Remote device monitoring and diagnostics
- Web-based control interface
- Over-the-Air (OTA) firmware updates
- Hardware-software co-design

The system allows users to control, monitor, debug, and update the device entirely from a browser.


FEATURES
--------

Core Functionality
- Real-time digital clock using WS2812B LEDs
- Adjustable brightness and color customization
- Multiple LED animation modes

Dual WiFi Mode (AP + STA)
- Simultaneous Access Point (AP) and Station (STA) operation
- AP Mode:
  - Always active for direct device access
  - Default gateway IP: 192.168.1.4
- STA Mode:
  - Connects to external WiFi networks
  - IP assigned dynamically by router
- Enables:
  - Direct access via AP
  - Internet connectivity via STA simultaneously

Smart WiFi Management
- Stores multiple STA credentials
- Iteratively attempts connection to saved networks
- Maintains AP availability regardless of STA state
- Designed for reliable and flexible connectivity

Time Synchronization
- Automatic NTP synchronization when STA connection is available
- Ensures accurate and drift-free timekeeping

Web Interface (Control + Monitoring)
- Browser-based dashboard (no app required)
- Accessible via both AP and STA networks
- Configure:
  - Time settings
  - LED colors
  - Animation modes
  - Alarm settings

Over-the-Air (OTA) Updates
- Firmware updates directly via web interface
- No physical connection required
- Supports partition-based OTA updates
- Enables remote feature upgrades and bug fixes

Live System Logs (Remote Debugging)
- Real-time log streaming over WiFi
- Monitor firmware behavior without serial connection
- Useful for debugging deployed systems

System Monitoring Dashboard
- Device status overview
- Live metrics:
  - RAM usage
  - CPU usage
  - Device temperature
  - System uptime
- OTA partition status and memory usage
- Designed for observability and diagnostics

Power & Monitoring
- Battery voltage monitoring using ADC
- Optimized for embedded efficiency

Alarm System
- Configurable alarm feature
- LED-based visual alerts


TECH STACK
----------
- Microcontroller: ESP32-S3
- Framework: ESP-IDF
- LED: WS2812B (NeoPixel)
- Language: C
- Networking:
  - Simultaneous WiFi AP + STA mode
  - HTTP Web Server
- Time Sync: NTP
- Firmware Update: OTA (Over-the-Air)
- Debugging: Live log streaming over network
- Architecture: Modular Embedded Firmware


GETTING STARTED
---------------

Prerequisites
- ESP-IDF installed and configured
- ESP32-S3 development board
- WS2812B LED strip or ring

Build and Flash
---------------
idf.py set-target esp32s3
idf.py build
idf.py flash monitor


WEB INTERFACE USAGE
-------------------
1. Power on the device

2. Connect using either:
   - AP Mode: connect to device hotspot (192.168.1.4)
   OR
   - STA Mode: use IP assigned by your router

3. Open browser:

   http://'your-device-ip'

4. Access:
   - Device controls
   - System dashboard
   - Live logs
   - OTA update interface


OTA UPDATE USAGE
----------------
- Upload new firmware via web dashboard
- Device validates and installs update
- System reboots into updated firmware
- OTA partition status visible in dashboard


LIVE LOG MONITORING
-------------------
- Stream logs directly from ESP32 over WiFi
- Monitor events and system behavior in real-time
- No USB/serial connection required

Use Cases:
- Debugging firmware remotely
- Monitoring system state transitions
- Diagnosing connectivity or timing issues


SYSTEM DASHBOARD METRICS
------------------------
- RAM usage
- CPU usage
- Device temperature
- System uptime
- OTA partition status and usage
- Network status (AP/STA)


LED MODES
---------
- Static color display
- Breathing effect
- Rainbow animation
- Time-reactive display patterns


KEY ENGINEERING HIGHLIGHTS
--------------------------
- True simultaneous WiFi AP + STA operation
- Persistent multi-network credential management
- Secure and reliable OTA firmware update system
- Automatic NTP time synchronization
- Real-time log streaming over network
- Embedded observability dashboard
- Efficient WS2812B LED control with precise timing
- Concurrent task management in embedded environment
- ADC-based battery monitoring
- Modular and scalable firmware architecture


SKILLS DEMONSTRATED
-------------------
- Embedded Systems Development
- ESP-IDF Framework Mastery
- Advanced WiFi Networking (AP + STA concurrency)
- HTTP Server Implementation
- OTA Firmware Update Systems
- Real-Time Firmware Design
- Remote Debugging & Observability
- Memory and Performance Monitoring
- Hardware-Software Integration


FUTURE IMPROVEMENTS
-------------------
- Secure OTA (HTTPS / signed firmware)
- Enhanced log filtering (INFO/WARN/ERROR levels)
- Secure WiFi credential storage
- Mobile-optimized web interface
- RTC backup for offline timekeeping
- MQTT / cloud integration


DEMO (TO ADD)
-------------
- Hardware setup images
- LED animation previews
- Web dashboard screenshots
- Live log interface preview
- OTA update flow

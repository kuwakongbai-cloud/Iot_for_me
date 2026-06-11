# ESP32-S3 AIoT Edge Smart Agriculture Monitoring Node

This project is an industrial-grade edge intelligent agricultural data collection terminal built with the **ESP32-S3 (N16R8)** chip. The system integrates multiple high-precision sensors and uses lightweight MQTT communication without TLS. To address the vulnerabilities of plaintext transmission over the traditional `1883` port, such as Man-in-the-Middle (MITM) attacks and data tampering, this project innovatively introduces an **AES-256-CBC dynamic signing mechanism** and **NTP timestamp anti-replay verification** at the edge. While ensuring low memory and low power operation of the microcontroller, it provides a financial-grade secure data link guarantee.

---

## Core Features

* **Multi-dimensional Environmental Sensing**: Comprehensively covers atmospheric temperature and humidity, light intensity, CO2 concentration, soil temperature, soil electrical conductivity (EC), and soil moisture, among other physical quantities.
* **Edge-Side Hardware-Level Security Defense**:
    * **AES-256-CBC Signature Mechanism**: Hardware-level AES encryption is applied to the collected data and timestamps, then converted into a Base64 signature, uploaded together with the message to prevent data tampering in transit.
    * **Anti-Replay Attack**: Enforces inclusion of NTP millisecond-level network timestamps in the signing process. If the time window between two successive messages is abnormal or the timestamp is earlier than a specific baseline, the server can directly intercept it.
* **Industrial-Grade Robustness Architecture**:
    * **Hardware Watchdog (WDT)**: 15-second hardware guard prevents sudden crashes in extreme environments; the system automatically resets on timeout.
    * **Sensor Fault Tolerance and Permanent Offline Isolation**: Independent software counters track the health status of each sensor. After 3 consecutive read failures, a sensor is marked as "offline", and a stepped hourly asynchronous retry mechanism is initiated to prevent endless loops blocking the main thread.
    * **Zero-String Memory Optimization**: Abandons Arduino's traditional `String` dynamic concatenation, using static character buffers and `snprintf` throughout the entire process, completely eliminating memory wild pointers and memory leaks caused by heap fragmentation.
* **Dynamic Network Self-Healing**: Uses `WiFiManager` for dynamic network provisioning. When the network unexpectedly disconnects, a dynamic exponential backoff algorithm (30s ~ 5min) is activated for reconnection to avoid network storms.

---

## Hardware Architecture and Pin Mapping

### 1. Core Controller
* **MCU**: ESP32-S3-WROOM-1 (N16R8: 16MB Flash / 8MB PSRAM)

### 2. Sensor Bus List
| Sensor Model | Measured Physical Quantity | Communication Protocol/Interface | ESP32-S3 Default Pin (PinConfig) |
| :--- | :--- | :--- | :--- |
| **SHT-31** | Ambient Temperature, Ambient Humidity | I2C (0x44) | SDA: `IO21` / SCL: `IO20` |
| **BH1750** | Ambient Light Intensity | I2C (0x23) | Shared I2C Bus |
| **DS18B20** | Soil Temperature | OneWire | DATA: `IO4` |
| **MH-Z19B** | Ambient Carbon Dioxide (CO2) | HardwareSerial1 | RX: `IO16` / TX: `IO17` |
| **RS485 Sensor** | Soil Electrical Conductivity (EC) | HardwareSerial2 + ModbusRTU | RX: `IO18` / TX: `IO19` / RE_DE: `IO32` |
| **Soil Moisture Sensor** | Soil Relative Water Content | Analog (ADC) | A0: `IO36` |

---

## Security Signing and Anti-Tampering Logic

Since the system operates on the `1883` port without password authentication or TLS encryption, a symmetric encryption bidirectional verification mechanism is designed between the client and server to defend against man-in-the-middle attacks:

### Encryption Flow Diagram
1. **Data Packaging**: Combine sensor values with the current NTP UTC timestamp to create an original plaintext JSON string: `Payload_A`.
2. **IV Mixing and Encryption**: Generate a 16-byte random Initialization Vector (IV) using the hardware Random Number Generator (RNG). Encrypt `Payload_A` using the **AES-256-CBC** algorithm with a 32-byte `SECRET_TOKEN` as the key.
3. **Signature Output**: Combine the `[16-byte IV] + [encrypted ciphertext]` and perform standard **Base64 encoding** to generate the final signature `sign`.
4. **Final Message**: Combine the content of `Payload_A` with the `sign` for transmission.

### Data Message Example
MQTT messages are reported every 15 seconds to the topic `agri/sensor/data`:

```json
{
  "device_id": "esp32_s3_n16r8_001",
  "device_type": "agriculture_sensor",
  "timestamp": 1781253400,
  "data": {
    "air_temperature": 26.54,
    "air_humidity": 62.30,
    "soil_temperature": 22.15,
    "soil_moisture": 45,
    "lux": 12500.00,
    "co2": 450,
    "soil_ec": 1.24
  },
  "sign": "u7G3fK9X...[Base64 encoded string composed of 16-byte IV and ciphertext]..."
}
```
# Backend Server Verification Logic
When the server receives the above message on port 1883, it can perform the following verification steps. If verification fails, the message is discarded:

1. **Timestamp Anti-Replay Check:** Determine whether the `timestamp` in the message falls within the allowed time window. If it is out of range or earlier than a fixed historical baseline `(1704067200)`, it is directly considered an illegal replay message.

2. **Extract and Decode Signature:** Perform `Base64` decoding on the sign field from the final message. Extract the first 16 bytes as the `IV`, and the subsequent bytes as the ciphertext.

3. **Decrypt and Restore:** The server uses the same 32-byte `SECRET_TOKEN` key stored in the backend to decrypt the ciphertext using `AES-256-CBC`.

4. **Strict Consistency Comparison:** Verify whether the decrypted `JSON` string content is completely identical to the plaintext data section. If they match, it indicates that the data has not been tampered with or captured for replay by a middleman.

## Firmware Deployment Guide
### 1. Development Environment Setup
This project is developed based on the PlatformIO platform (highly recommended) and is also compatible with the Arduino IDE.
Core dependency library list:

`WiFiManager by tzapu`

`PubSubClient by Nick O'Leary`

`Adafruit SHT31 Library`

`DallasTemperature & OneWire`

`BH1750_WE`

`MHZ19 by Wouter van Marle`

`ModbusMaster by Doc Walker`

### 2. Confidential Credential Storage
For code security, hardcoding any keys in `main.cpp` is strictly prohibited. You must create a `credentials.h` file in the project's src directory and configure the following content:
```
C++
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// Mandatory: SECRET_TOKEN must be a 32-byte string used as the AES-256 key
#define SECRET_TOKEN "YOUR_32_BYTE_SUPER_SECRET_KEY_!!"

// WiFiManager initial configuration hotspot password
#define WIFI_PASSWORD "Admin123456"

#endif
```
### 3. Configuration, Compilation and Flashing
Please ensure that in `platformio.ini` you have enabled `PSRAM` and correctly selected the board type:
```
text
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.arduino.memory_type = qio_opi
board_build.f_flash = 80000000L
build_flags =
    -DBOARD_HAS_PSRAM
```
## Operation, Maintenance and Debugging Logs
The project has built-in comprehensive hardware self-test and runtime log output (baud rate 115200):

1. **Startup Self-Test:** During initialization, it will self-test buses and print the `====== Sensor Hardware Self-Test Status ======` table.

2. **Fault Isolation:** When any single sensor reaches the consecutive read failure limit of `3` times, the serial port will issue a `[Warning]` message and mark it as permanently offline, retrying in a stepped manner once every hour.

3. **Communication Status Monitoring:** Real-time output of network, NTP synchronization status `[NTP]` Time synchronization completed, and data reporting status `[MQTT]` Data `successfully published -> Length: XXX.`








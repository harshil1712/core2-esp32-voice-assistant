# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-based virtual assistant project targeting the M5Stack Core2 device. The project uses PlatformIO with the ESP-IDF framework for embedded development.

## Architecture

- **Platform**: ESP32 (M5Stack Core2)
- **Framework**: ESP-IDF v4.2 (framework-espidf @ ~3.40200.0)
- **Build System**: PlatformIO + CMake
- **Main Dependencies**: 
  - M5Core2 library v0.1.7 for hardware abstraction
  - ArduinoJson v6.21.3 for JSON processing
- **Project Structure**:
  - `src/` - Main application source code
  - `include/` - Project header files
  - `lib/` - Project-specific private libraries
  - `test/` - Unit tests using PlatformIO Test Runner

## Development Commands

### Building and Flashing
```bash
# Build the project
pio run

# Build and upload to device
pio run --target upload

# Build specific environment
pio run -e m5stack-core2
```

### Monitoring and Debugging
```bash
# Open serial monitor (115200 baud)
pio device monitor

# Upload and monitor in one command
pio run --target upload --target monitor

# Clean build files
pio run --target clean
```

### Testing
```bash
# Run all tests
pio test

# Run tests with verbose output
pio test -v

# Run specific test
pio test -f test_name
```

## Hardware Configuration

- **Board**: M5Stack Core2
- **Upload Speed**: 921600 baud
- **Monitor Speed**: 115200 baud
- **Partition Scheme**: huge_app.csv (for larger applications)
- **PSRAM**: Enabled with cache issue fix
- **Debug Level**: 3 (verbose)

## Security Configuration

The project includes SSL/TLS certificate support with embedded certificates:
- `src/certs/ca_cert.pem` - Certificate Authority certificate
- `src/certs/client_cert.pem` - Client certificate
- `src/certs/client_key.pem` - Client private key

These certificates are embedded into the firmware during build.

## ESP-IDF Specific Notes

- Uses ESP-IDF v4.2 for optimal M5Stack Core2 compatibility
- CMake-based build system with automatic source file discovery
- PSRAM support enabled with ESP32 cache issue workaround
- Custom partition table for applications requiring more flash space
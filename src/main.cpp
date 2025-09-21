#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <memory>

#define WIFI_SSID "WIFI_SSID"
#define WIFI_PASS "PASSWORD"
#define WIFI_MAXIMUM_RETRY 5

// WebSocket server configuration
#define WS_HOST "DNS_OR_IP_ADDRESS" // Replace with your server address
// Alternative: #define WS_HOST "192.168.0.1" // Try router IP if direct connection fails
#define WS_PORT 443   // Replace with your server port
#define WS_PATH "/ws" // WebSocket endpoint
// Test settings: #define WS_PORT 5174 and #define WS_PATH "/"

// Device states for MVP
enum DeviceState
{
    STATE_BOOT,
    STATE_CONNECTING_WIFI,
    STATE_CONNECTING_SERVER,
    STATE_READY,
    STATE_LISTENING,
    STATE_PROCESSING,
    STATE_TRANSCRIBING,
    STATE_SPEAKING,
    STATE_ERROR
};

// Global state variables
DeviceState current_state = STATE_BOOT;
bool wifi_connected = false;
bool websocket_connected = false;
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASS;

// Audio configuration for MVP
#define SAMPLE_RATE 16000
#define I2S_NUM (i2s_port_t)0
#define BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define I2S_SCK 12 // Bit clock (correct)
#define I2S_WS 13  // Word select (corrected for M5Core2)
#define I2S_SD 35  // Serial data (corrected for M5Core2)
#define BUFFER_SIZE 1024

// WebSocket and audio buffer configuration
#define AUDIO_CHUNK_SIZE 48000 // 3 seconds at 16kHz (was 4096 = 0.256s)

// WebSocket client instance
WebSocketsClient webSocket;

// Simple logging macros for Arduino
#define LOG_INFO(tag, format, ...) Serial.printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_ERROR(tag, format, ...) Serial.printf("[ERROR][%s] " format "\n", tag, ##__VA_ARGS__)

// Logging tags
static const char *TAG = "voice_assistant";
static const char *AUDIO_TAG = "audio";
static const char *WS_TAG = "websocket";
static const char *LCD_TAG = "lcd";

// Function declarations
void update_display(const char *message);
void update_display_with_transcription(const char *status, const char *transcription);
void handle_touch();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void handle_transcription_message(const char *json_string);
void init_websocket();
void send_audio_chunk(uint8_t *data, size_t length);
void start_recording();
void stop_recording();
void init_audio();
void set_state(DeviceState new_state);
void play_audio_response(uint8_t *data, size_t length);
void check_processing_timeout();
void check_recording_timeout();

// Global variables for recording state
bool is_recording = false;
std::unique_ptr<int16_t[]> audio_buffer;
size_t audio_buffer_pos = 0;

// Variables for transcription and timeout handling
String last_transcription = "";
String last_response = "";
unsigned long processing_start_time = 0;
const unsigned long PROCESSING_TIMEOUT = 30000; // 30 seconds timeout

// Variables for recording timeout
unsigned long recording_start_time = 0;
const unsigned long RECORDING_TIMEOUT = 5000; // 5 seconds max recording

// Variables for chunked audio reception
bool receiving_chunked_audio = false;
std::unique_ptr<uint8_t[]> chunked_audio_buffer;
size_t expected_audio_size = 0;
size_t received_audio_size = 0;
int expected_chunks = 0;
int received_chunks = 0;

// MVP Implementation - Core Functions

// State management
void set_state(DeviceState new_state)
{
    current_state = new_state;

    switch (new_state)
    {
    case STATE_BOOT:
        update_display("Starting...");
        break;
    case STATE_CONNECTING_WIFI:
        update_display("Connecting WiFi...");
        break;
    case STATE_CONNECTING_SERVER:
        update_display("Connecting Server...");
        break;
    case STATE_READY:
        update_display("TAP TO SPEAK");
        break;
    case STATE_LISTENING:
        update_display("Listening...");
        break;
    case STATE_PROCESSING:
        update_display("Processing...");
        break;
    case STATE_TRANSCRIBING:
        update_display("Transcribing...");
        break;
    case STATE_SPEAKING:
        update_display("Speaking...");
        break;
    case STATE_ERROR:
        update_display("Error - Tap to retry");
        break;
    }
}

// Simple display function
void update_display(const char *message)
{
    LOG_INFO(LCD_TAG, "Display: %s", message);

    // Clear screen
    M5.Display.fillScreen(TFT_BLACK);

    // Set text properties
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);

    // Center the text
    int x = (M5.Display.width() - strlen(message) * 12) / 2;
    int y = M5.Display.height() / 2;

    M5.Display.setCursor(x, y);
    M5.Display.print(message);
}

// Enhanced display function with transcription text
void update_display_with_transcription(const char *status, const char *transcription)
{
    LOG_INFO(LCD_TAG, "Display: %s | Transcription: %s", status, transcription);

    // Clear screen
    M5.Display.fillScreen(TFT_BLACK);

    // Display status at top
    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.print(status);

    // Display transcription in the middle (if available)
    if (transcription && strlen(transcription) > 0)
    {
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, 50);

        // Word wrap for long transcriptions
        String text = String(transcription);
        int maxCharsPerLine = 35; // Approximate for text size 1
        int lineHeight = 20;
        int currentY = 50;

        while (text.length() > 0 && currentY < M5.Display.height() - 20)
        {
            String line = text.substring(0, min((int)text.length(), maxCharsPerLine));

            // Find last space to avoid breaking words
            if (text.length() > maxCharsPerLine)
            {
                int lastSpace = line.lastIndexOf(' ');
                if (lastSpace > 0)
                {
                    line = line.substring(0, lastSpace);
                }
            }

            M5.Display.setCursor(10, currentY);
            M5.Display.print(line);

            text = text.substring(line.length());
            text.trim(); // Remove leading spaces
            currentY += lineHeight;
        }
    }

    // Display instructions at bottom
    M5.Display.setTextColor(TFT_YELLOW);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, M5.Display.height() - 30);
    M5.Display.print("Tap to speak");
}

// Handle JSON transcription messages from server
void handle_transcription_message(const char *json_string)
{
    LOG_INFO(WS_TAG, "Parsing JSON: %s", json_string);

    // Parse JSON using ArduinoJson
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, json_string);

    if (error)
    {
        LOG_ERROR(WS_TAG, "JSON parsing failed: %s", error.c_str());
        return;
    }

    // Check message type
    const char *type = doc["type"];
    if (type == nullptr)
    {
        LOG_ERROR(WS_TAG, "No message type found in JSON");
        return;
    }

    if (strcmp(type, "transcription") == 0)
    {
        // Handle transcription message
        const char *text = doc["text"];
        const char *response = doc["response"];

        if (text != nullptr)
        {
            last_transcription = String(text);
            LOG_INFO(WS_TAG, "Transcription received: %s", text);
        }

        if (response != nullptr)
        {
            last_response = String(response);
            LOG_INFO(WS_TAG, "Response text: %s", response);
        }

        // Update display with transcription
        set_state(STATE_TRANSCRIBING);
        update_display_with_transcription("Transcribed", last_transcription.c_str());
    }
    else if (strcmp(type, "error") == 0)
    {
        // Handle error message
        const char *message = doc["message"];
        if (message != nullptr)
        {
            LOG_ERROR(WS_TAG, "Server error: %s", message);
            update_display_with_transcription("Error", message);
            delay(3000); // Show error for 3 seconds
            set_state(STATE_READY);
        }
    }
    else if (strcmp(type, "audio_start") == 0)
    {
        // Handle chunked audio start
        expected_audio_size = doc["totalSize"];
        expected_chunks = doc["chunks"];
        int chunk_size = doc["chunkSize"];

        LOG_INFO(WS_TAG, "Starting chunked audio reception: %u bytes, %d chunks, %d bytes/chunk",
                 expected_audio_size, expected_chunks, chunk_size);

        // Allocate buffer for full audio
        chunked_audio_buffer.reset(new uint8_t[expected_audio_size]);
        received_audio_size = 0;
        received_chunks = 0;
        receiving_chunked_audio = true;

        update_display_with_transcription("Receiving Audio", "Downloading chunks...");
    }
    else if (strcmp(type, "audio_complete") == 0)
    {
        // Handle chunked audio completion
        LOG_INFO(WS_TAG, "Chunked audio reception complete: %u bytes, %d chunks",
                 received_audio_size, received_chunks);

        if (receiving_chunked_audio && received_audio_size == expected_audio_size)
        {
            // Debug: Check first few bytes of reassembled audio
            LOG_INFO(WS_TAG, "First 8 bytes of reassembled audio: %02x %02x %02x %02x %02x %02x %02x %02x",
                     chunked_audio_buffer[0], chunked_audio_buffer[1], chunked_audio_buffer[2], chunked_audio_buffer[3],
                     chunked_audio_buffer[4], chunked_audio_buffer[5], chunked_audio_buffer[6], chunked_audio_buffer[7]);

            // Comprehensive debugging
            LOG_INFO(WS_TAG, "CHUNK DEBUG: Expected %u bytes, received %u bytes, chunks %d/%d",
                     expected_audio_size, received_audio_size, received_chunks, expected_chunks);

            // Play the reassembled audio
            LOG_INFO(WS_TAG, "Playing reassembled audio: %u bytes", received_audio_size);
            set_state(STATE_SPEAKING);
            play_audio_response(chunked_audio_buffer.get(), received_audio_size);
        }
        else
        {
            LOG_ERROR(WS_TAG, "Audio chunk mismatch: received %u bytes, expected %u bytes",
                      received_audio_size, expected_audio_size);
            update_display_with_transcription("Audio Error", "Incomplete audio received");
            delay(2000);
            set_state(STATE_READY);
        }

        // Reset chunked audio state
        receiving_chunked_audio = false;
        chunked_audio_buffer.reset();
    }
    else if (strcmp(type, "connection") == 0)
    {
        // Handle connection confirmation
        const char *message = doc["message"];
        if (message != nullptr)
        {
            LOG_INFO(WS_TAG, "Connection message: %s", message);
        }
    }
    else
    {
        LOG_INFO(WS_TAG, "Unknown message type: %s", type);
    }
}

// Touch detection
void handle_touch()
{
    M5.update();

    if (M5.Touch.getCount() > 0)
    {
        auto touchDetail = M5.Touch.getDetail();
        LOG_INFO(TAG, "Touch detected at (%d, %d)", touchDetail.x, touchDetail.y);

        // Visual feedback for touch
        M5.Display.fillCircle(touchDetail.x, touchDetail.y, 10, TFT_RED);
        delay(100); // Brief visual feedback

        if (current_state == STATE_READY)
        {
            LOG_INFO(TAG, "Starting recording from touch");
            start_recording();
        }
        else if (current_state == STATE_LISTENING)
        {
            LOG_INFO(TAG, "Stopping recording from touch");
            stop_recording();
        }
        else if (current_state == STATE_ERROR)
        {
            LOG_INFO(TAG, "Retrying connection from touch");
            // Retry connection
            if (!websocket_connected)
            {
                init_websocket();
            }
            else
            {
                set_state(STATE_READY);
            }
        }
        else if (current_state == STATE_TRANSCRIBING)
        {
            LOG_INFO(TAG, "Currently transcribing, ignoring touch");
            update_display_with_transcription("Transcribing...", "Please wait");
        }
        else if (current_state == STATE_PROCESSING)
        {
            LOG_INFO(TAG, "Currently processing, ignoring touch");
            update_display("Processing... Please wait");
        }

        // Wait for touch release to avoid multiple triggers
        while (M5.Touch.getCount() > 0)
        {
            M5.update();
            delay(50);
        }
        delay(100); // Additional debounce
    }
}

// WebSocket event handler
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        LOG_ERROR(WS_TAG, "WebSocket Disconnected - length: %u, heap free: %u bytes", length, ESP.getFreeHeap());
        websocket_connected = false;
        set_state(STATE_ERROR);
        break;
    case WStype_ERROR:
        LOG_ERROR(WS_TAG, "WebSocket Error (length: %u): %s, heap free: %u bytes", length, payload, ESP.getFreeHeap());
        websocket_connected = false;
        set_state(STATE_ERROR);
        break;
    case WStype_CONNECTED:
        LOG_INFO(WS_TAG, "WebSocket Connected to: %s", payload);
        websocket_connected = true;
        set_state(STATE_READY);
        break;
    case WStype_TEXT:
        LOG_INFO(WS_TAG, "Received text: %s", payload);
        // Handle JSON messages (transcription, errors, etc.)
        handle_transcription_message((const char *)payload);
        break;
    case WStype_BIN:
        LOG_INFO(WS_TAG, "Received binary data: %u bytes, heap before: %u bytes", length, ESP.getFreeHeap());

        if (receiving_chunked_audio)
        {
            // This is a chunk of the audio stream
            if (received_audio_size + length <= expected_audio_size)
            {
                memcpy(chunked_audio_buffer.get() + received_audio_size, payload, length);
                received_audio_size += length;
                received_chunks++;

                LOG_INFO(WS_TAG, "Received chunk %d/%d: %u bytes (total: %u/%u bytes)",
                         received_chunks, expected_chunks, length, received_audio_size, expected_audio_size);

                // Update progress display
                int progress = (received_audio_size * 100) / expected_audio_size;
                char progress_text[50];
                snprintf(progress_text, sizeof(progress_text), "Progress: %d%% (%d/%d chunks)",
                         progress, received_chunks, expected_chunks);
                update_display_with_transcription("Receiving Audio", progress_text);
            }
            else
            {
                LOG_ERROR(WS_TAG, "Audio chunk overflow: would exceed expected size");
            }
        }
        else
        {
            // Legacy: single large audio message (fallback)
            LOG_INFO(WS_TAG, "Received complete audio: %u bytes", length);
            set_state(STATE_SPEAKING);
            play_audio_response(payload, length);
        }

        LOG_INFO(WS_TAG, "Binary data processed, heap after: %u bytes", ESP.getFreeHeap());
        break;
    default:
        break;
    }
}

// Initialize WebSocket connection
void init_websocket()
{
    LOG_INFO(WS_TAG, "Initializing WebSocket connection...");
    LOG_INFO(WS_TAG, "Connecting to: wss://%s:%d%s", WS_HOST, WS_PORT, WS_PATH);
    set_state(STATE_CONNECTING_SERVER);

    // webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
    webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    // Disable heartbeat for now
    // webSocket.enableHeartbeat(15000, 3000, 2);
}

// Send audio chunk via WebSocket
void send_audio_chunk(uint8_t *data, size_t length)
{
    if (websocket_connected)
    {
        webSocket.sendBIN(data, length);
        LOG_INFO(WS_TAG, "Sent audio chunk: %d bytes", length);
    }
}

// Initialize audio system
void init_audio()
{
    LOG_INFO(AUDIO_TAG, "Initializing audio system...");

    // Try to use M5Unified microphone first
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.over_sampling = 1;
    mic_cfg.magnification = 16;
    mic_cfg.use_adc = false;

    M5.Mic.config(mic_cfg);
    LOG_INFO(AUDIO_TAG, "M5Unified microphone configured");

    if (M5.Mic.begin())
    {
        LOG_INFO(AUDIO_TAG, "M5 Microphone started successfully");
    }
    else
    {
        LOG_ERROR(AUDIO_TAG, "Failed to start M5 microphone");
    }

    // Fallback to manual I2S configuration
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = BUFFER_SIZE,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0};

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD};

    esp_err_t err = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
    if (err != ESP_OK)
    {
        LOG_ERROR(AUDIO_TAG, "Failed to install I2S driver: %d", err);
    }
    else
    {
        err = i2s_set_pin(I2S_NUM, &pin_config);
        if (err != ESP_OK)
        {
            LOG_ERROR(AUDIO_TAG, "Failed to set I2S pins: %d", err);
        }
        else
        {
            LOG_INFO(AUDIO_TAG, "I2S driver configured successfully");
        }
    }

    // Allocate audio buffer
    audio_buffer.reset(new int16_t[AUDIO_CHUNK_SIZE]);

    LOG_INFO(AUDIO_TAG, "Audio system initialized");
}

// Start recording audio
void start_recording()
{
    if (is_recording)
        return;

    LOG_INFO(AUDIO_TAG, "Starting recording...");
    set_state(STATE_LISTENING);
    is_recording = true;
    audio_buffer_pos = 0;
    recording_start_time = millis(); // Start recording timeout timer

    // Visual feedback for recording start
    update_display_with_transcription("RECORDING", "Speak now... Tap again to stop");

    // Add a red recording indicator
    M5.Display.fillCircle(M5.Display.width() - 20, 20, 8, TFT_RED);
}

// Stop recording and send audio
void stop_recording()
{
    if (!is_recording)
        return;

    LOG_INFO(AUDIO_TAG, "Stopping recording...");
    set_state(STATE_PROCESSING);
    is_recording = false;
    processing_start_time = millis(); // Start timeout timer

    if (audio_buffer_pos > 0)
    {
        // Send recorded audio to server
        send_audio_chunk((uint8_t *)audio_buffer.get(), audio_buffer_pos * sizeof(int16_t));
        LOG_INFO(AUDIO_TAG, "Sent %d samples (%d bytes) to server",
                 audio_buffer_pos, audio_buffer_pos * sizeof(int16_t));
    }
    else
    {
        LOG_ERROR(AUDIO_TAG, "No audio data to send");
        set_state(STATE_READY);
    }

    audio_buffer_pos = 0;
}

// Play audio response
void play_audio_response(uint8_t *data, size_t length)
{
    LOG_INFO(AUDIO_TAG, "Playing audio response: %d bytes", length);

    // Check if this looks like an error beep (short audio)
    if (length < 1000)
    {
        LOG_INFO(AUDIO_TAG, "Short audio detected - likely error beep");
        update_display_with_transcription("Error", "Server error occurred");
    }
    else
    {
        update_display_with_transcription("Speaking", last_response.c_str());
    }

    // Set volume and play audio
    M5.Speaker.setVolume(180); // Increase volume for better audibility

    // Check if audio data looks valid (back to working validation)
    bool validAudio = false;
    int16_t maxSample = 0;
    int nonZeroSamples = 0;

    if (length >= 2)
    {
        // First, find where actual audio starts (skip initial zeros/headers)
        size_t audioStartByte = 0;
        for (size_t byte = 0; byte < min(length, (size_t)200); byte += 2)
        {
            int16_t sample = (int16_t)(data[byte] | (data[byte + 1] << 8));
            if (abs(sample) > 20) // Found meaningful audio data
            {
                audioStartByte = byte;
                LOG_INFO(AUDIO_TAG, "Audio data starts at byte %d", audioStartByte);
                break;
            }
        }

        // Analyze samples starting from where audio actually begins
        size_t startSample = audioStartByte / 2;
        size_t totalSamples = length / 2;
        size_t samplesToCheck = min(totalSamples - startSample, (size_t)100);

        LOG_INFO(AUDIO_TAG, "Checking %d samples starting from sample %d", samplesToCheck, startSample);

        for (size_t i = 0; i < samplesToCheck; i++)
        {
            size_t sampleIndex = startSample + i;
            size_t byteIndex = sampleIndex * 2;

            if (byteIndex + 1 < length)
            {
                // ESP32 uses little-endian format
                int16_t sample = (int16_t)(data[byteIndex] | (data[byteIndex + 1] << 8));
                int16_t absSample = abs(sample);

                if (absSample > 50) // Lower threshold for compressed audio
                {
                    nonZeroSamples++;
                    if (absSample > maxSample)
                        maxSample = absSample;
                }
            }
        }

        // Valid if we have some non-zero samples (>5% of checked samples)
        if (samplesToCheck > 0)
        {
            validAudio = nonZeroSamples > (samplesToCheck / 20);
        }

        LOG_INFO(AUDIO_TAG, "Audio validation: %d/%d non-zero samples, max: %d, valid: %s",
                 nonZeroSamples, samplesToCheck, maxSample, validAudio ? "YES" : "NO");
    }

    if (validAudio)
    {
        // Play audio starting from where actual data begins (skip headers)
        size_t audioStartByte = 0;
        for (size_t byte = 0; byte < min(length, (size_t)200); byte += 2)
        {
            int16_t sample = (int16_t)(data[byte] | (data[byte + 1] << 8));
            if (abs(sample) > 20)
            {
                audioStartByte = byte;
                break;
            }
        }

        size_t audioDataLength = length - audioStartByte;
        LOG_INFO(AUDIO_TAG, "Playing audio from byte %d, length %d", audioStartByte, audioDataLength);

        // Try to play as raw PCM audio at 16kHz (back to working state)
        M5.Speaker.playRaw(data + audioStartByte, audioDataLength, SAMPLE_RATE, false);

        // Wait for playback to complete
        while (M5.Speaker.isPlaying())
        {
            delay(10);
            // Allow other tasks to run during playback
            webSocket.loop();
        }
    }
    else
    {
        LOG_ERROR(AUDIO_TAG, "Invalid audio data received");
        // Play a simple beep as fallback
        M5.Speaker.tone(800, 500); // 800Hz for 500ms
        delay(500);
    }

    LOG_INFO(AUDIO_TAG, "Audio playback completed");

    // Return to ready state after playing audio
    delay(500); // Small delay before showing ready state
    set_state(STATE_READY);

    // Show the transcription result for a few seconds
    if (last_transcription.length() > 0)
    {
        update_display_with_transcription("Ready", last_transcription.c_str());
    }
}

// Check for processing timeout
void check_processing_timeout()
{
    if (current_state == STATE_PROCESSING || current_state == STATE_TRANSCRIBING)
    {
        if (millis() - processing_start_time > PROCESSING_TIMEOUT)
        {
            LOG_ERROR(TAG, "Processing timeout reached");
            update_display_with_transcription("Timeout", "No response from server");
            delay(3000);
            set_state(STATE_READY);
        }
    }
}

// Check for recording timeout
void check_recording_timeout()
{
    if (is_recording)
    {
        if (millis() - recording_start_time > RECORDING_TIMEOUT)
        {
            LOG_INFO(AUDIO_TAG, "Recording timeout reached (5 seconds)");
            stop_recording();
        }
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    LOG_INFO(TAG, "=== M5Stack Core2 Voice Assistant MVP ===");
    LOG_INFO(TAG, "Initial heap: %u bytes", ESP.getFreeHeap());

    // Initialize state
    set_state(STATE_BOOT);

    // Initialize M5Stack
    M5.begin();
    LOG_INFO(TAG, "M5Stack initialized, heap: %u bytes", ESP.getFreeHeap());

    // Initialize audio
    init_audio();
    LOG_INFO(TAG, "Audio initialized, heap: %u bytes", ESP.getFreeHeap());

    // Connect to WiFi
    set_state(STATE_CONNECTING_WIFI);
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
        delay(500);
        attempts++;
        LOG_INFO(TAG, "WiFi connection attempt %d/20", attempts);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        wifi_connected = true;
        LOG_INFO(TAG, "WiFi connected: %s", WiFi.localIP().toString().c_str());
        LOG_INFO(TAG, "Gateway: %s", WiFi.gatewayIP().toString().c_str());
        LOG_INFO(TAG, "DNS: %s", WiFi.dnsIP().toString().c_str());

        // Test basic connectivity first
        WiFiClient testClient;
        LOG_INFO(TAG, "Testing HTTP connection to %s:%d", WS_HOST, WS_PORT);
        if (testClient.connect(WS_HOST, WS_PORT))
        {
            LOG_INFO(TAG, "HTTP connection successful!");
            testClient.stop();
        }
        else
        {
            LOG_ERROR(TAG, "HTTP connection failed!");
        }

        // Initialize WebSocket
        init_websocket();
    }
    else
    {
        LOG_ERROR(TAG, "WiFi connection failed");
        set_state(STATE_ERROR);
    }
}

void loop()
{
    // Handle WebSocket events
    webSocket.loop();

    // Handle touch input
    handle_touch();

    // Check for processing timeouts
    check_processing_timeout();

    // Check for recording timeouts
    check_recording_timeout();

    // Read audio data when recording
    if (is_recording)
    {
        int16_t temp_buffer[BUFFER_SIZE];
        size_t samples_read = 0;

        // Use I2S to read audio data directly
        size_t bytes_read = 0;
        esp_err_t ret = i2s_read(I2S_NUM, temp_buffer, BUFFER_SIZE * sizeof(int16_t),
                                 &bytes_read, 10 / portTICK_PERIOD_MS);

        if (ret == ESP_OK && bytes_read > 0)
        {
            samples_read = bytes_read / sizeof(int16_t);

            // Debug: Print first few samples to check for actual audio data
            if (samples_read > 4 && audio_buffer_pos < 100) // Only log first few chunks
            {
                Serial.printf("Audio samples: %d, %d, %d, %d (total: %d)\n",
                              temp_buffer[0], temp_buffer[1], temp_buffer[2], temp_buffer[3], samples_read);
            }

            // Copy samples to our buffer if there's space
            for (size_t i = 0; i < samples_read && audio_buffer_pos < AUDIO_CHUNK_SIZE; i++)
            {
                audio_buffer[audio_buffer_pos++] = temp_buffer[i];
            }
        }
        else if (ret != ESP_OK)
        {
            Serial.printf("I2S read failed: ret=%d, bytes_read=%d\n", ret, bytes_read);
        }

        // Visual feedback - pulse recording indicator
        static unsigned long lastPulse = 0;
        static bool pulseState = false;
        if (millis() - lastPulse > 500) // Pulse every 500ms
        {
            pulseState = !pulseState;
            uint16_t color = pulseState ? TFT_RED : TFT_MAROON; // Use TFT_MAROON instead of TFT_DARKRED
            M5.Display.fillCircle(M5.Display.width() - 20, 20, 8, color);
            lastPulse = millis();
        }

        // Safety check - stop if buffer is nearly full (keep some margin)
        if (audio_buffer_pos >= AUDIO_CHUNK_SIZE - 1000)
        {
            LOG_INFO(AUDIO_TAG, "Buffer nearly full, stopping recording for safety");
            stop_recording();
        }
    }

    delay(10);
}
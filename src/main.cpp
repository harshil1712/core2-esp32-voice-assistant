#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <memory>
#include <cmath>

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
#define BUFFER_SIZE 1024
// Note: I2S configuration handled by M5Unified microphone API

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
// void test_speaker_hardware();

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
        else if (current_state == STATE_SPEAKING)
        {
            LOG_INFO(TAG, "Currently playing audio, ignoring touch");
            update_display_with_transcription("Playing Audio", "Please wait for completion");
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
    LOG_INFO(AUDIO_TAG, "Playing audio response: %u bytes", (unsigned)length);

    // DEBUGGING: Dump first 100 bytes as hex to see exact data
    LOG_INFO(AUDIO_TAG, "Raw audio data dump:");
    for (int i = 0; i < min(100, (int)length); i += 16)
    {
        char hex_line[120];
        sprintf(hex_line, "%04x: ", i);
        for (int j = 0; j < 16 && (i + j) < length; j++)
        {
            sprintf(hex_line + strlen(hex_line), "%02x ", data[i + j]);
        }
        Serial.println(hex_line);
    }

    // Enhanced audio debugging - log first 16 bytes to analyze format
    if (length >= 16)
    {
        LOG_INFO(AUDIO_TAG, "Audio format analysis - First 16 bytes (should be immediate audio data):");
        for (int i = 0; i < 16; i += 2)
        {
            int16_t sample = (int16_t)(data[i] | (data[i + 1] << 8));
            Serial.printf("  [%d-%d]: 0x%02x%02x -> %d\n", i, i + 1, data[i], data[i + 1], sample);
        }
    }

    // Short-audio check (keep existing behavior)
    if (length < 1000)
    {
        LOG_INFO(AUDIO_TAG, "Short audio detected - likely error beep");
        update_display_with_transcription("Error", "Server error occurred");
    }
    else
    {
        update_display_with_transcription("Speaking", last_response.c_str());
    }

    // Set volume and configure speaker for better quality
    M5.Speaker.setVolume(120);           // Balanced volume for clarity
    M5.Speaker.setChannelVolume(0, 120); // Ensure consistent channel volume

    // ---------------------------
    // New validation: peak + RMS
    // ---------------------------
    bool validAudio = false;

    if (length >= 2)
    {
        size_t totalSamples = length / 2;
        size_t samplesToCheck = min(totalSamples, (size_t)1024);

        LOG_INFO(AUDIO_TAG, "Validating %u samples for peak/RMS", (unsigned)samplesToCheck);

        if (samplesToCheck == 0)
        {
            LOG_ERROR(AUDIO_TAG, "No samples available to validate");
            validAudio = false;
        }
        else
        {
            uint64_t sumSquares = 0;
            int16_t peakSample = 0;

            for (size_t i = 0; i < samplesToCheck; ++i)
            {
                size_t byteIndex = i * 2;
                int16_t sample = (int16_t)(data[byteIndex] | (data[byteIndex + 1] << 8));
                int16_t absS = (sample < 0) ? -sample : sample;
                sumSquares += (uint64_t)absS * (uint64_t)absS;
                if (absS > peakSample)
                    peakSample = absS;
            }

            double rms = sqrt((double)sumSquares / (double)samplesToCheck);

            LOG_INFO(AUDIO_TAG, "Validation metrics: peak=%d, rms=%.1f", peakSample, rms);

            // Tunable thresholds (adjust after testing)
            const int PEAK_THRESHOLD = 300;    // brief transient threshold
            const double RMS_THRESHOLD = 40.0; // sustained energy threshold

            // Accept if either measure indicates audio energy
            validAudio = (peakSample >= PEAK_THRESHOLD) || (rms >= RMS_THRESHOLD);

            // Fallback: accept very-low-energy signals to avoid false negatives
            if (!validAudio && peakSample > 0 && rms > 1.0)
            {
                LOG_INFO(AUDIO_TAG, "Low-energy audio detected; accepting as fallback (peak=%d, rms=%.1f)", peakSample, rms);
                validAudio = true;
            }
        }
    }
    else
    {
        validAudio = false;
    }

    LOG_INFO(AUDIO_TAG, "Audio validation result: %s", validAudio ? "ACCEPTED" : "REJECTED");

    // ---------------------------
    // Play or fallback
    // ---------------------------
    if (validAudio)
    {
        // AUTOMATIC HARDWARE TEST: Play clean tone first for comparison
        // LOG_INFO(AUDIO_TAG, "STEP 1: Playing clean hardware test tone for comparison");
        // update_display_with_transcription("Testing Hardware", "Clean 800Hz tone...");

        // test_speaker_hardware();

        // delay(500); // Brief pause between test and actual audio

        // NOW PLAY SERVER AUDIO
        LOG_INFO(AUDIO_TAG, "STEP 2: Playing server audio: %u bytes", (unsigned)length);
        update_display_with_transcription("Playing Server Audio", "Listen for noise/distortion...");

        // Fallback: use 24kHz playback rate. If you have a global audioFileSampleRate,
        // replace the hardcoded 24000 below with that variable (ensure it's declared above).
        uint32_t playback_rate = 24000;

        LOG_INFO(AUDIO_TAG, "Playing audio at %u Hz", playback_rate);

        // Configure speaker for optimal playback before playing
        M5.Speaker.stop();                   // Ensure clean start
        M5.Speaker.setAllChannelVolume(120); // Set all channels consistently

        // STREAMING PLAYBACK: Use triple-buffering like SD card code for smooth playback
        static constexpr const size_t buf_num = 3;
        static constexpr const size_t buf_size = 1024;
        static uint8_t play_buffers[buf_num][buf_size];

        LOG_INFO(AUDIO_TAG, "Streaming audio with triple-buffering");

        size_t data_remaining = length;
        size_t data_pos = 0;
        size_t buf_idx = 0;

        // Stream audio using rotating buffers - no waiting between chunks
        while (data_remaining > 0)
        {
            size_t chunk_len = (data_remaining < buf_size) ? data_remaining : buf_size;

            // Copy chunk to buffer
            memcpy(play_buffers[buf_idx], data + data_pos, chunk_len);

            // Queue chunk for playback (non-blocking with wait=0)
            // For 16-bit audio: cast to int16_t* and pass sample count (bytes >> 1)
            M5.Speaker.playRaw((const int16_t *)play_buffers[buf_idx], chunk_len >> 1, playback_rate, false, 1, 0);

            data_remaining -= chunk_len;
            data_pos += chunk_len;
            buf_idx = (buf_idx < (buf_num - 1)) ? buf_idx + 1 : 0;
        }

        // Wait for final playback to complete
        while (M5.Speaker.isPlaying())
        {
            delay(50);
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

// Test speaker hardware with pure tone
// void test_speaker_hardware()
// {
//     LOG_INFO(AUDIO_TAG, "Testing speaker hardware with 800Hz tone");

//     const int sample_rate = 24000;
//     const int duration_ms = 200; // 1 second
//     const int frequency = 100;   // 800Hz tone
//     const int samples = sample_rate * duration_ms / 1000;

//     // Generate pure sine wave
//     int16_t *tone_buffer = new int16_t[samples];
//     for (int i = 0; i < samples; i++)
//     {
//         float t = (float)i / sample_rate;
//         float amplitude = 0.3; // 30% volume to avoid clipping
//         tone_buffer[i] = (int16_t)(amplitude * 32767 * sin(2 * M_PI * frequency * t));
//     }

//     LOG_INFO(AUDIO_TAG, "Generated %d samples for tone test", samples);

//     // Configure and play tone
//     M5.Speaker.stop();
//     M5.Speaker.setVolume(120);
//     M5.Speaker.playRaw((uint8_t *)tone_buffer, samples * sizeof(int16_t), sample_rate, false, 1, 0);

//     // Wait for completion
//     while (M5.Speaker.isPlaying())
//     {
//         delay(50);
//     }

//     delete[] tone_buffer;
//     LOG_INFO(AUDIO_TAG, "Hardware tone test completed");
// }

void loop()
{
    // Handle WebSocket events (skip during critical audio playback)
    if (current_state != STATE_SPEAKING)
    {
        webSocket.loop();
    }

    // Handle touch input
    handle_touch();

    // Check for processing timeouts
    check_processing_timeout();

    // Check for recording timeouts
    check_recording_timeout();

    // Read audio data when recording
    if (is_recording)
    {
        // Use M5Unified microphone API (NOT direct i2s_read)
        if (M5.Mic.isEnabled())
        {
            // Record directly into our buffer
            size_t samples_to_read = min((size_t)BUFFER_SIZE, AUDIO_CHUNK_SIZE - audio_buffer_pos);

            if (samples_to_read > 0 && M5.Mic.record(audio_buffer.get() + audio_buffer_pos, samples_to_read, SAMPLE_RATE))
            {
                // Debug: Print first few samples to verify real audio
                if (audio_buffer_pos < 100) // Only log first few chunks
                {
                    Serial.printf("Audio samples: %d, %d, %d, %d (total: %zu)\n",
                                  audio_buffer[audio_buffer_pos],
                                  audio_buffer[audio_buffer_pos + 1],
                                  audio_buffer[audio_buffer_pos + 2],
                                  audio_buffer[audio_buffer_pos + 3],
                                  samples_to_read);
                }

                audio_buffer_pos += samples_to_read;
            }
        }

        // Visual feedback - pulse recording indicator
        static unsigned long lastPulse = 0;
        static bool pulseState = false;
        if (millis() - lastPulse > 500) // Pulse every 500ms
        {
            pulseState = !pulseState;
            uint16_t color = pulseState ? TFT_RED : TFT_MAROON;
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
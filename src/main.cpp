#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <memory>

#define WIFI_SSID "WIFI_SSID_HERE"
#define WIFI_PASS "WIFI_PASSWORD_HERE"
#define WIFI_MAXIMUM_RETRY 5

// WebSocket server configuration
#define WS_HOST "DEPLOYED_WS_URL" // Replace with your server host
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
#define I2S_SCK 12
#define I2S_WS 0
#define I2S_SD 34
#define BUFFER_SIZE 1024

// WebSocket and audio buffer configuration
#define AUDIO_CHUNK_SIZE 4096

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
void handle_touch();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void init_websocket();
void send_audio_chunk(uint8_t *data, size_t length);
void start_recording();
void stop_recording();
void init_audio();
void set_state(DeviceState new_state);
void play_audio_response(uint8_t *data, size_t length);

// Global variables for recording state
bool is_recording = false;
std::unique_ptr<int16_t[]> audio_buffer;
size_t audio_buffer_pos = 0;

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

// Touch detection
void handle_touch()
{
    M5.update();

    if (M5.Touch.getCount() > 0)
    {
        LOG_INFO(TAG, "Touch detected");
        delay(200); // Debounce touch

        if (current_state == STATE_READY)
        {
            start_recording();
        }
        else if (current_state == STATE_LISTENING)
        {
            stop_recording();
        }
        else if (current_state == STATE_ERROR)
        {
            // Retry connection
            if (!websocket_connected)
            {
                init_websocket();
            }
        }
    }
}

// WebSocket event handler
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        LOG_INFO(WS_TAG, "WebSocket Disconnected");
        websocket_connected = false;
        set_state(STATE_ERROR);
        break;
    case WStype_ERROR:
        LOG_ERROR(WS_TAG, "WebSocket Error: %s", payload);
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
        break;
    case WStype_BIN:
        LOG_INFO(WS_TAG, "Received binary data: %u bytes", length);
        // This is audio response from server
        set_state(STATE_SPEAKING);
        play_audio_response(payload, length);
        set_state(STATE_READY);
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
        return;
    }

    err = i2s_set_pin(I2S_NUM, &pin_config);
    if (err != ESP_OK)
    {
        LOG_ERROR(AUDIO_TAG, "Failed to set I2S pins: %d", err);
        return;
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
}

// Stop recording and send audio
void stop_recording()
{
    if (!is_recording)
        return;

    LOG_INFO(AUDIO_TAG, "Stopping recording...");
    set_state(STATE_PROCESSING);
    is_recording = false;

    if (audio_buffer_pos > 0)
    {
        // Send recorded audio to server
        send_audio_chunk((uint8_t *)audio_buffer.get(), audio_buffer_pos * sizeof(int16_t));
    }

    audio_buffer_pos = 0;
}

// Play audio response
void play_audio_response(uint8_t *data, size_t length)
{
    LOG_INFO(AUDIO_TAG, "Playing audio response: %d bytes", length);

    // Use M5.Speaker to play the audio
    M5.Speaker.setVolume(128);
    M5.Speaker.playRaw(data, length, SAMPLE_RATE, false);

    // Wait for playback to complete
    while (M5.Speaker.isPlaying())
    {
        delay(10);
    }

    LOG_INFO(AUDIO_TAG, "Audio playback completed");
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    LOG_INFO(TAG, "=== M5Stack Core2 Voice Assistant MVP ===");

    // Initialize state
    set_state(STATE_BOOT);

    // Initialize M5Stack
    M5.begin();
    LOG_INFO(TAG, "M5Stack initialized");

    // Initialize audio
    init_audio();

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

    // Read audio data when recording
    if (is_recording)
    {
        int16_t temp_buffer[BUFFER_SIZE];
        size_t bytes_read = 0;

        esp_err_t ret = i2s_read(I2S_NUM, temp_buffer, BUFFER_SIZE * sizeof(int16_t),
                                 &bytes_read, 10 / portTICK_PERIOD_MS);

        if (ret == ESP_OK && bytes_read > 0)
        {
            size_t samples_read = bytes_read / sizeof(int16_t);

            // Copy samples to our buffer if there's space
            for (size_t i = 0; i < samples_read && audio_buffer_pos < AUDIO_CHUNK_SIZE; i++)
            {
                audio_buffer[audio_buffer_pos++] = temp_buffer[i];
            }

            // Auto-stop if buffer is full
            if (audio_buffer_pos >= AUDIO_CHUNK_SIZE)
            {
                LOG_INFO(AUDIO_TAG, "Buffer full, stopping recording");
                stop_recording();
            }
        }
    }

    delay(10);
}
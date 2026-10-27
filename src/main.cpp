#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <cstring>
#include <cmath>

#define WIFI_SSID "Harshil-2.4"
#define WIFI_PASS "!9q*Dh*Az%PyqYNe"
#define WIFI_MAXIMUM_RETRY 5

#define WS_HOST "ws-server-core2.api-harshil.workers.dev"
#define WS_PORT 443
#define WS_PATH "/agents/voice-agent/default"

enum DeviceState
{
    STATE_BOOT,
    STATE_CONNECTING_WIFI,
    STATE_CONNECTING_SERVER,
    STATE_READY,
    STATE_IN_CALL,
    STATE_ERROR
};

DeviceState current_state = STATE_BOOT;
bool wifi_connected = false;
bool websocket_connected = false;
const char *ssid = WIFI_SSID;
const char *password = WIFI_PASS;

#define SAMPLE_RATE 16000
#define MIC_BUFFER_SIZE 1600
#define SPEAKER_SAMPLE_RATE 24000

// Triple-buffer for audio playback (copied from working implementation)
static constexpr const size_t PLAY_BUF_NUM = 3;
static constexpr const size_t PLAY_BUF_SIZE = 1024;
static uint8_t play_buffers[PLAY_BUF_NUM][PLAY_BUF_SIZE];

WebSocketsClient webSocket;

#define LOG_INFO(tag, format, ...) Serial.printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#define LOG_ERROR(tag, format, ...) Serial.printf("[ERROR][%s] " format "\n", tag, ##__VA_ARGS__)

static const char *TAG = "voice_assistant";
static const char *AUDIO_TAG = "audio";
static const char *WS_TAG = "websocket";
static const char *LCD_TAG = "lcd";

void update_display(const char *message);
void update_display_with_text(const char *status, const char *detail);
void handle_touch();
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
void handle_server_message(const char *json_string);
void init_websocket();
void init_audio();
void set_state(DeviceState new_state);
void start_call();
void end_call();
void start_playback();
void stop_playback();

bool in_call = false;
bool is_playing = false;
bool pending_stop_playback = false; // deferred stop — processed in loop()
String last_user_text = "";
String last_assistant_text = "";
String streaming_text = "";
bool is_streaming_response = false;
uint32_t audio_sample_rate = SPEAKER_SAMPLE_RATE;

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
        update_display("TAP TO CALL");
        break;
    case STATE_IN_CALL:
        update_display_with_text("In Call", "Listening...");
        break;
    case STATE_ERROR:
        update_display("Error - Tap to retry");
        break;
    }
}

void update_display(const char *message)
{
    LOG_INFO(LCD_TAG, "Display: %s", message);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(2);
    int x = (M5.Display.width() - strlen(message) * 12) / 2;
    int y = M5.Display.height() / 2;
    M5.Display.setCursor(x, y);
    M5.Display.print(message);
}

void update_display_with_text(const char *status, const char *detail)
{
    LOG_INFO(LCD_TAG, "Display: %s | %s", status, detail);
    M5.Display.fillScreen(TFT_BLACK);

    M5.Display.setTextColor(TFT_CYAN);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.print(status);

    if (detail && strlen(detail) > 0)
    {
        M5.Display.setTextColor(TFT_WHITE);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, 50);

        String text = String(detail);
        int maxCharsPerLine = 35;
        int lineHeight = 20;
        int currentY = 50;

        while (text.length() > 0 && currentY < M5.Display.height() - 20)
        {
            String line = text.substring(0, min((int)text.length(), maxCharsPerLine));
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
            text.trim();
            currentY += lineHeight;
        }
    }

    if (in_call)
    {
        M5.Display.setTextColor(TFT_YELLOW);
        M5.Display.setTextSize(1);
        M5.Display.setCursor(10, M5.Display.height() - 30);
        M5.Display.print("Tap to end call");

        M5.Display.fillCircle(M5.Display.width() - 20, 20, 8, TFT_GREEN);
    }
}

void handle_server_message(const char *json_string)
{
    LOG_INFO(WS_TAG, "Parsing JSON: %s", json_string);

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, json_string);

    if (error)
    {
        LOG_ERROR(WS_TAG, "JSON parsing failed: %s", error.c_str());
        return;
    }

    const char *type = doc["type"];
    if (type == nullptr)
    {
        LOG_ERROR(WS_TAG, "No message type found in JSON");
        return;
    }

    if (strcmp(type, "welcome") == 0)
    {
        int protocol_version = doc["protocol_version"] | 0;
        LOG_INFO(WS_TAG, "Server welcome, protocol version: %d", protocol_version);
    }
    else if (strcmp(type, "status") == 0)
    {
        const char *status = doc["status"];
        if (status == nullptr)
            return;

        LOG_INFO(WS_TAG, "Server status: %s", status);

        if (strcmp(status, "idle") == 0)
        {
            // Defer stop to loop() — don't block the WebSocket callback
            if (is_playing)
                pending_stop_playback = true;
            else
                is_playing = false;
            update_display_with_text("Idle", last_assistant_text.c_str());
        }
        else if (strcmp(status, "listening") == 0)
        {
            // Defer stop to loop() — don't block the WebSocket callback
            if (is_playing)
                pending_stop_playback = true;
            else
                is_playing = false;
            update_display_with_text("Listening", last_user_text.c_str());
        }
        else if (strcmp(status, "thinking") == 0)
        {
            update_display_with_text("Thinking...", streaming_text.c_str());
        }
        else if (strcmp(status, "speaking") == 0)
        {
            start_playback();
            update_display_with_text("Speaking", last_assistant_text.c_str());
        }
    }
    else if (strcmp(type, "transcript") == 0)
    {
        const char *role = doc["role"];
        const char *text = doc["text"];
        if (role == nullptr || text == nullptr)
            return;

        LOG_INFO(WS_TAG, "Transcript [%s]: %s", role, text);

        if (strcmp(role, "user") == 0)
        {
            last_user_text = String(text);
            update_display_with_text("Heard", last_user_text.c_str());
        }
        else if (strcmp(role, "assistant") == 0)
        {
            last_assistant_text = String(text);
            is_streaming_response = false;
            streaming_text = "";
            update_display_with_text("Response", last_assistant_text.c_str());
        }
    }
    else if (strcmp(type, "transcript_interim") == 0)
    {
        const char *text = doc["text"];
        if (text != nullptr)
        {
            LOG_INFO(WS_TAG, "Interim transcript: %s", text);
            update_display_with_text("Hearing...", text);
        }
    }
    else if (strcmp(type, "transcript_start") == 0)
    {
        is_streaming_response = true;
        streaming_text = "";
        LOG_INFO(WS_TAG, "Streaming response started");
    }
    else if (strcmp(type, "transcript_delta") == 0)
    {
        const char *text = doc["text"];
        if (text != nullptr)
        {
            streaming_text += String(text);
            update_display_with_text("Thinking", streaming_text.c_str());
        }
    }
    else if (strcmp(type, "transcript_end") == 0)
    {
        const char *text = doc["text"];
        if (text != nullptr)
        {
            last_assistant_text = String(text);
        }
        else
        {
            last_assistant_text = streaming_text;
        }
        is_streaming_response = false;
        streaming_text = "";
        LOG_INFO(WS_TAG, "Streaming response complete: %s", last_assistant_text.c_str());
    }
    else if (strcmp(type, "audio_config") == 0)
    {
        const char *format = doc["format"];
        if (doc["sampleRate"].is<int>())
        {
            audio_sample_rate = doc["sampleRate"].as<int>();
        }
        LOG_INFO(WS_TAG, "Audio config: format=%s, sampleRate=%u",
                 format ? format : "unknown", audio_sample_rate);
    }
    else if (strcmp(type, "metrics") == 0)
    {
        int llm_ms = doc["llm_ms"] | 0;
        int tts_ms = doc["tts_ms"] | 0;
        int first_audio_ms = doc["first_audio_ms"] | 0;
        int total_ms = doc["total_ms"] | 0;
        LOG_INFO(WS_TAG, "Metrics: llm=%dms tts=%dms first_audio=%dms total=%dms",
                 llm_ms, tts_ms, first_audio_ms, total_ms);
    }
    else if (strcmp(type, "error") == 0)
    {
        const char *message = doc["message"];
        if (message != nullptr)
        {
            LOG_ERROR(WS_TAG, "Server error: %s", message);
            update_display_with_text("Error", message);
        }
    }
    else
    {
        LOG_INFO(WS_TAG, "Unknown message type: %s", type);
    }
}

void handle_touch()
{
    M5.update();

    if (M5.Touch.getCount() > 0)
    {
        auto touchDetail = M5.Touch.getDetail();
        LOG_INFO(TAG, "Touch detected at (%d, %d)", touchDetail.x, touchDetail.y);

        M5.Display.fillCircle(touchDetail.x, touchDetail.y, 10, TFT_RED);
        delay(100);

        if (current_state == STATE_READY)
        {
            LOG_INFO(TAG, "Starting call from touch");
            start_call();
        }
        else if (current_state == STATE_IN_CALL)
        {
            LOG_INFO(TAG, "Ending call from touch");
            end_call();
        }
        else if (current_state == STATE_ERROR)
        {
            LOG_INFO(TAG, "Retrying connection from touch");
            if (!websocket_connected)
            {
                init_websocket();
            }
            else
            {
                set_state(STATE_READY);
            }
        }

        while (M5.Touch.getCount() > 0)
        {
            M5.update();
            delay(50);
        }
        delay(100);
    }
}

void start_call()
{
    if (in_call)
        return;

    LOG_INFO(WS_TAG, "Starting voice call");
    in_call = true;
    last_user_text = "";
    last_assistant_text = "";
    streaming_text = "";
    is_streaming_response = false;
    pending_stop_playback = false;

    webSocket.sendTXT("{\"type\":\"start_call\",\"preferred_format\":\"pcm16\"}");
    set_state(STATE_IN_CALL);
}

void end_call()
{
    if (!in_call)
        return;

    LOG_INFO(WS_TAG, "Ending voice call");
    in_call = false;
    is_streaming_response = false;
    is_playing = false;

    M5.Speaker.stop();
    webSocket.sendTXT("{\"type\":\"end_call\"}");
    set_state(STATE_READY);
}

// Core2 has separate I2S for mic and speaker - no switching needed
// Just pause mic streaming while playing audio to avoid feedback
void start_playback()
{
    if (is_playing)
        return;
    LOG_INFO(AUDIO_TAG, "Starting audio playback");
    M5.Speaker.stop();
    M5.Speaker.setVolume(255);
    M5.Speaker.setAllChannelVolume(255);
    is_playing = true;
}

void stop_playback()
{
    if (!is_playing)
        return;
    LOG_INFO(AUDIO_TAG, "Stopping audio playback");
    M5.Speaker.stop();
    M5.Speaker.end();
    is_playing = false;
    pending_stop_playback = false;

    // Restart mic — Speaker.playRaw on Core2 can disrupt the mic I2S bus
    M5.Mic.end();
    delay(10);
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.over_sampling = 1;
    mic_cfg.magnification = 16;
    mic_cfg.use_adc = false;
    M5.Mic.config(mic_cfg);
    if (M5.Mic.begin())
    {
        LOG_INFO(AUDIO_TAG, "Mic restarted successfully after playback");
    }
    else
    {
        LOG_ERROR(AUDIO_TAG, "Failed to restart mic after playback!");
    }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        LOG_ERROR(WS_TAG, "WebSocket Disconnected - heap: %u bytes", ESP.getFreeHeap());
        websocket_connected = false;
        in_call = false;
        is_playing = false;
        M5.Speaker.stop();
        set_state(STATE_ERROR);
        break;
    case WStype_ERROR:
        LOG_ERROR(WS_TAG, "WebSocket Error: %s, heap: %u bytes", payload, ESP.getFreeHeap());
        websocket_connected = false;
        in_call = false;
        set_state(STATE_ERROR);
        break;
    case WStype_CONNECTED:
        LOG_INFO(WS_TAG, "WebSocket Connected to: %s", payload);
        websocket_connected = true;
        webSocket.sendTXT("{\"type\":\"hello\",\"protocol_version\":1}");
        set_state(STATE_READY);
        break;
    case WStype_TEXT:
        LOG_INFO(WS_TAG, "Received text: %s", payload);
        handle_server_message((const char *)payload);
        break;
    case WStype_BIN:
    {
        static uint32_t audio_frame_count = 0;
        static uint32_t audio_total_bytes = 0;
        audio_frame_count++;
        audio_total_bytes += length;
        // Log every 10th frame to avoid serial flooding
        if (audio_frame_count % 10 == 1)
        {
            LOG_INFO(WS_TAG, "Audio frame #%u: %u bytes (total: %u bytes, heap: %u)",
                     audio_frame_count, length, audio_total_bytes, ESP.getFreeHeap());
        }
        if (in_call && is_playing && length >= 2)
        {
            // Triple-buffered streaming playback (proven working approach)
            size_t data_remaining = length;
            size_t data_pos = 0;
            static size_t buf_idx = 0;

            while (data_remaining > 0)
            {
                size_t chunk_len = (data_remaining < PLAY_BUF_SIZE) ? data_remaining : PLAY_BUF_SIZE;
                memcpy(play_buffers[buf_idx], payload + data_pos, chunk_len);
                M5.Speaker.playRaw((const int16_t *)play_buffers[buf_idx], chunk_len >> 1, audio_sample_rate, false, 1, 0);
                data_remaining -= chunk_len;
                data_pos += chunk_len;
                buf_idx = (buf_idx < (PLAY_BUF_NUM - 1)) ? buf_idx + 1 : 0;
            }
        }
        break;
    }
    default:
        break;
    }
}

void init_websocket()
{
    LOG_INFO(WS_TAG, "Initializing WebSocket connection...");
    LOG_INFO(WS_TAG, "Connecting to: wss://%s:%d%s", WS_HOST, WS_PORT, WS_PATH);
    set_state(STATE_CONNECTING_SERVER);

    webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
}

void init_audio()
{
    LOG_INFO(AUDIO_TAG, "Initializing audio system...");

    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = SAMPLE_RATE;
    mic_cfg.over_sampling = 1;
    mic_cfg.magnification = 16;
    mic_cfg.use_adc = false;
    M5.Mic.config(mic_cfg);

    if (M5.Mic.begin())
    {
        LOG_INFO(AUDIO_TAG, "M5 Microphone started successfully");
    }
    else
    {
        LOG_ERROR(AUDIO_TAG, "Failed to start M5 microphone");
    }

    M5.Speaker.setVolume(255);
    M5.Speaker.setChannelVolume(0, 255);

    LOG_INFO(AUDIO_TAG, "Audio system initialized");
}

void setup()
{
    Serial.begin(115200);
    delay(1000);

    LOG_INFO(TAG, "=== M5Stack Core2 Voice Assistant ===");
    LOG_INFO(TAG, "Initial heap: %u bytes", ESP.getFreeHeap());

    set_state(STATE_BOOT);

    M5.begin();
    LOG_INFO(TAG, "M5Stack initialized, heap: %u bytes", ESP.getFreeHeap());

    init_audio();
    LOG_INFO(TAG, "Audio initialized, heap: %u bytes", ESP.getFreeHeap());

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
    webSocket.loop();

    // Deferred playback stop — runs outside WebSocket callback context
    if (pending_stop_playback)
    {
        stop_playback();
    }

    handle_touch();

    if (in_call && !is_playing && M5.Mic.isEnabled())
    {
        int16_t mic_buf[MIC_BUFFER_SIZE];
        if (M5.Mic.record(mic_buf, MIC_BUFFER_SIZE, SAMPLE_RATE))
        {
            webSocket.sendBIN((uint8_t *)mic_buf, MIC_BUFFER_SIZE * sizeof(int16_t));
        }
    }


    static unsigned long lastPulse = 0;
    static bool pulseState = false;
    if (in_call && millis() - lastPulse > 500)
    {
        pulseState = !pulseState;
        uint16_t color = pulseState ? TFT_GREEN : TFT_DARKGREEN;
        M5.Display.fillCircle(M5.Display.width() - 20, 20, 8, color);
        lastPulse = millis();
    }

    delay(5);
}

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <memory>
#include <vector>

// #define WIFI_SSID "WIFI_NETWORK" // Replace with your WiFi SSID
// #define WIFI_PASS "PASSWORD1234" // Replace with your WiFi credentials

#define WIFI_MAXIMUM_RETRY 5

// WiFi status tracking
bool wifi_connected = false;

// Audio configuration
#define SAMPLE_RATE 16000
#define I2S_NUM (i2s_port_t)0
#define BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define CHANNEL_NUM 1
#define BUFFER_SIZE 1024
#define RECORDING_DURATION_SEC 10 // Longer for streaming

// M5Stack Core2 AXP192 Power Management I2C Configuration
#define AXP192_I2C_ADDR 0x34
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_1
#define I2C_MASTER_FREQ_HZ 400000
#define STREAM_CHUNK_SIZE 4096 // Send chunks of this size

// Voice Activity Detection with adaptive threshold
#define VAD_BASE_THRESHOLD 600 // Lower base threshold for better detection
#define VAD_MIN_CONSECUTIVE 2  // Require 2 consecutive detections
#define VAD_NOISE_SAMPLES 50   // Fewer samples to learn noise floor faster
#define SILENCE_TIMEOUT_MS 2000

// Keyword Detection Configuration
#define KEYWORD_BUFFER_SIZE 32000		// 2 seconds of audio at 16kHz
#define KEYWORD_MIN_LENGTH_MS 800		// Minimum keyword length (0.8 seconds)
#define KEYWORD_MAX_LENGTH_MS 2000		// Maximum keyword length (2 seconds)
#define KEYWORD_DETECTION_THRESHOLD 0.3 // Lower threshold for initial testing
#define KEYWORD_TIMEOUT_MS 5000			// Timeout waiting for keyword
#define NUM_MFCC_FEATURES 13			// Number of MFCC coefficients

// Console equalizer configuration (for debugging)
#define EQ_BARS 20
#define EQ_MAX_HEIGHT 50

#define WORKER_URL "https://your-worker.your-subdomain.workers.dev/audio" // Production

// Color definitions for M5GFX (RGB565 format)
#define COLOR_BLACK TFT_BLACK
#define COLOR_WHITE TFT_WHITE
#define COLOR_RED TFT_RED
#define COLOR_GREEN TFT_GREEN
#define COLOR_BLUE TFT_BLUE
#define COLOR_YELLOW TFT_YELLOW
#define COLOR_CYAN TFT_CYAN
#define COLOR_MAGENTA TFT_MAGENTA

// Arduino framework already provides ESP_LOG* macros

static const char *TAG = "voice_assistant";
static const char *AUDIO_TAG = "audio";
static const char *HTTP_TAG = "http_client";
static const char *LCD_TAG = "lcd";
static int s_retry_num = 0;

// Function declarations
void show_wifi_status(bool connected);
void show_voice_status(const char *status, uint16_t color);

// C++ AudioData class with RAII
class AudioData
{
private:
	std::unique_ptr<uint8_t[]> audio_data_;
	size_t data_len_;

public:
	// Constructor
	AudioData(size_t len = 0) : data_len_(len)
	{
		if (len > 0)
		{
			audio_data_.reset(new uint8_t[len]);
		}
	}

	// Copy constructor
	AudioData(const AudioData &other) : data_len_(other.data_len_)
	{
		if (data_len_ > 0)
		{
			audio_data_.reset(new uint8_t[data_len_]);
			memcpy(audio_data_.get(), other.audio_data_.get(), data_len_);
		}
	}

	// Move constructor
	AudioData(AudioData &&other) noexcept
		: audio_data_(std::move(other.audio_data_)), data_len_(other.data_len_)
	{
		other.data_len_ = 0;
	}

	// Assignment operators
	AudioData &operator=(const AudioData &other)
	{
		if (this != &other)
		{
			data_len_ = other.data_len_;
			if (data_len_ > 0)
			{
				audio_data_.reset(new uint8_t[data_len_]);
				memcpy(audio_data_.get(), other.audio_data_.get(), data_len_);
			}
			else
			{
				audio_data_.reset();
			}
		}
		return *this;
	}

	AudioData &operator=(AudioData &&other) noexcept
	{
		if (this != &other)
		{
			audio_data_ = std::move(other.audio_data_);
			data_len_ = other.data_len_;
			other.data_len_ = 0;
		}
		return *this;
	}

	// Accessors
	uint8_t *data() { return audio_data_.get(); }
	const uint8_t *data() const { return audio_data_.get(); }
	size_t size() const { return data_len_; }

	// Resize functionality
	void resize(size_t new_len)
	{
		if (new_len != data_len_)
		{
			if (new_len > 0)
			{
				std::unique_ptr<uint8_t[]> new_data(new uint8_t[new_len]);
				if (audio_data_ && data_len_ > 0)
				{
					memcpy(new_data.get(), audio_data_.get(),
								min(data_len_, new_len));
				}
				audio_data_ = std::move(new_data);
			}
			else
			{
				audio_data_.reset();
			}
			data_len_ = new_len;
		}
	}

	// Check if empty
	bool empty() const { return data_len_ == 0 || !audio_data_; }
};

// Keyword detection states
typedef enum
{
	KEYWORD_STATE_LISTENING, // Waiting for keyword
	KEYWORD_STATE_DETECTING, // Processing potential keyword
	KEYWORD_STATE_CONFIRMED, // Keyword confirmed, ready for command
	KEYWORD_STATE_TIMEOUT	 // Keyword detection timeout
} keyword_state_t;

// C++ KeywordDetector class with RAII and better encapsulation
class KeywordDetector
{
private:
	std::unique_ptr<int16_t[]> audio_buffer_; // RAII managed circular buffer
	size_t buffer_size_;					  // Size of the buffer
	size_t buffer_pos_;						  // Current position in buffer
	size_t samples_collected_;				  // Total samples collected
	keyword_state_t state_;					  // Current detection state
	TickType_t detection_start_;			  // When keyword detection started
	float confidence_;						  // Detection confidence score
	bool keyword_detected_;					  // Final detection result

public:
	// Constructor
	KeywordDetector(size_t buffer_size = KEYWORD_BUFFER_SIZE)
		: buffer_size_(buffer_size), buffer_pos_(0), samples_collected_(0),
		  state_(KEYWORD_STATE_LISTENING), detection_start_(0),
		  confidence_(0.0f), keyword_detected_(false)
	{

		// Try to allocate in PSRAM first, fall back to regular heap
		audio_buffer_.reset(static_cast<int16_t *>(
			heap_caps_malloc(buffer_size_ * sizeof(int16_t),
							 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));

		if (!audio_buffer_)
		{
			audio_buffer_.reset(new int16_t[buffer_size_]);
		}

		if (!audio_buffer_)
		{
			ESP_LOGE(AUDIO_TAG, "Failed to allocate keyword detector buffer");
			buffer_size_ = 0;
		}
	}

	// Destructor handled automatically by unique_ptr
	~KeywordDetector() = default;

	// Non-copyable but movable
	KeywordDetector(const KeywordDetector &) = delete;
	KeywordDetector &operator=(const KeywordDetector &) = delete;

	KeywordDetector(KeywordDetector &&other) noexcept
		: audio_buffer_(std::move(other.audio_buffer_)),
		  buffer_size_(other.buffer_size_),
		  buffer_pos_(other.buffer_pos_),
		  samples_collected_(other.samples_collected_),
		  state_(other.state_),
		  detection_start_(other.detection_start_),
		  confidence_(other.confidence_),
		  keyword_detected_(other.keyword_detected_)
	{
		other.buffer_size_ = 0;
		other.reset();
	}

	KeywordDetector &operator=(KeywordDetector &&other) noexcept
	{
		if (this != &other)
		{
			audio_buffer_ = std::move(other.audio_buffer_);
			buffer_size_ = other.buffer_size_;
			buffer_pos_ = other.buffer_pos_;
			samples_collected_ = other.samples_collected_;
			state_ = other.state_;
			detection_start_ = other.detection_start_;
			confidence_ = other.confidence_;
			keyword_detected_ = other.keyword_detected_;

			other.buffer_size_ = 0;
			other.reset();
		}
		return *this;
	}

	// Public interface methods
	void addSamples(int16_t *samples, size_t count)
	{
		if (!samples || !audio_buffer_ || buffer_size_ == 0)
			return;

		for (size_t i = 0; i < count; i++)
		{
			audio_buffer_[buffer_pos_] = samples[i];
			buffer_pos_ = (buffer_pos_ + 1) % buffer_size_;

			if (samples_collected_ < buffer_size_)
			{
				samples_collected_++;
			}
		}
	}

	bool processDetection(int16_t *audio_samples, size_t sample_count)
	{
		if (!audio_buffer_)
			return false;

		addSamples(audio_samples, sample_count);

		TickType_t current_time = xTaskGetTickCount();

		switch (state_)
		{
		case KEYWORD_STATE_LISTENING:
			if (detectKeywordPattern())
			{
				state_ = KEYWORD_STATE_DETECTING;
				detection_start_ = current_time;
				ESP_LOGI(AUDIO_TAG, "🎯 Potential keyword detected, verifying...");
			}
			break;

		case KEYWORD_STATE_DETECTING:
			if (detectKeywordPattern())
			{
				state_ = KEYWORD_STATE_CONFIRMED;
				keyword_detected_ = true;
				ESP_LOGI(AUDIO_TAG, "✅ KEYWORD CONFIRMED! Ready for command (confidence: %.3f)", confidence_);
				return true;
			}

			if ((current_time - detection_start_) > pdMS_TO_TICKS(1000))
			{
				state_ = KEYWORD_STATE_LISTENING;
				ESP_LOGI(AUDIO_TAG, "⏰ Keyword verification timeout, back to listening");
			}
			break;

		case KEYWORD_STATE_CONFIRMED:
			// Keyword was detected, waiting for reset
			break;

		case KEYWORD_STATE_TIMEOUT:
			state_ = KEYWORD_STATE_LISTENING;
			break;
		}

		return false;
	}

	void reset()
	{
		state_ = KEYWORD_STATE_LISTENING;
		keyword_detected_ = false;
		confidence_ = 0.0f;
		buffer_pos_ = 0;
		samples_collected_ = 0;
		ESP_LOGI(AUDIO_TAG, "🔄 Keyword detector reset to listening state");
	}

	// Accessors
	keyword_state_t getState() const { return state_; }
	float getConfidence() const { return confidence_; }
	bool isKeywordDetected() const { return keyword_detected_; }
	size_t getSamplesCollected() const { return samples_collected_; }
	bool isValid() const { return audio_buffer_ != nullptr && buffer_size_ > 0; }

private:
	bool detectKeywordPattern()
	{
		if (samples_collected_ < (SAMPLE_RATE * KEYWORD_MIN_LENGTH_MS / 1000))
		{
			return false; // Not enough samples
		}

		// Get recent audio samples for analysis
		size_t samples_to_analyze = SAMPLE_RATE * 1; // Analyze 1 second of audio
		if (samples_to_analyze > samples_collected_)
		{
			samples_to_analyze = samples_collected_;
		}

		// Get samples from circular buffer using RAII
		std::unique_ptr<int16_t[]> analysis_buffer(new int16_t[samples_to_analyze]);
		if (!analysis_buffer)
			return false;

		size_t start_pos = (buffer_pos_ + buffer_size_ - samples_to_analyze) % buffer_size_;
		for (size_t i = 0; i < samples_to_analyze; i++)
		{
			analysis_buffer[i] = audio_buffer_[(start_pos + i) % buffer_size_];
		}

		// Simple keyword detection: look for sustained voice activity pattern
		const size_t num_segments = 10;
		size_t samples_per_segment = samples_to_analyze / num_segments;
		int active_segments = 0;
		float total_energy = 0;
		float segment_energies[10]; // Store individual segment energies

		for (size_t seg = 0; seg < num_segments; seg++)
		{
			float segment_energy = 0;
			size_t start_idx = seg * samples_per_segment;
			size_t end_idx = (seg + 1) * samples_per_segment;

			for (size_t i = start_idx; i < end_idx && i < samples_to_analyze; i++)
			{
				float sample = (float)abs(analysis_buffer[i]) / 32768.0f;
				segment_energy += sample * sample;
			}

			segment_energy = segment_energy / samples_per_segment;
			segment_energies[seg] = segment_energy;
			total_energy += segment_energy;

			if (segment_energy > 0.001f)
			{ // Lowered threshold
				active_segments++;
			}
		}

		float avg_energy = total_energy / num_segments;

		float energy_variation = 0.0f;
		if (active_segments > 0)
		{
			for (int i = 0; i < num_segments; i++)
			{
				float segment_energy = segment_energies[i];
				energy_variation += fabsf(segment_energy - avg_energy);
			}
			energy_variation /= num_segments;
		}

		// LOWERED THRESHOLDS: Much more permissive detection for "Hey El"
		bool has_sustained_speech = (active_segments >= 2);
		bool has_sufficient_energy = (avg_energy > 0.0005f);
		bool has_variation = (energy_variation > 0.00005f);
		bool reasonable_duration = (samples_collected_ >= buffer_size_ * 0.1f);

		bool keyword_detected_full = has_sustained_speech && has_sufficient_energy && has_variation && reasonable_duration;
		bool fallback_detection = (active_segments >= 1) && (avg_energy > 0.0003f) && (samples_collected_ >= buffer_size_ * 0.05f);
		bool keyword_detected = keyword_detected_full || fallback_detection;

		confidence_ = avg_energy;

		// Enhanced debug logging
		static int debug_counter = 0;
		if (++debug_counter % 50 == 0 || keyword_detected || has_sustained_speech)
		{
			ESP_LOGI(AUDIO_TAG, "🔍 'Hey El': Segs=%d/10(≥2), Energy=%.4f(>0.0005), Var=%.5f(>0.00005), Dur=%d%%(≥10%%), Det=%s [%s|%s|%s|%s]",
					 active_segments, avg_energy, energy_variation,
					 (int)((samples_collected_ * 100) / buffer_size_),
					 keyword_detected ? "YES" : "NO",
					 has_sustained_speech ? "S" : "s",
					 has_sufficient_energy ? "E" : "e",
					 has_variation ? "V" : "v",
					 reasonable_duration ? "D" : "d");
		}

		return keyword_detected;
	}
};

// Arduino-style WiFi connection function
void connect_wifi() {
	WiFi.begin(WIFI_SSID, WIFI_PASS);
	ESP_LOGI(TAG, "Connecting to WiFi...");
	show_wifi_status(false);

	int retry_count = 0;
	while (WiFi.status() != WL_CONNECTED && retry_count < WIFI_MAXIMUM_RETRY) {
		delay(1000);
		ESP_LOGI(TAG, "WiFi connection attempt %d/%d", retry_count + 1, WIFI_MAXIMUM_RETRY);
		retry_count++;
	}

	if (WiFi.status() == WL_CONNECTED) {
		wifi_connected = true;
		ESP_LOGI(TAG, "WiFi connected! IP: %s", WiFi.localIP().toString().c_str());
		show_wifi_status(true);
	} else {
		wifi_connected = false;
		ESP_LOGE(TAG, "WiFi connection failed");
		show_wifi_status(false);
	}
}

// Manual display functions removed - replaced with M5Unified/M5GFX

// Draw a simple power-on indicator using M5.Display
void show_power_on_indicator(void)
{
	ESP_LOGI(LCD_TAG, "Displaying power-on indicator using M5.Display");

	// Clear screen with black background
	M5.Display.fillScreen(COLOR_BLACK);

	// Display welcome message
	M5.Display.setTextColor(COLOR_WHITE);
	M5.Display.setTextSize(2);
	M5.Display.setCursor(60, 100);
	M5.Display.print("M5Stack Core2");

	// Draw a green power indicator in the center
	int center_x = M5.Display.width() / 2;
	int center_y = M5.Display.height() / 2;

	M5.Display.fillCircle(center_x, center_y + 40, 20, COLOR_GREEN);

	// Add "READY" text
	M5.Display.setTextColor(COLOR_GREEN);
	M5.Display.setTextSize(1);
	M5.Display.setCursor(center_x - 20, center_y + 70);
	M5.Display.print("READY");

	ESP_LOGI(LCD_TAG, "Power-on indicator displayed using M5.Display");
}

// Show WiFi status on display using M5.Display
void show_wifi_status(bool connected)
{
	ESP_LOGI(LCD_TAG, "Updating WiFi status: %s", connected ? "Connected" : "Connecting...");

	// WiFi indicator position (top-right corner)
	int wifi_x = M5.Display.width() - 80;
	int wifi_y = 10;

	// Clear the WiFi status area
	M5.Display.fillRect(wifi_x, wifi_y, 70, 25, COLOR_BLACK);

	if (connected)
	{
		// Draw connected indicator (green WiFi text)
		M5.Display.setTextColor(COLOR_GREEN);
		M5.Display.setTextSize(1);
		M5.Display.setCursor(wifi_x, wifi_y);
		M5.Display.print("WiFi OK");

		// Draw simple signal bars
		M5.Display.fillRect(wifi_x + 50, wifi_y + 18, 2, 4, COLOR_GREEN);
		M5.Display.fillRect(wifi_x + 53, wifi_y + 15, 2, 7, COLOR_GREEN);
		M5.Display.fillRect(wifi_x + 56, wifi_y + 12, 2, 10, COLOR_GREEN);
		M5.Display.fillRect(wifi_x + 59, wifi_y + 9, 2, 13, COLOR_GREEN);
	}
	else
	{
		// Draw connecting indicator
		M5.Display.setTextColor(COLOR_YELLOW);
		M5.Display.setTextSize(1);
		M5.Display.setCursor(wifi_x, wifi_y);
		M5.Display.print("WiFi...");
	}
}

// Show voice assistant status using M5.Display
void show_voice_status(const char *status, uint16_t color)
{
	ESP_LOGI(LCD_TAG, "Updating voice status: %s", status);

	// Status indicator position (bottom center)
	int status_x = M5.Display.width() / 2 - 60;
	int status_y = M5.Display.height() - 40;

	// Clear the status area
	M5.Display.fillRect(status_x, status_y, 120, 30, COLOR_BLACK);

	// Convert color parameter to M5 color (simple mapping)
	uint16_t m5_color = COLOR_WHITE; // Default
	if (color == 0xF800)
		m5_color = COLOR_RED;
	else if (color == 0x07E0)
		m5_color = COLOR_GREEN;
	else if (color == 0x001F)
		m5_color = COLOR_BLUE;
	else if (color == 0xFFE0)
		m5_color = COLOR_YELLOW;

	// Draw status text
	M5.Display.setTextColor(m5_color);
	M5.Display.setTextSize(1);
	M5.Display.setCursor(status_x, status_y + 10);
	M5.Display.print(status);

	// Draw simple visual indicator
	if (strcmp(status, "LISTENING") == 0)
	{
		M5.Display.drawCircle(status_x - 15, status_y + 15, 8, m5_color);
	}
	else if (strcmp(status, "PROCESSING") == 0)
	{
		// Draw processing dots
		for (int i = 0; i < 3; i++)
		{
			M5.Display.fillCircle(status_x - 20 + (i * 8), status_y + 15, 2, m5_color);
		}
	}
	else if (strcmp(status, "RESPONDING") == 0)
	{
		M5.Display.fillTriangle(status_x - 15, status_y + 10, status_x - 15, status_y + 20, status_x - 8, status_y + 15, m5_color);
	}
}

// Audio initialization for PDM microphone (recording)
esp_err_t init_microphone(void)
{
	i2s_config_t i2s_config = {};
	i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
	i2s_config.sample_rate = SAMPLE_RATE;
	i2s_config.bits_per_sample = BITS_PER_SAMPLE;
	i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
	i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
	i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
	i2s_config.dma_buf_count = 8;
	i2s_config.dma_buf_len = BUFFER_SIZE;
	i2s_config.use_apll = false;
	i2s_config.tx_desc_auto_clear = false;
	i2s_config.fixed_mclk = 0;

	// M5Stack Core2 PDM microphone pins (typical configuration)
	i2s_pin_config_t pin_config = {};
	pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
	pin_config.ws_io_num = GPIO_NUM_0; // PDM CLK
	pin_config.data_out_num = I2S_PIN_NO_CHANGE;
	pin_config.data_in_num = GPIO_NUM_34; // PDM DATA

	esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to install I2S driver");
		return ret;
	}

	ret = i2s_set_pin(I2S_NUM, &pin_config);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to set I2S pins");
		return ret;
	}

	ESP_LOGI(AUDIO_TAG, "Microphone initialized successfully");
	return ESP_OK;
}

// Speaker, I2C and AXP192 are now handled by M5Unified library

// Play a ready beep sound with enhanced M5Stack Core2 support
void play_ready_beep(void)
{
	ESP_LOGI(AUDIO_TAG, "🔊 Playing M5Stack Core2 ready beep...");

	// Set speaker volume
	M5.Speaker.setVolume(100); // Set volume to maximum

	// Play a pleasant ascending tone sequence
	M5.Speaker.tone(784, 100); // G5 - first note
	// Wait for tone to finish playing
	while (M5.Speaker.isPlaying())
	{
		delay(1);
	}
	delay(20);

	M5.Speaker.tone(988, 100); // B5 - second note
	// Wait for tone to finish playing
	while (M5.Speaker.isPlaying())
	{
		delay(1);
	}
	delay(20);

	M5.Speaker.tone(1175, 200); // D6 - final note, slightly longer
	// Wait for tone to finish playing
	while (M5.Speaker.isPlaying())
	{
		delay(1);
	}

	ESP_LOGI(AUDIO_TAG, "✅ Beep sequence played using M5.Speaker");
	M5.Speaker.stop(); // Stop any ongoing sound
	M5.Speaker.end();  // End speaker session
}

// Initialize system with display
void init_system(void)
{
	ESP_LOGI(TAG, "=================================");
	ESP_LOGI(TAG, "    M5Stack Core2 Voice Assistant");
	ESP_LOGI(TAG, "=================================");

	// Initialize M5Stack Core2 hardware using M5Unified
	ESP_LOGI(TAG, "Initializing M5Stack Core2 hardware...");
	auto cfg = M5.config();
	M5.begin(cfg);

	// Show power-on indicator immediately
	show_power_on_indicator();
	ESP_LOGI(TAG, "M5Stack Core2 initialized and power indicator shown");

	// Initialize WiFi status indicator (initially disconnected)
	show_wifi_status(false);

	ESP_LOGI(TAG, "Connecting to WiFi...");
	play_ready_beep(); // Play ready beep sound
}

// Display console-based equalizer
void update_console_equalizer(int16_t *audio_buffer, size_t samples)
{
	static int display_counter = 0;

	// Update equalizer every 20 calls to avoid spam
	if (++display_counter % 20 != 0)
		return;

	// Calculate energy for each frequency band (simplified)
	int samples_per_bar = samples / EQ_BARS;
	String equalizer_line;
	equalizer_line.reserve(EQ_BARS);

	for (int bar = 0; bar < EQ_BARS; bar++)
	{
		int64_t bar_energy = 0;
		int start_idx = bar * samples_per_bar;
		int end_idx = (bar + 1) * samples_per_bar;

		if (end_idx > samples)
			end_idx = samples;

		for (int i = start_idx; i < end_idx; i++)
		{
			bar_energy += abs(audio_buffer[i]);
		}

		// Normalize to console height (0-5)
		int bar_height = (bar_energy / samples_per_bar) / 1000;
		if (bar_height > 5)
			bar_height = 5;

		// Create visual bar
		if (bar_height >= 4)
			equalizer_line += '#'; // High
		else if (bar_height >= 2)
			equalizer_line += '|'; // Medium
		else if (bar_height >= 1)
			equalizer_line += '.'; // Low
		else
			equalizer_line += ' '; // Silent
	}

	// Calculate total audio level
	int64_t total_energy = 0;
	for (size_t i = 0; i < samples; i++)
	{
		total_energy += abs(audio_buffer[i]);
	}
	int avg_level = total_energy / samples;

	// Display console equalizer and status
	ESP_LOGI(AUDIO_TAG, "EQ: [%s] Level: %d %s",
			 equalizer_line.c_str(),
			 avg_level,
			 "👂 Listening...");
}

// Simple MFCC-like feature extraction for keyword detection
void extract_audio_features(int16_t *audio_buffer, size_t samples, float *features, size_t num_features)
{
	// Initialize features
	for (size_t i = 0; i < num_features; i++)
	{
		features[i] = 0.0f;
	}

	if (samples == 0)
		return;

	// Simple energy-based features (simplified MFCC alternative)
	size_t samples_per_feature = samples / num_features;
	if (samples_per_feature == 0)
		samples_per_feature = 1;

	for (size_t feat = 0; feat < num_features; feat++)
	{
		float energy = 0.0f;
		size_t start_idx = feat * samples_per_feature;
		size_t end_idx = (feat + 1) * samples_per_feature;
		if (end_idx > samples)
			end_idx = samples;

		// Calculate energy in this frequency band
		for (size_t i = start_idx; i < end_idx; i++)
		{
			float sample = (float)audio_buffer[i] / 32768.0f; // Normalize to [-1, 1]
			energy += sample * sample;
		}

		// Convert to log scale (like MFCC)
		energy = energy / (end_idx - start_idx);
		features[feat] = logf(energy + 1e-10f); // Add small value to avoid log(0)
	}
}

// Calculate similarity between two feature vectors
float calculate_feature_similarity(float *features1, float *features2, size_t num_features)
{
	float similarity = 0.0f;
	float norm1 = 0.0f, norm2 = 0.0f;

	// Calculate dot product and norms
	for (size_t i = 0; i < num_features; i++)
	{
		similarity += features1[i] * features2[i];
		norm1 += features1[i] * features1[i];
		norm2 += features2[i] * features2[i];
	}

	// Normalize (cosine similarity)
	float magnitude = sqrtf(norm1) * sqrtf(norm2);
	if (magnitude > 0.0f)
	{
		similarity = similarity / magnitude;
	}

	return similarity;
}

// Adaptive voice activity detection with noise floor learning
bool detect_voice_activity(int16_t *audio_buffer, size_t samples)
{
	// Update console equalizer
	update_console_equalizer(audio_buffer, samples);

	// Calculate average energy and max amplitude
	int64_t energy = 0;
	int32_t max_amplitude = 0;

	for (size_t i = 0; i < samples; i++)
	{
		int16_t sample = abs(audio_buffer[i]);
		energy += sample;
		if (sample > max_amplitude)
		{
			max_amplitude = sample;
		}
	}

	int32_t avg_energy = energy / samples;

	// Adaptive noise floor calculation
	static int32_t noise_floor = 0;
	static int32_t noise_samples_count = 0;
	static int32_t noise_sum = 0;

	// Learn noise floor during first VAD_NOISE_SAMPLES
	if (noise_samples_count < VAD_NOISE_SAMPLES)
	{
		noise_sum += avg_energy;
		noise_samples_count++;
		if (noise_samples_count == VAD_NOISE_SAMPLES)
		{
			noise_floor = noise_sum / VAD_NOISE_SAMPLES;
			ESP_LOGI(AUDIO_TAG, "🎯 Noise floor learned: %d", noise_floor);
		}
		return false; // Don't detect voice while learning
	}

	// Adaptive threshold = noise floor + margin
	int32_t adaptive_threshold = noise_floor + VAD_BASE_THRESHOLD;
	int32_t max_threshold = adaptive_threshold / 2;

	// Simplified voice detection - less restrictive
	bool energy_check = avg_energy > adaptive_threshold;
	bool amplitude_check = max_amplitude > max_threshold;

	// Use OR instead of AND for one of the conditions to be more permissive
	bool voice_detected = energy_check || (amplitude_check && (max_amplitude > avg_energy * 1.1));

	// Log every 15 calls for more frequent debugging
	static int log_counter = 0;
	if (++log_counter % 15 == 0)
	{
		ESP_LOGI(AUDIO_TAG, "DEBUG - Avg:%d Max:%d | Noise:%d AdaptThresh:%d MaxThresh:%d | ECheck:%s ACheck:%s Voice:%s",
				 avg_energy, max_amplitude, noise_floor, adaptive_threshold, max_threshold,
				 energy_check ? "Y" : "N", amplitude_check ? "Y" : "N", voice_detected ? "YES" : "NO");
	}

	// Debug voice detection result
	if (voice_detected)
	{
		ESP_LOGI(AUDIO_TAG, "✅ VOICE! Avg:%d>%d Max:%d>%d Ratio:%.1f",
				 avg_energy, adaptive_threshold, max_amplitude, max_threshold,
				 (float)max_amplitude / avg_energy);
	}

	return voice_detected;
}

// Old C-style function removed - replaced by KeywordDetector constructor

// Old C-style functions removed - replaced by KeywordDetector C++ class methods

// Arduino-style HTTP client for sending audio data
bool stream_audio_chunk(uint8_t *audio_data, size_t data_len, bool is_first_chunk, bool is_last_chunk)
{
	if (!wifi_connected) {
		ESP_LOGE(HTTP_TAG, "WiFi not connected");
		return false;
	}

	HTTPClient http;
	http.begin(WORKER_URL);

	// Set headers
	http.addHeader("Content-Type", "audio/pcm");
	http.addHeader("User-Agent", "M5Stack-Core2-Assistant");
	http.addHeader("X-Audio-Sample-Rate", "16000");
	http.addHeader("X-Audio-Channels", "1");
	http.addHeader("X-Audio-Bits-Per-Sample", "16");

	// Add streaming headers
	if (is_first_chunk) {
		http.addHeader("X-Stream-Start", "true");
	}
	if (is_last_chunk) {
		http.addHeader("X-Stream-End", "true");
	}

	// Send POST request
	int httpResponseCode = http.POST(audio_data, data_len);

	if (httpResponseCode > 0) {
		ESP_LOGI(HTTP_TAG, "Streamed %zu bytes, Status = %d", data_len, httpResponseCode);
		String response = http.getString();
		ESP_LOGI(HTTP_TAG, "Response: %s", response.c_str());
	} else {
		ESP_LOGE(HTTP_TAG, "HTTP request failed: %d", httpResponseCode);
	}

	http.end();
	return httpResponseCode > 0;
}

// Send complete audio data to Cloudflare Worker (non-streaming fallback)
bool send_audio_to_worker(uint8_t *audio_data, size_t data_len)
{
	return stream_audio_chunk(audio_data, data_len, true, true);
}

// Record and stream audio in real-time
void record_and_stream_audio(void)
{
	ESP_LOGI(AUDIO_TAG, "Starting real-time audio streaming...");
	show_voice_status("RESPONDING", COLOR_GREEN);

	// Check available heap memory
	size_t free_heap = esp_get_free_heap_size();
	ESP_LOGI(AUDIO_TAG, "Free heap before recording: %d bytes", free_heap);

	// Uninstall any existing I2S driver first
	esp_err_t uninstall_err = i2s_driver_uninstall(I2S_NUM);
	if (uninstall_err == ESP_OK)
	{
		ESP_LOGI(AUDIO_TAG, "Uninstalled existing I2S driver");
	}

	if (init_microphone() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize microphone");
		return;
	}

	// Smaller buffers for streaming using RAII
	std::unique_ptr<int16_t[]> temp_buffer(new int16_t[BUFFER_SIZE]);
	std::unique_ptr<uint8_t[]> stream_buffer(new uint8_t[STREAM_CHUNK_SIZE]);

	if (!temp_buffer || !stream_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate memory for streaming. Free heap: %d", esp_get_free_heap_size());
		i2s_driver_uninstall(I2S_NUM);
		return;
	}

	ESP_LOGI(AUDIO_TAG, "Memory allocated successfully. Free heap: %d bytes", esp_get_free_heap_size());

	size_t bytes_read = 0;
	size_t stream_buffer_pos = 0;
	size_t total_streamed = 0;
	bool recording_started = false;
	bool is_first_chunk = true;
	TickType_t last_voice_time = 0;
	TickType_t recording_start_time = 0;
	int consecutive_voice_count = 0; // Track consecutive voice detections

	ESP_LOGI(AUDIO_TAG, "Listening for voice... (base threshold: %d)", VAD_BASE_THRESHOLD);

	while (1)
	{
		esp_err_t ret = i2s_read(I2S_NUM, temp_buffer.get(), BUFFER_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);

		if (ret != ESP_OK)
		{
			ESP_LOGE(AUDIO_TAG, "I2S read error: %s", esp_err_to_name(ret));
			break;
		}

		if (bytes_read > 0)
		{
			bool voice_detected = detect_voice_activity(temp_buffer.get(), bytes_read / sizeof(int16_t));

			if (voice_detected)
			{
				consecutive_voice_count++;

				// Only start recording after consecutive voice detections
				if (!recording_started && consecutive_voice_count >= VAD_MIN_CONSECUTIVE)
				{
					ESP_LOGI(AUDIO_TAG, "🎙️ Strong voice signal detected! Starting real-time streaming...");

					recording_started = true;
					recording_start_time = xTaskGetTickCount();
					stream_buffer_pos = 0;
					total_streamed = 0;
				}

				if (recording_started)
				{
					last_voice_time = xTaskGetTickCount();
				}
			}
			else
			{
				// Reset consecutive count on silence
				consecutive_voice_count = 0;
			}

			if (recording_started)
			{
				// Add data to stream buffer
				size_t bytes_to_copy = bytes_read;
				if (stream_buffer_pos + bytes_to_copy > STREAM_CHUNK_SIZE)
				{
					bytes_to_copy = STREAM_CHUNK_SIZE - stream_buffer_pos;
				}

				memcpy(stream_buffer.get() + stream_buffer_pos, temp_buffer.get(), bytes_to_copy);
				stream_buffer_pos += bytes_to_copy;

				// Stream when buffer is full
				if (stream_buffer_pos >= STREAM_CHUNK_SIZE)
				{
					ESP_LOGI(AUDIO_TAG, "Streaming chunk %d bytes...", stream_buffer_pos);

					esp_err_t stream_err = stream_audio_chunk(stream_buffer.get(), stream_buffer_pos, is_first_chunk, false);
					if (stream_err == ESP_OK)
					{
						total_streamed += stream_buffer_pos;
						ESP_LOGI(AUDIO_TAG, "Successfully streamed chunk. Total: %d bytes", total_streamed);
					}
					else
					{
						ESP_LOGE(AUDIO_TAG, "Failed to stream chunk");
					}

					// Reset for next chunk
					stream_buffer_pos = 0;
					is_first_chunk = false;

					// Handle remaining data if any
					size_t remaining = bytes_read - bytes_to_copy;
					if (remaining > 0)
					{
						memcpy(stream_buffer.get(), (uint8_t *)temp_buffer.get() + bytes_to_copy, remaining);
						stream_buffer_pos = remaining;
					}
				}

				// Check for silence timeout
				if (!voice_detected && (xTaskGetTickCount() - last_voice_time) > pdMS_TO_TICKS(SILENCE_TIMEOUT_MS))
				{
					ESP_LOGI(AUDIO_TAG, "Silence detected. Ending streaming...");

					// Send final chunk if there's data
					if (stream_buffer_pos > 0)
					{
						ESP_LOGI(AUDIO_TAG, "Sending final chunk: %d bytes", stream_buffer_pos);
						stream_audio_chunk(stream_buffer.get(), stream_buffer_pos, false, true);
						total_streamed += stream_buffer_pos;
					}
					else if (!is_first_chunk)
					{
						// Send empty end marker
						stream_audio_chunk(NULL, 0, false, true);
					}

					ESP_LOGI(AUDIO_TAG, "Streaming completed. Total streamed: %d bytes", total_streamed);
					break;
				}

				// Maximum recording time check
				if ((xTaskGetTickCount() - recording_start_time) > pdMS_TO_TICKS(RECORDING_DURATION_SEC * 1000))
				{
					ESP_LOGI(AUDIO_TAG, "Maximum recording time reached. Ending streaming...");
					if (stream_buffer_pos > 0)
					{
						stream_audio_chunk(stream_buffer.get(), stream_buffer_pos, false, true);
					}
					break;
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(1)); // Minimal delay for streaming
	}

	// RAII: temp_buffer and stream_buffer automatically freed when going out of scope
	i2s_driver_uninstall(I2S_NUM);
}

// Play audio response (placeholder for when you receive audio back)
void play_audio_response(uint8_t *audio_data, size_t data_len)
{
	ESP_LOGI(AUDIO_TAG, "Playing audio response...");

	// Use M5.Speaker to play the audio
	M5.Speaker.playRaw(audio_data, data_len, SAMPLE_RATE, false);

	ESP_LOGI(AUDIO_TAG, "Audio playback completed. Data length: %zu", data_len);
}

// Enhanced voice assistant with keyword detection
void keyword_listening_loop(void)
{
	ESP_LOGI(AUDIO_TAG, "🎧 Starting keyword detection - Say 'Hey El'");

	// Initialize keyword detector using C++ class with RAII
	KeywordDetector detector;
	if (!detector.isValid())
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize keyword detector");
		return;
	}

	// Initialize microphone for keyword detection
	if (init_microphone() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize microphone for keyword detection");
		return; // RAII will handle cleanup automatically
	}

	// Use RAII for temp buffer too
	std::unique_ptr<int16_t[]> temp_buffer(new int16_t[BUFFER_SIZE]);
	if (!temp_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate temp buffer for keyword detection");
		i2s_driver_uninstall(I2S_NUM);
		return;
	}

	ESP_LOGI(AUDIO_TAG, "👂 Listening for keyword 'Hey El'...");
	show_voice_status("LISTENING", COLOR_CYAN);

	while (1)
	{
		size_t bytes_read = 0;
		esp_err_t ret = i2s_read(I2S_NUM, temp_buffer.get(), BUFFER_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);

		if (ret != ESP_OK)
		{
			ESP_LOGE(AUDIO_TAG, "I2S read error during keyword detection: %s", esp_err_to_name(ret));
			continue;
		}

		if (bytes_read > 0)
		{
			size_t sample_count = bytes_read / sizeof(int16_t);

			// Check for basic voice activity first to avoid unnecessary processing
			bool has_voice = detect_voice_activity(temp_buffer.get(), sample_count);

			if (has_voice)
			{
				// Process samples for keyword detection only when voice is present
				if (detector.processDetection(temp_buffer.get(), sample_count))
				{
					ESP_LOGI(AUDIO_TAG, "🎯 KEYWORD DETECTED! Switching to command mode...");
					show_voice_status("PROCESSING", COLOR_YELLOW);

					// Play confirmation beep
					i2s_driver_uninstall(I2S_NUM);
					play_ready_beep();

					// Switch to voice command recording
					record_and_stream_audio();

					// Reset keyword detector and continue listening
					detector.reset();

					// Reinitialize microphone for keyword detection
					if (init_microphone() != ESP_OK)
					{
						ESP_LOGE(AUDIO_TAG, "Failed to reinitialize microphone");
						break;
					}

					ESP_LOGI(AUDIO_TAG, "🔄 Back to keyword listening mode");
					show_voice_status("LISTENING", COLOR_CYAN);
				}

				// Shorter delay when voice is active
				vTaskDelay(pdMS_TO_TICKS(5));
			}
			else
			{
				// Longer delay when no voice activity to save CPU
				vTaskDelay(pdMS_TO_TICKS(50));
			}
		}
		else
		{
			// Small delay when no data received
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}

	// RAII handles cleanup automatically when function exits
	i2s_driver_uninstall(I2S_NUM);
}

// Voice assistant task with keyword detection
void voice_assistant_task(void *pvParameters)
{
	ESP_LOGI(TAG, "🚀 Starting Voice Assistant with Keyword Detection");
	ESP_LOGI(TAG, "💡 Say 'Hey El' to activate voice commands");

	// Enter continuous keyword listening loop
	keyword_listening_loop();
}

void setup() {
	// Initialize Serial communication
	Serial.begin(115200);
	delay(1000);

	ESP_LOGI(TAG, "=================================");
	ESP_LOGI(TAG, "    M5Stack Core2 Voice Assistant");
	ESP_LOGI(TAG, "=================================");

	// Initialize M5Stack Core2 hardware using M5Unified
	ESP_LOGI(TAG, "Initializing M5Stack Core2 hardware...");
	auto cfg = M5.config();
	M5.begin(cfg);

	// Show power-on indicator immediately
	show_power_on_indicator();
	ESP_LOGI(TAG, "M5Stack Core2 initialized and power indicator shown");

	// Connect to WiFi
	connect_wifi();

	if (wifi_connected) {
		ESP_LOGI(TAG, "✅ WiFi connection established. Starting voice assistant...");
		play_ready_beep(); // Play ready beep sound

		// Start voice assistant in main loop
		ESP_LOGI(TAG, "🚀 Starting Voice Assistant with Keyword Detection");
		ESP_LOGI(TAG, "💡 Say 'Hey El' to activate voice commands");
	}
}

void loop() {
	// Main voice assistant loop
	if (wifi_connected) {
		keyword_listening_loop();
	} else {
		// Try to reconnect WiFi if disconnected
		delay(5000);
		connect_wifi();
	}

	delay(100); // Small delay to prevent watchdog issues
}
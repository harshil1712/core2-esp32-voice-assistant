#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

// #define WIFI_SSID "WIFI_NETWORK" // Replace with your WiFi SSID
// #define WIFI_PASS "PASSWORD1234" // Replace with your WiFi credentials

#define WIFI_MAXIMUM_RETRY 5

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Audio configuration
#define SAMPLE_RATE 16000
#define I2S_NUM (0)
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

static EventGroupHandle_t s_wifi_event_group;

// Global I2C initialization flag
static bool i2c_master_initialized = false;
static bool speaker_amplifier_enabled = false;
static const char *TAG = "voice_assistant";
static const char *AUDIO_TAG = "audio";
static const char *HTTP_TAG = "http_client";
static int s_retry_num = 0;

typedef struct
{
	uint8_t *audio_data;
	size_t data_len;
} audio_data_t;

// Keyword detection states
typedef enum
{
	KEYWORD_STATE_LISTENING, // Waiting for keyword
	KEYWORD_STATE_DETECTING, // Processing potential keyword
	KEYWORD_STATE_CONFIRMED, // Keyword confirmed, ready for command
	KEYWORD_STATE_TIMEOUT	 // Keyword detection timeout
} keyword_state_t;

// Keyword detection data structure
typedef struct
{
	int16_t *audio_buffer;		// Circular buffer for audio
	size_t buffer_pos;			// Current position in buffer
	size_t samples_collected;	// Total samples collected
	keyword_state_t state;		// Current detection state
	TickType_t detection_start; // When keyword detection started
	float confidence;			// Detection confidence score
	bool keyword_detected;		// Final detection result
} keyword_detector_t;

static void event_handler(void *arg, esp_event_base_t event_base,
						  int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (s_retry_num < WIFI_MAXIMUM_RETRY)
		{
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		}
		else
		{
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG, "connect to the AP fail");
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

// Audio initialization for PDM microphone (recording)
esp_err_t init_microphone(void)
{
	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM,
		.sample_rate = SAMPLE_RATE,
		.bits_per_sample = BITS_PER_SAMPLE,
		.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count = 8,
		.dma_buf_len = BUFFER_SIZE,
		.use_apll = false,
		.tx_desc_auto_clear = false,
		.fixed_mclk = 0};

	// M5Stack Core2 PDM microphone pins (typical configuration)
	i2s_pin_config_t pin_config = {
		.bck_io_num = I2S_PIN_NO_CHANGE,
		.ws_io_num = GPIO_NUM_0, // PDM CLK
		.data_out_num = I2S_PIN_NO_CHANGE,
		.data_in_num = GPIO_NUM_34, // PDM DATA
	};

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

// Initialize I2C for AXP192 communication (one-time initialization)
esp_err_t init_i2c_master(void)
{
	// Check if already initialized
	if (i2c_master_initialized)
	{
		ESP_LOGI(AUDIO_TAG, "♻️ I2C master already initialized, reusing existing driver");
		return ESP_OK;
	}

	ESP_LOGI(AUDIO_TAG, "🔧 Initializing I2C master for AXP192 (first time)...");

	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = I2C_MASTER_SDA_IO,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_io_num = I2C_MASTER_SCL_IO,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = I2C_MASTER_FREQ_HZ,
	};

	esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ I2C param config failed: %s", esp_err_to_name(ret));
		return ret;
	}

	ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ I2C driver install failed: %s", esp_err_to_name(ret));
		return ret;
	}

	// Mark as initialized
	i2c_master_initialized = true;
	ESP_LOGI(AUDIO_TAG, "✅ I2C master initialized for AXP192 - ready for reuse");
	return ESP_OK;
}

// Enable M5Stack Core2 speaker amplifier via AXP192
esp_err_t enable_speaker_amplifier(void)
{
	// Check if already enabled (AXP192 registers retain state)
	if (speaker_amplifier_enabled)
	{
		ESP_LOGI(AUDIO_TAG, "🔊 Speaker amplifier already enabled, ready for use");
		return ESP_OK;
	}

	ESP_LOGI(AUDIO_TAG, "🔌 Enabling M5Stack Core2 speaker amplifier via AXP192...");

	// Initialize I2C if not already done
	esp_err_t ret = init_i2c_master();
	if (ret != ESP_OK)
	{
		return ret;
	}

	// Step 1: Set AXP192 GPIO0 to output mode (register 0x90)
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (AXP192_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, 0x90, true); // GPIO0 function register
	i2c_master_write_byte(cmd, 0x02, true); // Set GPIO0 as output (bit 1)
	i2c_master_stop(cmd);
	ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
	i2c_cmd_link_delete(cmd);

	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to set AXP192 GPIO0 mode: %s", esp_err_to_name(ret));
		return ret;
	}
	ESP_LOGI(AUDIO_TAG, "✅ AXP192 GPIO0 set to output mode");

	// Step 2: Enable GPIO0 output (register 0x94)
	cmd = i2c_cmd_link_create();
	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (AXP192_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, 0x94, true); // GPIO0 output state register
	i2c_master_write_byte(cmd, 0x01, true); // Set GPIO0 high (enable amplifier)
	i2c_master_stop(cmd);
	ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(1000));
	i2c_cmd_link_delete(cmd);

	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to enable AXP192 GPIO0: %s", esp_err_to_name(ret));
		return ret;
	}
	ESP_LOGI(AUDIO_TAG, "✅ AXP192 GPIO0 enabled (speaker amplifier should be powered)");

	// Additional delay for amplifier to power up
	vTaskDelay(pdMS_TO_TICKS(200));

	// Mark as successfully enabled
	speaker_amplifier_enabled = true;
	ESP_LOGI(AUDIO_TAG, "🔊 M5Stack Core2 speaker amplifier fully enabled and cached");
	return ESP_OK;
}

// Audio initialization for I2S speaker (playback)
esp_err_t init_speaker(void)
{
	ESP_LOGI(AUDIO_TAG, "🔊 Initializing M5Stack Core2 speaker (NS4168 amplifier)...");

	// CRITICAL: Enable speaker amplifier via AXP192 power management
	esp_err_t amp_ret = enable_speaker_amplifier();
	if (amp_ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to enable speaker amplifier - audio will not work");
		return amp_ret;
	}

	// First uninstall microphone driver if installed
	esp_err_t uninstall_err = i2s_driver_uninstall(I2S_NUM);
	if (uninstall_err != ESP_OK && uninstall_err != ESP_ERR_INVALID_STATE)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to uninstall I2S driver: %s", esp_err_to_name(uninstall_err));
	}
	else
	{
		ESP_LOGI(AUDIO_TAG, "I2S driver uninstalled successfully");
	}

	// Add delay to ensure clean driver state transition
	vTaskDelay(pdMS_TO_TICKS(100));

	i2s_config_t i2s_config = {
		.mode = I2S_MODE_MASTER | I2S_MODE_TX,
		.sample_rate = SAMPLE_RATE,
		.bits_per_sample = BITS_PER_SAMPLE,
		.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
		.communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
		.dma_buf_count = 8,
		.dma_buf_len = BUFFER_SIZE,
		.use_apll = false,
		.tx_desc_auto_clear = true,
	};

	// M5Stack Core2 I2S speaker pins
	i2s_pin_config_t pin_config = {
		.bck_io_num = GPIO_NUM_12,	// BCLK
		.ws_io_num = GPIO_NUM_0,	// LRCK
		.data_out_num = GPIO_NUM_2, // DOUT
		.data_in_num = I2S_PIN_NO_CHANGE,
	};

	esp_err_t ret = i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to install I2S driver for speaker: %s", esp_err_to_name(ret));
		return ret;
	}
	ESP_LOGI(AUDIO_TAG, "✅ I2S driver installed for speaker");

	ret = i2s_set_pin(I2S_NUM, &pin_config);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to set I2S pins for speaker: %s", esp_err_to_name(ret));
		i2s_driver_uninstall(I2S_NUM); // Clean up on failure
		return ret;
	}
	ESP_LOGI(AUDIO_TAG, "✅ I2S pins configured: BCK=GPIO12, WS=GPIO0, DATA=GPIO2");

	// Add delay to ensure hardware is ready
	vTaskDelay(pdMS_TO_TICKS(50));

	// Clear any existing data in DMA buffers
	i2s_zero_dma_buffer(I2S_NUM);
	ESP_LOGI(AUDIO_TAG, "✅ DMA buffers cleared");

	ESP_LOGI(AUDIO_TAG, "🎵 M5Stack Core2 speaker ready for audio playback");
	return ESP_OK;
}

// Initialize system (console-based for now)
void init_system(void)
{
	ESP_LOGI(TAG, "=================================");
	ESP_LOGI(TAG, "    M5Stack Core2 Voice Assistant");
	ESP_LOGI(TAG, "=================================");
	ESP_LOGI(TAG, "Connecting to WiFi...");
}

// Play a ready beep sound with enhanced M5Stack Core2 support
void play_ready_beep(void)
{
	ESP_LOGI(AUDIO_TAG, "🔊 Playing M5Stack Core2 ready beep...");

	// Initialize speaker with proper error handling
	if (init_speaker() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to initialize speaker for beep - no audio feedback");
		return;
	}

	// Additional delay after speaker initialization for hardware stability
	vTaskDelay(pdMS_TO_TICKS(100));

	// Try multiple beep strategies for M5Stack Core2 NS4168 amplifier
	const int beep_strategies = 3;
	const int beep_freqs[] = {800, 1000, 1200}; // Different frequencies
	const int beep_duration_ms = 500;			// Longer duration for better audibility
	const int total_samples = (SAMPLE_RATE * beep_duration_ms) / 1000;

	int16_t *beep_buffer = (int16_t *)malloc(total_samples * sizeof(int16_t));
	if (!beep_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "❌ Failed to allocate beep buffer");
		i2s_driver_uninstall(I2S_NUM);
		return;
	}

	ESP_LOGI(AUDIO_TAG, "🎵 Generating multi-tone beep pattern...");

	// Generate triple-tone beep: 800Hz -> 1000Hz -> 1200Hz
	int samples_per_tone = total_samples / beep_strategies;
	for (int strategy = 0; strategy < beep_strategies; strategy++)
	{
		int freq = beep_freqs[strategy];
		int samples_per_cycle = SAMPLE_RATE / freq;
		int start_idx = strategy * samples_per_tone;
		int end_idx = (strategy + 1) * samples_per_tone;

		ESP_LOGI(AUDIO_TAG, "   🎼 Tone %d: %dHz (%d samples)", strategy + 1, freq, samples_per_tone);

		for (int i = start_idx; i < end_idx && i < total_samples; i++)
		{
			int sample_in_cycle = (i - start_idx) % samples_per_cycle;

			// Try both sine wave and square wave for better compatibility
			if (strategy == 1)
			{
				// Square wave for middle tone (may work better with some amplifiers)
				float duty_cycle = 0.5f;
				beep_buffer[i] = (sample_in_cycle < (samples_per_cycle * duty_cycle)) ? 12000 : -12000;
			}
			else
			{
				// Sine wave for first and third tones
				float angle = (2.0 * M_PI * sample_in_cycle) / samples_per_cycle;
				beep_buffer[i] = (int16_t)(sin(angle) * 12000); // Maximum volume
			}
		}
	}

	// Play the beep with enhanced error checking
	ESP_LOGI(AUDIO_TAG, "🎶 Starting audio playback (%d bytes)...", total_samples * sizeof(int16_t));

	size_t bytes_written = 0;
	size_t expected_bytes = total_samples * sizeof(int16_t);
	esp_err_t ret = i2s_write(I2S_NUM, beep_buffer, expected_bytes, &bytes_written, pdMS_TO_TICKS(2000));

	if (ret == ESP_OK)
	{
		if (bytes_written == expected_bytes)
		{
			ESP_LOGI(AUDIO_TAG, "✅ BEEP SUCCESS: %d/%d bytes written to NS4168 amplifier", bytes_written, expected_bytes);
		}
		else
		{
			ESP_LOGW(AUDIO_TAG, "⚠️ Partial write: %d/%d bytes written", bytes_written, expected_bytes);
		}
	}
	else
	{
		ESP_LOGE(AUDIO_TAG, "❌ I2S write failed: %s (wrote %d/%d bytes)", esp_err_to_name(ret), bytes_written, expected_bytes);
	}

	// Wait for DMA to finish playing all samples
	ESP_LOGI(AUDIO_TAG, "⏳ Waiting for audio DMA completion...");
	vTaskDelay(pdMS_TO_TICKS(600)); // Wait for full 500ms beep + buffer time

	// Verify DMA buffer state
	ESP_LOGI(AUDIO_TAG, "🔍 Checking DMA buffer status...");
	i2s_zero_dma_buffer(I2S_NUM); // Clear any remaining data

	free(beep_buffer);

	// Clean driver shutdown
	esp_err_t uninstall_ret = i2s_driver_uninstall(I2S_NUM);
	if (uninstall_ret == ESP_OK)
	{
		ESP_LOGI(AUDIO_TAG, "✅ I2S driver uninstalled cleanly");
	}
	else
	{
		ESP_LOGW(AUDIO_TAG, "⚠️ I2S uninstall warning: %s", esp_err_to_name(uninstall_ret));
	}

	ESP_LOGI(AUDIO_TAG, "🎵 Beep playback sequence completed");
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
	char equalizer_line[EQ_BARS + 10];

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
			equalizer_line[bar] = '#'; // High
		else if (bar_height >= 2)
			equalizer_line[bar] = '|'; // Medium
		else if (bar_height >= 1)
			equalizer_line[bar] = '.'; // Low
		else
			equalizer_line[bar] = ' '; // Silent
	}

	equalizer_line[EQ_BARS] = '\0';

	// Calculate total audio level
	int64_t total_energy = 0;
	for (size_t i = 0; i < samples; i++)
	{
		total_energy += abs(audio_buffer[i]);
	}
	int avg_level = total_energy / samples;

	// Display console equalizer and status
	ESP_LOGI(AUDIO_TAG, "EQ: [%s] Level: %d %s",
			 equalizer_line,
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

// Initialize keyword detector
keyword_detector_t *init_keyword_detector(void)
{
	keyword_detector_t *detector = (keyword_detector_t *)malloc(sizeof(keyword_detector_t));
	if (!detector)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate keyword detector");
		return NULL;
	}

	// Allocate audio buffer in PSRAM for better performance
	detector->audio_buffer = (int16_t *)heap_caps_malloc(KEYWORD_BUFFER_SIZE * sizeof(int16_t),
														 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
	if (!detector->audio_buffer)
	{
		detector->audio_buffer = (int16_t *)malloc(KEYWORD_BUFFER_SIZE * sizeof(int16_t));
	}

	if (!detector->audio_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate keyword audio buffer");
		free(detector);
		return NULL;
	}

	// Initialize detector state
	detector->buffer_pos = 0;
	detector->samples_collected = 0;
	detector->state = KEYWORD_STATE_LISTENING;
	detector->detection_start = 0;
	detector->confidence = 0.0f;
	detector->keyword_detected = false;

	ESP_LOGI(AUDIO_TAG, "Keyword detector initialized with %d sample buffer", KEYWORD_BUFFER_SIZE);
	return detector;
}

// Add audio sample to keyword detector
void keyword_detector_add_samples(keyword_detector_t *detector, int16_t *samples, size_t count)
{
	if (!detector || !samples)
		return;

	for (size_t i = 0; i < count; i++)
	{
		// Add to circular buffer
		detector->audio_buffer[detector->buffer_pos] = samples[i];
		detector->buffer_pos = (detector->buffer_pos + 1) % KEYWORD_BUFFER_SIZE;

		if (detector->samples_collected < KEYWORD_BUFFER_SIZE)
		{
			detector->samples_collected++;
		}
	}
}

// Simplified keyword detection based on voice activity patterns
bool detect_keyword_pattern(keyword_detector_t *detector)
{
	if (detector->samples_collected < (SAMPLE_RATE * KEYWORD_MIN_LENGTH_MS / 1000))
	{
		return false; // Not enough samples
	}

	// Get recent audio samples for analysis
	size_t samples_to_analyze = SAMPLE_RATE * 1; // Analyze 1 second of audio
	if (samples_to_analyze > detector->samples_collected)
	{
		samples_to_analyze = detector->samples_collected;
	}

	// Get samples from circular buffer
	int16_t *analysis_buffer = (int16_t *)malloc(samples_to_analyze * sizeof(int16_t));
	if (!analysis_buffer)
		return false;

	size_t start_pos = (detector->buffer_pos + KEYWORD_BUFFER_SIZE - samples_to_analyze) % KEYWORD_BUFFER_SIZE;
	for (size_t i = 0; i < samples_to_analyze; i++)
	{
		analysis_buffer[i] = detector->audio_buffer[(start_pos + i) % KEYWORD_BUFFER_SIZE];
	}

	// Simple keyword detection: look for sustained voice activity pattern
	// Calculate energy in segments to detect speech patterns
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
		segment_energies[seg] = segment_energy; // Store for variation calculation
		total_energy += segment_energy;

		// Consider segment "active" if it has significant energy
		if (segment_energy > 0.001f)
		{ // Lowered threshold
			active_segments++;
		}
	}

	float avg_energy = total_energy / num_segments;

	// Enhanced keyword detection for "Hey El" - looking for specific patterns:
	// 1. Must have sustained speech (30% active segments - shorter phrase)
	// 2. Must have sufficient energy above background
	// 3. Must have energy variation (not just noise)
	// 4. Must last for reasonable duration (~0.3 seconds minimum for "Hey El")

	float energy_variation = 0.0f;
	if (active_segments > 0)
	{
		// Calculate energy variation across segments
		for (int i = 0; i < num_segments; i++)
		{
			float segment_energy = segment_energies[i];
			energy_variation += fabsf(segment_energy - avg_energy);
		}
		energy_variation /= num_segments;
	}

	// LOWERED THRESHOLDS: Much more permissive detection for "Hey El"
	bool has_sustained_speech = (active_segments >= 2);										// 20% of segments active (very lenient)
	bool has_sufficient_energy = (avg_energy > 0.0005f);									// Much lower threshold, closer to background
	bool has_variation = (energy_variation > 0.00005f);										// Very low variation threshold
	bool reasonable_duration = (detector->samples_collected >= KEYWORD_BUFFER_SIZE * 0.1f); // 10% of buffer (0.2s minimum)

	// Additional pattern recognition for "Hey El" (two-syllable phrase)
	bool has_two_syllable_pattern = false;
	if (has_sustained_speech && has_sufficient_energy)
	{
		// Look for two energy peaks characteristic of "Hey" + "El"
		// Find segments with highest energy (representing syllables)
		float peak_threshold = avg_energy * 1.2f; // 20% above average
		int peak_count = 0;
		int first_peak = -1, last_peak = -1;

		for (int i = 0; i < num_segments; i++)
		{
			if (segment_energies[i] > peak_threshold)
			{
				peak_count++;
				if (first_peak == -1)
					first_peak = i;
				last_peak = i;
			}
		}

		// "Hey El" should have 1-3 peak segments with reasonable spacing
		if (peak_count >= 1 && peak_count <= 4)
		{
			int peak_span = (last_peak - first_peak) + 1;
			// Peaks should span 2-6 segments (reasonable for short phrase)
			has_two_syllable_pattern = (peak_span >= 2 && peak_span <= 6);
		}
	}

	// Multi-level detection logic with fallback
	bool keyword_detected_full = has_sustained_speech && has_sufficient_energy && has_variation && reasonable_duration;

	// Fallback: Super simple detection for any significant voice activity
	bool fallback_detection = (active_segments >= 1) && (avg_energy > 0.0003f) &&
							  (detector->samples_collected >= KEYWORD_BUFFER_SIZE * 0.05f); // Just 0.1s of any voice

	// Use either full criteria OR fallback (for testing)
	bool keyword_detected = keyword_detected_full || fallback_detection;

	// DEBUG: Show which detection method triggered
	if (keyword_detected && !keyword_detected_full)
	{
		ESP_LOGI(AUDIO_TAG, "🎯 FALLBACK detection triggered! (not full criteria)");
	}

	detector->confidence = avg_energy;

	// Enhanced debug logging with detailed failure analysis
	static int debug_counter = 0;
	if (++debug_counter % 50 == 0 || keyword_detected || has_sustained_speech) // More frequent logging when voice detected
	{
		ESP_LOGI(AUDIO_TAG, "🔍 'Hey El': Segs=%d/10(≥2), Energy=%.4f(>0.0005), Var=%.5f(>0.00005), Dur=%d%%(≥10%%), Det=%s [%s|%s|%s|%s|%s]",
				 active_segments, avg_energy, energy_variation,
				 (int)((detector->samples_collected * 100) / KEYWORD_BUFFER_SIZE),
				 keyword_detected ? "YES" : "NO",
				 has_sustained_speech ? "S" : "s",
				 has_sufficient_energy ? "E" : "e",
				 has_variation ? "V" : "v",
				 reasonable_duration ? "D" : "d",
				 has_two_syllable_pattern ? "P" : "p");

		// Additional pattern analysis debug when voice is detected
		if (has_sustained_speech)
		{
			float peak_threshold = avg_energy * 1.2f;
			int peak_count = 0;
			int first_peak = -1, last_peak = -1;

			for (int i = 0; i < num_segments; i++)
			{
				if (segment_energies[i] > peak_threshold)
				{
					peak_count++;
					if (first_peak == -1)
						first_peak = i;
					last_peak = i;
				}
			}

			if (peak_count > 0)
			{
				int peak_span = (last_peak - first_peak) + 1;
				ESP_LOGI(AUDIO_TAG, "   📊 Pattern: PeakCount=%d(1-4), PeakSpan=%d(2-6), Threshold=%.4f",
						 peak_count, peak_span, peak_threshold);
			}
		}
	}

	free(analysis_buffer);

	return keyword_detected;
}

// Main keyword detection function
bool process_keyword_detection(keyword_detector_t *detector, int16_t *audio_samples, size_t sample_count)
{
	if (!detector)
		return false;

	// Add new samples to detector
	keyword_detector_add_samples(detector, audio_samples, sample_count);

	TickType_t current_time = xTaskGetTickCount();

	switch (detector->state)
	{
	case KEYWORD_STATE_LISTENING:
		// Look for keyword pattern
		if (detect_keyword_pattern(detector))
		{
			detector->state = KEYWORD_STATE_DETECTING;
			detector->detection_start = current_time;
			ESP_LOGI(AUDIO_TAG, "🎯 Potential keyword detected, verifying...");
		}
		break;

	case KEYWORD_STATE_DETECTING:
		// Verify keyword over a short period
		if (detect_keyword_pattern(detector))
		{
			detector->state = KEYWORD_STATE_CONFIRMED;
			detector->keyword_detected = true;
			ESP_LOGI(AUDIO_TAG, "✅ KEYWORD CONFIRMED! Ready for command (confidence: %.3f)",
					 detector->confidence);
			return true;
		}

		// Check timeout
		if ((current_time - detector->detection_start) > pdMS_TO_TICKS(1000))
		{
			detector->state = KEYWORD_STATE_LISTENING;
			ESP_LOGI(AUDIO_TAG, "⏰ Keyword verification timeout, back to listening");
		}
		break;

	case KEYWORD_STATE_CONFIRMED:
		// Keyword was detected, waiting for reset
		break;

	case KEYWORD_STATE_TIMEOUT:
		detector->state = KEYWORD_STATE_LISTENING;
		break;
	}

	return false;
}

// Reset keyword detector to listening state
void reset_keyword_detector(keyword_detector_t *detector)
{
	if (detector)
	{
		detector->state = KEYWORD_STATE_LISTENING;
		detector->keyword_detected = false;
		detector->confidence = 0.0f;
		detector->buffer_pos = 0;
		detector->samples_collected = 0;
		ESP_LOGI(AUDIO_TAG, "🔄 Keyword detector reset to listening state");
	}
}

// HTTP event handler for response data
esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
	switch (evt->event_id)
	{
	case HTTP_EVENT_ON_DATA:
		ESP_LOGI(HTTP_TAG, "Received %d bytes from server", evt->data_len);
		// Here you could save the audio response data for playback
		break;
	case HTTP_EVENT_ON_FINISH:
		ESP_LOGI(HTTP_TAG, "HTTP request completed");
		break;
	case HTTP_EVENT_ERROR:
		ESP_LOGE(HTTP_TAG, "HTTP error occurred");
		break;
	default:
		break;
	}
	return ESP_OK;
}

// Send audio chunk to Cloudflare Worker (streaming)
esp_err_t stream_audio_chunk(uint8_t *audio_data, size_t data_len, bool is_first_chunk, bool is_last_chunk)
{
	esp_http_client_config_t config = {
		.url = WORKER_URL,
		.method = HTTP_METHOD_POST,
		.event_handler = http_event_handler,
		.timeout_ms = 5000,
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (!client)
	{
		ESP_LOGE(HTTP_TAG, "Failed to initialize HTTP client");
		return ESP_FAIL;
	}

	// Set headers
	esp_http_client_set_header(client, "Content-Type", "audio/pcm");
	esp_http_client_set_header(client, "User-Agent", "M5Stack-Core2-Assistant");
	esp_http_client_set_header(client, "X-Audio-Sample-Rate", "16000");
	esp_http_client_set_header(client, "X-Audio-Channels", "1");
	esp_http_client_set_header(client, "X-Audio-Bits-Per-Sample", "16");

	// Add streaming headers
	if (is_first_chunk)
	{
		esp_http_client_set_header(client, "X-Stream-Start", "true");
	}
	if (is_last_chunk)
	{
		esp_http_client_set_header(client, "X-Stream-End", "true");
	}

	// Set POST data
	esp_err_t err = esp_http_client_set_post_field(client, (const char *)audio_data, data_len);
	if (err != ESP_OK)
	{
		ESP_LOGE(HTTP_TAG, "Failed to set POST data: %s", esp_err_to_name(err));
		esp_http_client_cleanup(client);
		return err;
	}

	// Perform HTTP request
	err = esp_http_client_perform(client);
	if (err == ESP_OK)
	{
		int status_code = esp_http_client_get_status_code(client);
		ESP_LOGI(HTTP_TAG, "Streamed %d bytes, Status = %d", data_len, status_code);
	}
	else
	{
		ESP_LOGE(HTTP_TAG, "HTTP streaming failed: %s", esp_err_to_name(err));
	}

	esp_http_client_cleanup(client);
	return err;
}

// Send complete audio data to Cloudflare Worker (non-streaming fallback)
esp_err_t send_audio_to_worker(uint8_t *audio_data, size_t data_len)
{
	return stream_audio_chunk(audio_data, data_len, true, true);
}

// Record and stream audio in real-time
void record_and_stream_audio(void)
{
	ESP_LOGI(AUDIO_TAG, "Starting real-time audio streaming...");

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

	// Smaller buffers for streaming
	int16_t *temp_buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
	uint8_t *stream_buffer = (uint8_t *)malloc(STREAM_CHUNK_SIZE);

	if (!temp_buffer || !stream_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate memory for streaming. Free heap: %d", esp_get_free_heap_size());
		if (temp_buffer)
			free(temp_buffer);
		if (stream_buffer)
			free(stream_buffer);
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
		esp_err_t ret = i2s_read(I2S_NUM, temp_buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);

		if (ret != ESP_OK)
		{
			ESP_LOGE(AUDIO_TAG, "I2S read error: %s", esp_err_to_name(ret));
			break;
		}

		if (bytes_read > 0)
		{
			bool voice_detected = detect_voice_activity(temp_buffer, bytes_read / sizeof(int16_t));

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

				memcpy(stream_buffer + stream_buffer_pos, temp_buffer, bytes_to_copy);
				stream_buffer_pos += bytes_to_copy;

				// Stream when buffer is full
				if (stream_buffer_pos >= STREAM_CHUNK_SIZE)
				{
					ESP_LOGI(AUDIO_TAG, "Streaming chunk %d bytes...", stream_buffer_pos);

					esp_err_t stream_err = stream_audio_chunk(stream_buffer, stream_buffer_pos, is_first_chunk, false);
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
						memcpy(stream_buffer, (uint8_t *)temp_buffer + bytes_to_copy, remaining);
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
						stream_audio_chunk(stream_buffer, stream_buffer_pos, false, true);
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
						stream_audio_chunk(stream_buffer, stream_buffer_pos, false, true);
					}
					break;
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(1)); // Minimal delay for streaming
	}

	free(temp_buffer);
	free(stream_buffer);
	i2s_driver_uninstall(I2S_NUM);
}

// Play audio response (placeholder for when you receive audio back)
void play_audio_response(uint8_t *audio_data, size_t data_len)
{
	ESP_LOGI(AUDIO_TAG, "Playing audio response...");

	if (init_speaker() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize speaker");
		return;
	}

	size_t bytes_written = 0;
	esp_err_t ret = i2s_write(I2S_NUM, audio_data, data_len, &bytes_written, portMAX_DELAY);

	if (ret == ESP_OK)
	{
		ESP_LOGI(AUDIO_TAG, "Audio playback completed. Bytes written: %d", bytes_written);
	}
	else
	{
		ESP_LOGE(AUDIO_TAG, "Audio playback failed: %s", esp_err_to_name(ret));
	}

	i2s_driver_uninstall(I2S_NUM);
}

// Enhanced voice assistant with keyword detection
void keyword_listening_loop(void)
{
	ESP_LOGI(AUDIO_TAG, "🎧 Starting keyword detection - Say 'Hey El'");

	// Initialize keyword detector
	keyword_detector_t *detector = init_keyword_detector();
	if (!detector)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize keyword detector");
		return;
	}

	// Initialize microphone for keyword detection
	if (init_microphone() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize microphone for keyword detection");
		free(detector);
		return;
	}

	int16_t *temp_buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
	if (!temp_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate temp buffer for keyword detection");
		i2s_driver_uninstall(I2S_NUM);
		free(detector);
		return;
	}

	ESP_LOGI(AUDIO_TAG, "👂 Listening for keyword 'Hey El'...");

	while (1)
	{
		size_t bytes_read = 0;
		esp_err_t ret = i2s_read(I2S_NUM, temp_buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_read, portMAX_DELAY);

		if (ret != ESP_OK)
		{
			ESP_LOGE(AUDIO_TAG, "I2S read error during keyword detection: %s", esp_err_to_name(ret));
			continue;
		}

		if (bytes_read > 0)
		{
			size_t sample_count = bytes_read / sizeof(int16_t);

			// Check for basic voice activity first to avoid unnecessary processing
			bool has_voice = detect_voice_activity(temp_buffer, sample_count);

			if (has_voice)
			{
				// Process samples for keyword detection only when voice is present
				if (process_keyword_detection(detector, temp_buffer, sample_count))
				{
					ESP_LOGI(AUDIO_TAG, "🎯 KEYWORD DETECTED! Switching to command mode...");

					// Play confirmation beep
					i2s_driver_uninstall(I2S_NUM);
					play_ready_beep();

					// Switch to voice command recording
					record_and_stream_audio();

					// Reset keyword detector and continue listening
					reset_keyword_detector(detector);

					// Reinitialize microphone for keyword detection
					if (init_microphone() != ESP_OK)
					{
						ESP_LOGE(AUDIO_TAG, "Failed to reinitialize microphone");
						break;
					}

					ESP_LOGI(AUDIO_TAG, "🔄 Back to keyword listening mode");
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

	// Cleanup
	free(temp_buffer);
	if (detector->audio_buffer)
		free(detector->audio_buffer);
	free(detector);
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

void wifi_init_sta(void)
{
	s_wifi_event_group = xEventGroupCreate();

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
														ESP_EVENT_ANY_ID,
														&event_handler,
														NULL,
														&instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
														IP_EVENT_STA_GOT_IP,
														&event_handler,
														NULL,
														&instance_got_ip));

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASS,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.pmf_cfg = {
				.capable = true,
				.required = false},
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	ESP_LOGI(TAG, "wifi_init_sta finished.");

	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
										   WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
										   pdFALSE,
										   pdFALSE,
										   portMAX_DELAY);

	if (bits & WIFI_CONNECTED_BIT)
	{
		ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
				 WIFI_SSID, WIFI_PASS);
	}
	else if (bits & WIFI_FAIL_BIT)
	{
		ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
				 WIFI_SSID, WIFI_PASS);
	}
	else
	{
		ESP_LOGE(TAG, "UNEXPECTED EVENT");
	}
}

void app_main(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());

	// Initialize system
	init_system();

	ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
	wifi_init_sta();

	ESP_LOGI(TAG, "✅ WiFi connection established. Starting voice assistant...");

	// Create voice assistant task with larger stack size
	xTaskCreate(&voice_assistant_task, "voice_assistant", 16384, NULL, 5, NULL);

	// Main loop - keep app running
	while (1)
	{
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}
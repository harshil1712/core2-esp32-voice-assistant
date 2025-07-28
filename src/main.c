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
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

#define WIFI_SSID "WIFI_NETWORK" // Replace with your WiFi SSID
#define WIFI_PASS "PASSWORD1234" // Replace with your WiFi credentials
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
#define STREAM_CHUNK_SIZE 4096	  // Send chunks of this size

// Voice Activity Detection with adaptive threshold
#define VAD_BASE_THRESHOLD 600 // Lower base threshold for better detection
#define VAD_MIN_CONSECUTIVE 2  // Require 2 consecutive detections
#define VAD_NOISE_SAMPLES 50   // Fewer samples to learn noise floor faster
#define SILENCE_TIMEOUT_MS 2000

// Console equalizer configuration (for debugging)
#define EQ_BARS 20
#define EQ_MAX_HEIGHT 50

#define WORKER_URL "https://your-worker.your-subdomain.workers.dev/audio" // Production

static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "voice_assistant";
static const char *AUDIO_TAG = "audio";
static const char *HTTP_TAG = "http_client";
static int s_retry_num = 0;

typedef struct
{
	uint8_t *audio_data;
	size_t data_len;
} audio_data_t;

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

// Audio initialization for I2S speaker (playback)
esp_err_t init_speaker(void)
{
	// First uninstall microphone driver if installed
	esp_err_t uninstall_err = i2s_driver_uninstall(I2S_NUM);
	if (uninstall_err != ESP_OK && uninstall_err != ESP_ERR_INVALID_STATE)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to uninstall I2S driver: %s", esp_err_to_name(uninstall_err));
	}

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
		ESP_LOGE(AUDIO_TAG, "Failed to install I2S driver for speaker");
		return ret;
	}

	ret = i2s_set_pin(I2S_NUM, &pin_config);
	if (ret != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to set I2S pins for speaker");
		return ret;
	}

	ESP_LOGI(AUDIO_TAG, "Speaker initialized successfully");
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

// Play a ready beep sound
void play_ready_beep(void)
{
	ESP_LOGI(AUDIO_TAG, "Playing ready beep...");

	// Initialize speaker
	if (init_speaker() != ESP_OK)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to initialize speaker for beep");
		return;
	}

	// Generate a simple 1000Hz beep for 200ms
	const int beep_freq = 1000;
	const int beep_duration_ms = 200;
	const int samples_per_cycle = SAMPLE_RATE / beep_freq;
	const int total_samples = (SAMPLE_RATE * beep_duration_ms) / 1000;

	int16_t *beep_buffer = (int16_t *)malloc(total_samples * sizeof(int16_t));
	if (!beep_buffer)
	{
		ESP_LOGE(AUDIO_TAG, "Failed to allocate beep buffer");
		i2s_driver_uninstall(I2S_NUM);
		return;
	}

	// Generate sine wave
	for (int i = 0; i < total_samples; i++)
	{
		float angle = (2.0 * M_PI * i) / samples_per_cycle;
		beep_buffer[i] = (int16_t)(sin(angle) * 5000); // Volume control
	}

	// Play the beep
	size_t bytes_written = 0;
	i2s_write(I2S_NUM, beep_buffer, total_samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);

	free(beep_buffer);
	i2s_driver_uninstall(I2S_NUM);
	ESP_LOGI(AUDIO_TAG, "Ready beep completed");
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

// Voice assistant task
void voice_assistant_task(void *pvParameters)
{
	while (1)
	{
		ESP_LOGI(TAG, "🔊 Ready for voice command. Say something!");

		// Play ready beep
		play_ready_beep();

		// Record and stream audio to Cloudflare Worker
		record_and_stream_audio();

		ESP_LOGI(TAG, "📤 Processing complete. Waiting for next command...");

		// Wait before next recording
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
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
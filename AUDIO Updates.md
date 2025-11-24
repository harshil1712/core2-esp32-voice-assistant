#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <M5Unified.h>
#include <M5GFX.h>

#define SD_SPI_CS_PIN 4
#define SD_SPI_SCK_PIN 18
#define SD_SPI_MISO_PIN 38
#define SD_SPI_MOSI_PIN 23

void setup()
{
    M5.begin();

    M5.Display.setTextFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextSize(1);

    M5.Speaker.setAllChannelVolume(255);

    // SD Card Initialization
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000))
    {
        // Print a message if SD card initialization failed or if the SD card does not exist.
        M5.Display.print("\n SD card not detected\n");
        while (1)
            ;
    }
    else
    {
        M5.Display.print("\n SD card detected\n");
    }
    delay(1000);

    M5.Display.print("\n Playing audio...\n");

    // Write TXT file
    M5.Display.print("\n SD card write test...\n");
    auto file = SD.open("/WriteTest.txt", FILE_WRITE, true);
    if (file)
    {
        file.print("Hello, world! \nSD card write success! \n");
        file.close();
        M5.Display.print(" SD card write success\n");
    }
    else
    {
        M5.Display.print(" Failed to create TXT file\n");
    }
    delay(1000);

    M5.Display.print("\n SD card read test...\n");
    if (SD.open("/TestPicture01.png", FILE_READ, false))
    {
        M5.Display.print(" PNG file 01 detected\n");
    }
    else
    {
        M5.Display.print(" PNG file 01 not detected\n");
    }
    if (SD.open("/TestPicture02.png", FILE_READ, false))
    {
        M5.Display.print(" PNG file 02 detected\n");
    }
    else
    {
        M5.Display.print(" PNG file 02 not detected\n");
    }
}

struct __attribute__((packed)) wav_header_t
{
    char RIFF[4];
    uint32_t chunk_size;
    char WAVEfmt[8];
    uint32_t fmt_chunk_size;
    uint16_t audiofmt;
    uint16_t channel;
    uint32_t sample_rate;
    uint32_t byte_per_sec;
    uint16_t block_size;
    uint16_t bit_per_sample;
};

struct __attribute__((packed)) sub_chunk_t
{
    char identifier[4];
    uint32_t chunk_size;
    uint8_t data[1];
};

static constexpr const size_t buf_num = 3;
static constexpr const size_t buf_size = 1024;
static uint8_t wav_data[buf_num][buf_size];

bool playWavFile(const char *filename)
{
    auto file = SD.open(filename, FILE_READ);
    if (!file)
    {
        M5.Display.printf("Failed to open: %s\n", filename);
        return false;
    }

    M5.Display.printf("Playing: %s\n", filename);

    // Read WAV header
    wav_header_t wav_header;
    file.read((uint8_t *)&wav_header, sizeof(wav_header_t));

    // Validate WAV file
    if (memcmp(wav_header.RIFF, "RIFF", 4) ||
        memcmp(wav_header.WAVEfmt, "WAVEfmt ", 8) ||
        wav_header.audiofmt != 1 ||
        wav_header.bit_per_sample < 8 ||
        wav_header.bit_per_sample > 16 ||
        wav_header.channel == 0 ||
        wav_header.channel > 2)
    {
        M5.Display.printf("Invalid WAV file\n");
        file.close();
        return false;
    }

    // Find data chunk
    file.seek(offsetof(wav_header_t, audiofmt) + wav_header.fmt_chunk_size);
    sub_chunk_t sub_chunk;
    file.read((uint8_t *)&sub_chunk, 8);

    while (memcmp(sub_chunk.identifier, "data", 4))
    {
        if (!file.seek(sub_chunk.chunk_size, SeekMode::SeekCur))
        {
            break;
        }
        file.read((uint8_t *)&sub_chunk, 8);
    }

    if (memcmp(sub_chunk.identifier, "data", 4))
    {
        M5.Display.printf("No data chunk found\n");
        file.close();
        return false;
    }

    // Play audio data
    int32_t data_len = sub_chunk.chunk_size;
    bool flg_16bit = (wav_header.bit_per_sample >> 4);

    size_t idx = 0;
    while (data_len > 0)
    {
        size_t len = data_len < buf_size ? data_len : buf_size;
        len = file.read(wav_data[idx], len);
        data_len -= len;

        if (flg_16bit)
        {
            M5.Speaker.playRaw((const int16_t *)wav_data[idx], len >> 1, wav_header.sample_rate, wav_header.channel > 1, 1, 0);
        }
        else
        {
            M5.Speaker.playRaw((const uint8_t *)wav_data[idx], len, wav_header.sample_rate, wav_header.channel > 1, 1, 0);
        }
        idx = idx < (buf_num - 1) ? idx + 1 : 0;
    }

    file.close();
    return true;
}

void loop()
{
    playWavFile("/test_voice.wav");
    delay(500);
}
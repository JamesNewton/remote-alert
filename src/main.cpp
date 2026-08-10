#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#define SAMPLE_RATE 22050
#define DMA_BUF_LEN 256

enum SoundType { SOUND_NONE, SOUND_DOORBELL };
volatile SoundType currentSound = SOUND_NONE;
volatile uint32_t soundSampleIndex = 0;

void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT, // Maps specifically to GPIO25 (DAC1)
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = DMA_BUF_LEN,
        .use_apll = false
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_dac_mode(I2S_DAC_CHANNEL_RIGHT_EN); // Enables GPIO25 (DAC1)
}

// Synthesis task pinned to Core 1
void AudioTask(void *pvParameters) {
    uint16_t buffer[DMA_BUF_LEN];
    size_t bytes_written;

    while (true) {
        for (int i = 0; i < DMA_BUF_LEN; i++) {
            float outSample = 0.0f;

            if (currentSound == SOUND_DOORBELL) {
                float t = (float)soundSampleIndex / SAMPLE_RATE;
                // "Ding" (E5, ~659Hz) decaying over 1.5s
                if (t < 1.5f) {
                    outSample += sinf(2.0f * M_PI * 659.25f * t) * expf(-3.0f * t);
                }
                // "Dong" (C5, ~523Hz) triggering at 0.4s and decaying over 2.0s
                if (t > 0.4f && t < 2.4f) {
                    float t2 = t - 0.4f;
                    outSample += sinf(2.0f * M_PI * 523.25f * t2) * expf(-2.0f * t2);
                }

                soundSampleIndex++;
                // Stop generating once the tail dies out
                if (t > 2.5f) { currentSound = SOUND_NONE; }
            }

            // Scale float signal with headroom to prevent clipping, then map to 8-bit DAC offset
            uint8_t dac8 = (uint8_t)(constrain(
                (outSample * 0.4f + 0.5f) * 255.0f, 0.0f, 255.0f)
            );
            // Shift to upper byte for 16-bit I2S buffer format
            buffer[i] = ((uint16_t)dac8) << 8;
        }

        // Push samples to DMA queue
        i2s_write(I2S_NUM_0, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n ESP32 Doorbell Synth Test Loop ");
    setupI2S();
    // Spawn synthesis task on Core 1 at high priority (5)
    xTaskCreatePinnedToCore(AudioTask, "AudioTask", 4096, NULL, 5, NULL, 1);
}

void loop() {
    Serial.println("-> Triggering Doorbell Chime...");
    
    // Reset playhead and set active sound
    soundSampleIndex = 0;
    currentSound = SOUND_DOORBELL;

    // Wait 4 seconds before triggering again
    delay(4000);
}
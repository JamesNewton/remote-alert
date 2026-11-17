#include <Arduino.h>
#include <math.h>
#include "AudioTools.h"

#define SAMPLE_RATE 22050
#define DMA_BUF_LEN 256

AnalogAudioStream out;

enum SoundType { 
    SOUND_NONE, 
    SOUND_DOORBELL,
    SOUND_ALERT,
    SOUND_BIRDSONG
};

volatile SoundType currentSound = SOUND_NONE;
volatile uint32_t soundSampleIndex = 0;

// Birdsong chirp configuration
#define MAX_BIRD_CHIRPS 8
struct BirdChirp {
    uint32_t startSample;
    uint32_t attackEndSample;
    uint32_t endSample;
    float startFreq;
    float freqMult;
    float attackStep;
    float decayRate;
};

BirdChirp birdChirps[MAX_BIRD_CHIRPS];
int birdTotalChirps = 0;

void setupAudio() {
    auto config = out.defaultConfig(TX_MODE);
    config.sample_rate = SAMPLE_RATE;
    config.channels = 1;
    config.bits_per_sample = 16;
    out.begin(config);
}

/*
 * =========================================================================================
 * PERFORMANCE CRITICAL WARNING FOR AUDIO SYNTHESIS:
 * Do NOT call transcendental math functions (such as expf() or powf()) inside the per-sample
 * audio synthesis loop! The ESP32 does not have hardware instructions for them in its FPU.
 * Calling them multiple times per sample at 22,050 Hz will starve the I2S DMA buffers,
 * causing severe audio stuttering and dropouts.
 *
 * Instead:
 * - Calculate per-sample multiplier constants ONCE upfront:
 *       decayMultiplier = expf(-decayRate / SAMPLE_RATE);
 * - Update the envelope inside the per-sample loop with a single fast multiply:
 *       envelope *= decayMultiplier;
 * =========================================================================================
 */

// Synthesis task pinned to Core 1
void AudioTask(void *pvParameters) {
    int16_t buffer[DMA_BUF_LEN];
    
    // Doorbell state (Ding = E5 / 659.25Hz, Dong = C5 / 523.25Hz)
    const float stepDing = 2.0f * M_PI * 659.25f / SAMPLE_RATE;
    const float stepDong = 2.0f * M_PI * 523.25f / SAMPLE_RATE;
    const float decayDing = expf(-3.0f / SAMPLE_RATE);
    const float decayDong = expf(-2.0f / SAMPLE_RATE);
    float bellPhaseDing = 0.0f;
    float bellPhaseDong = 0.0f;
    float bellEnvDing = 0.0f;
    float bellEnvDong = 0.0f;

    // Alert beep state
    const float stepAlert1 = 2.0f * M_PI * 880.0f / SAMPLE_RATE;
    const float stepAlert2 = 2.0f * M_PI * 1320.0f / SAMPLE_RATE;
    float alertPhase = 0.0f;

    // Birdsong state
    float birdPhase = 0.0f;
    float birdFreq = 0.0f;
    float birdGain = 0.0f;
    int birdCurrentChirp = 0;

    while (true) {
        for (int i = 0; i < DMA_BUF_LEN; i++) {
            float outSample = 0.0f;

            if (currentSound == SOUND_DOORBELL) {
                if (soundSampleIndex == 0) {
                    bellPhaseDing = 0.0f;
                    bellEnvDing = 1.0f;
                    bellPhaseDong = 0.0f;
                    bellEnvDong = 0.0f;
                }

                // Ding (E5, 659Hz) active from 0.0s to 1.5s
                float note1 = 0.0f;
                if (soundSampleIndex < (uint32_t)(1.5f * SAMPLE_RATE)) {
                    note1 = sinf(bellPhaseDing) * bellEnvDing;
                    bellEnvDing *= decayDing;
                    bellPhaseDing += stepDing;
                    if (bellPhaseDing >= 2.0f * M_PI) bellPhaseDing -= 2.0f * M_PI;
                }

                // Dong (C5, 523Hz) triggers at 0.4s and runs to 2.4s
                float note2 = 0.0f;
                const uint32_t dongStart = (uint32_t)(0.4f * SAMPLE_RATE);
                const uint32_t dongEnd   = (uint32_t)(2.4f * SAMPLE_RATE);

                if (soundSampleIndex >= dongStart && soundSampleIndex < dongEnd) {
                    if (soundSampleIndex == dongStart) {
                        bellPhaseDong = 0.0f;
                        bellEnvDong = 1.0f;
                    }
                    note2 = sinf(bellPhaseDong) * bellEnvDong;
                    bellEnvDong *= decayDong;
                    bellPhaseDong += stepDong;
                    if (bellPhaseDong >= 2.0f * M_PI) bellPhaseDong -= 2.0f * M_PI;
                }

                // Mixed with 50% headroom each to completely prevent clipping
                outSample = 0.5f * note1 + 0.5f * note2;
                soundSampleIndex = soundSampleIndex + 1;
                if (soundSampleIndex > (uint32_t)(2.5f * SAMPLE_RATE)) {
                    currentSound = SOUND_NONE;
                }
            } else if (currentSound == SOUND_ALERT) {
                // Two crisp notification beeps: 880Hz then 1320Hz
                float t = (float)soundSampleIndex / SAMPLE_RATE;
                if (t < 0.12f) {
                    outSample = sinf(alertPhase) * 0.7f;
                    alertPhase += stepAlert1;
                    if (alertPhase >= 2.0f * M_PI) alertPhase -= 2.0f * M_PI;
                } else if (t > 0.18f && t < 0.32f) {
                    outSample = sinf(alertPhase) * 0.7f;
                    alertPhase += stepAlert2;
                    if (alertPhase >= 2.0f * M_PI) alertPhase -= 2.0f * M_PI;
                }

                soundSampleIndex = soundSampleIndex + 1;
                if (t >= 0.35f) {
                    currentSound = SOUND_NONE;
                    alertPhase = 0.0f;
                }
            } else if (currentSound == SOUND_BIRDSONG) {
                if (soundSampleIndex == 0) {
                    birdPhase = 0.0f;
                    birdCurrentChirp = 0;
                    birdGain = 0.0f;
                    if (birdTotalChirps > 0) {
                        birdFreq = birdChirps[0].startFreq;
                    }
                }

                // Advance to next chirp if current one ended
                while (birdCurrentChirp < birdTotalChirps && soundSampleIndex >= birdChirps[birdCurrentChirp].endSample) {
                    birdCurrentChirp++;
                    if (birdCurrentChirp < birdTotalChirps) {
                        birdFreq = birdChirps[birdCurrentChirp].startFreq;
                        birdGain = 0.0f;
                    }
                }

                // Synthesize active chirp
                if (birdCurrentChirp < birdTotalChirps && soundSampleIndex >= birdChirps[birdCurrentChirp].startSample) {
                    BirdChirp &c = birdChirps[birdCurrentChirp];
                    if (soundSampleIndex < c.attackEndSample) {
                        birdGain += c.attackStep;
                    } else {
                        birdGain *= c.decayRate;
                    }

                    birdFreq *= c.freqMult;
                    birdPhase += 2.0f * M_PI * birdFreq / SAMPLE_RATE;
                    if (birdPhase >= 2.0f * M_PI) birdPhase -= 2.0f * M_PI;

                    outSample = sinf(birdPhase) * birdGain * 0.7f;
                }

                soundSampleIndex = soundSampleIndex + 1;
                if (birdTotalChirps > 0 && soundSampleIndex > birdChirps[birdTotalChirps - 1].endSample + (uint32_t)(0.1f * SAMPLE_RATE)) {
                    currentSound = SOUND_NONE;
                }
            }

            // Convert [-1.0, 1.0] float signal to 16-bit signed PCM
            buffer[i] = (int16_t)constrain(outSample * 32767.0f, -32768.0f, 32767.0f);
        }

        // Push samples to analog audio stream (internal DAC)
        out.write((const uint8_t *)buffer, sizeof(buffer));
    }
}

// Generates randomized chirps for the Birdsong effect (executed once outside the audio task)
void triggerBirdSong() {
    birdTotalChirps = 4 + (rand() % 4); // 4 to 7 chirps
    uint32_t curSample = (uint32_t)(0.05f * SAMPLE_RATE); // 50ms initial pause

    for (int i = 0; i < birdTotalChirps; i++) {
        // Base frequency between 2800Hz and 6300Hz
        float baseFreq = 2800.0f + ((float)rand() / RAND_MAX) * 3500.0f;
        // Frequency sweep (+/- 2000Hz)
        float sweepAmount = (((float)rand() / RAND_MAX) - 0.5f) * 4000.0f;
        float endFreq = baseFreq + sweepAmount;
        if (endFreq < 800.0f) endFreq = 800.0f;
        if (endFreq > 8000.0f) endFreq = 8000.0f;

        // Duration between 30ms and 270ms
        float duration = 0.03f + ((float)rand() / RAND_MAX) * 0.24f;
        uint32_t durationSamples = (uint32_t)(duration * SAMPLE_RATE);
        if (durationSamples < 20) durationSamples = 20;

        uint32_t attackSamples = (uint32_t)(durationSamples * 0.2f);
        if (attackSamples < 5) attackSamples = 5;
        uint32_t decaySamples = durationSamples - attackSamples;

        birdChirps[i].startSample = curSample;
        birdChirps[i].attackEndSample = curSample + attackSamples;
        birdChirps[i].endSample = curSample + durationSamples;
        birdChirps[i].startFreq = baseFreq;
        // Multiplier applied per sample to exponentially ramp frequency
        birdChirps[i].freqMult = powf(endFreq / baseFreq, 1.0f / (float)durationSamples);
        birdChirps[i].attackStep = 1.0f / (float)attackSamples;
        birdChirps[i].decayRate = powf(0.001f, 1.0f / (float)decaySamples);

        // Random pause between chirps (20ms to 170ms)
        float gap = 0.02f + ((float)rand() / RAND_MAX) * 0.15f;
        curSample = birdChirps[i].endSample + (uint32_t)(gap * SAMPLE_RATE);
    }

    soundSampleIndex = 0;
    currentSound = SOUND_BIRDSONG;
}

void printMenu() {
    Serial.println("\n=== Remote Alert Sound Menu ===");
    Serial.println(" [1] Play Doorbell (Ding-Dong)");
    Serial.println(" [2] Play Double Alert Beep");
    Serial.println(" [3] Play Random Birdsong (Synthesized Chirps)");
    Serial.println(" [0] Stop / Silence");
    Serial.println("-------------------------------");
    Serial.print("Enter choice: ");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== ESP32 Remote Alert Initialized ===");
    setupAudio();
    xTaskCreatePinnedToCore(AudioTask, "AudioTask", 4096, NULL, 5, NULL, 1);
    printMenu();
}

void loop() {
    if (Serial.available() > 0) {
        char ch = Serial.read();
        if (ch == '\r' || ch == '\n' || ch == ' ') return;

        switch (ch) {
            case '1':
                Serial.println("1 -> Triggering Doorbell (Ding-Dong)...");
                soundSampleIndex = 0;
                currentSound = SOUND_DOORBELL;
                break;
            case '2':
                Serial.println("2 -> Triggering Alert Beep...");
                soundSampleIndex = 0;
                currentSound = SOUND_ALERT;
                break;
            case '3':
                Serial.println("3 -> Triggering Random Birdsong...");
                triggerBirdSong();
                break;
            case '0':
            case 's':
            case 'S':
                Serial.println("0 -> Stopped / Silence.");
                currentSound = SOUND_NONE;
                break;
            default:
                printMenu();
                break;
        }
    }
    delay(10);
}

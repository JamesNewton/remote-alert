#include <Arduino.h>
#include <math.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "AudioTools.h"
#include "FileSystems.h"
#include "espeak.h"
#include "config.h"

#define SAMPLE_RATE 22050
#define DMA_BUF_LEN 256
#define TTS_MAX_LEN 256
#define TTS_QUEUE_LEN 4
#define DAC_OUT_PIN 25
#define IDLE_MUTE_DELAY_MS 500
#define RAMP_DOWN_FRAMES 2

AnalogAudioStream out;
AnalogConfig audioConfig;
bool audioActive = false;

void clampDacLow() { //to avoid noise when the DAC is idle
    pinMode(DAC_OUT_PIN, OUTPUT);
    digitalWrite(DAC_OUT_PIN, LOW);
}

void restartAudio() {
    Serial.println("[Audio] Restarting AnalogAudioStream...");
    out.end();
    delay(10);
    out.begin(audioConfig);
    audioActive = true;
}

void wakeAudio() {
    if (audioActive) return;

    out.begin(audioConfig);
    audioActive = true;

    // Short ~11.6 ms linear ramp from 0V (-32768) to mid-scale (0) to avoid turn-on pop
    int16_t ramp[DMA_BUF_LEN];
    for (int i = 0; i < DMA_BUF_LEN; i++) {
        float frac = (float)(i + 1) / (float)DMA_BUF_LEN;
        ramp[i] = (int16_t)(-32768.0f * (1.0f - frac));
    }
    out.write((const uint8_t *)ramp, sizeof(ramp));
}

void muteAudio() {
    if (!audioActive) return;

    int16_t frame[DMA_BUF_LEN];
    const int totalSamples = RAMP_DOWN_FRAMES * DMA_BUF_LEN;

    // Gentle ~23.2 ms linear ramp from mid-scale (0) down to 0V (-32768)
    for (int f = 0; f < RAMP_DOWN_FRAMES; f++) {
        for (int i = 0; i < DMA_BUF_LEN; i++) {
            float frac = (float)(f * DMA_BUF_LEN + i + 1) / (float)totalSamples;
            frame[i] = (int16_t)(-32768.0f * frac);
        }
        out.write((const uint8_t *)frame, sizeof(frame));
    }

    // Push 0V frames to drain queued DMA descriptors before disabling the DAC
    // (Refill each iteration because AnalogDriverESP32V1 converts the buffer in-place)
    for (int f = 0; f < audioConfig.buffer_count; f++) {
        for (int i = 0; i < DMA_BUF_LEN; i++) {
            frame[i] = -32768;
        }
        out.write((const uint8_t *)frame, sizeof(frame));
    }

    out.end();
    clampDacLow();
    audioActive = false;
}

// Buffers TTS audio into exact DMA_BUF_LEN-sample (512-byte) frames so
// AnalogAudioStream (dac_continuous) never receives tiny/odd-sized writes
// and eSpeak's internal buffer is never mutated in-place.
class TtsOutAdapter : public Print {
public:
    static constexpr size_t FRAME_BYTES = DMA_BUF_LEN * sizeof(int16_t);

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t *buffer, size_t size) override {
        size_t offset = 0;
        while (offset < size) {
            size_t space = FRAME_BYTES - pendingLen;
            size_t toCopy = (size - offset < space) ? (size - offset) : space;
            memcpy(pending + pendingLen, buffer + offset, toCopy);
            pendingLen += toCopy;
            offset += toCopy;

            if (pendingLen == FRAME_BYTES) {
                if (!writeFrame(pending)) {
                    return offset;
                }
                pendingLen = 0;
            }
        }
        return offset;
    }

    void flushFrame() {
        if (pendingLen > 0) {
            memset(pending + pendingLen, 0, FRAME_BYTES - pendingLen);
            writeFrame(pending);
            pendingLen = 0;
        }
    }

private:
    uint8_t pending[FRAME_BYTES] = {0};
    size_t pendingLen = 0;

    bool writeFrame(const uint8_t *frame) {
        size_t written = out.write(frame, FRAME_BYTES);
        if (written == 0) {
            Serial.println("[TTS-OUT] ERROR: out.write() timed out, recovering audio...");
            restartAudio();
            written = out.write(frame, FRAME_BYTES);
        }
        return written > 0;
    }
};
TtsOutAdapter ttsOut;
ESpeak espeak(ttsOut);
bool espeakReady = false;

struct TtsMessage {
    char text[TTS_MAX_LEN];
};
QueueHandle_t ttsQueue = nullptr;

// Network & MQTT objects
WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttReconnectAttempt = 0;
bool wifiConnectedReported = false;

enum SoundType { 
    SOUND_NONE, 
    SOUND_DOORBELL,
    SOUND_ALERT,
    SOUND_BIRDSONG,
    SOUND_CRICKET,
    SOUND_CHICKEN
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

// Cricket chirp configuration
#define MAX_CRICKET_PULSES 16
struct CricketPulse {
    uint32_t startSample;
    uint32_t attackEndSample;
    uint32_t decayStartSample;
    uint32_t endSample;
    float attackStep;
    float decayStep;
};

CricketPulse cricketPulses[MAX_CRICKET_PULSES];
int cricketTotalPulses = 0;
float cricketPhaseStep = 2.0f * M_PI * 4300.0f / SAMPLE_RATE;

// Chicken cluck configuration (ported from Web Audio dual-triangle + 3kHz bandpass)
#define MAX_CHICKEN_CLUCKS 8
struct ChickenCluck {
    uint32_t startSample;
    uint32_t peakSample;
    uint32_t attackEndSample;
    uint32_t endSample;
    float freq;
    float freqRiseMult;
    float freqFallMult;
    float gain;
    float attackStep;
    float decayMult;
    float phase1;
    float phase2;
    float z1;
    float z2;
};

ChickenCluck chickenClucks[MAX_CHICKEN_CLUCKS];
int chickenTotalClucks = 0;
uint32_t chickenEndSample = 0;

void setupAudio() {
    audioConfig = out.defaultConfig(TX_MODE);
    audioConfig.sample_rate = SAMPLE_RATE;
    audioConfig.channels = 1;
    audioConfig.bits_per_sample = 16;
    audioConfig.timeout = pdMS_TO_TICKS(500);
    clampDacLow();
    Serial.printf("[Audio] AnalogAudioStream configured (idle-muted, buf_size=%d, buf_count=%d)\n",
                  audioConfig.buffer_size, audioConfig.buffer_count);

    espeakReady = espeak.begin(50);
    if (espeakReady) {
        espeak.setVoice("en");
        Serial.println("[TTS] eSpeak NG initialized.");
    } else {
        Serial.println("[TTS] ERROR: eSpeak NG failed to initialize!");
    }
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

    // Cricket state
    float cricketPhase = 0.0f;
    float cricketEnv = 0.0f;
    int cricketCurrentPulse = 0;

    // Chicken bandpass biquad coefficients (fc = 3000Hz, Q = 2.0, Web Audio constant-0dB bandpass)
    const float bpW0 = 2.0f * M_PI * 3000.0f / SAMPLE_RATE;
    const float bpAlpha = sinf(bpW0) / (2.0f * 2.0f);
    const float bpA0 = 1.0f + bpAlpha;
    const float bpB0 = bpAlpha / bpA0;
    const float bpB2 = -bpB0;
    const float bpA1 = (-2.0f * cosf(bpW0)) / bpA0;
    const float bpA2 = (1.0f - bpAlpha) / bpA0;
    const float chickenDetuneRatio = powf(2.0f, 15.0f / 1200.0f); // +15 cents
    const float invSampleRate = 1.0f / SAMPLE_RATE;

    unsigned long idleSinceMs = 0;

    while (true) {
        // Process any queued TTS message exclusively from AudioTask to avoid concurrent out.write()
        TtsMessage ttsMsg;
        if (ttsQueue != nullptr && xQueueReceive(ttsQueue, &ttsMsg, 0) == pdTRUE) {
            currentSound = SOUND_NONE;
            if (espeakReady) {
                wakeAudio();
                Serial.printf("[TTS] Speaking: %s (Stack HWM: %u, Free Heap: %u)\n",
                              ttsMsg.text,
                              (unsigned int)uxTaskGetStackHighWaterMark(NULL),
                              (unsigned int)ESP.getFreeHeap());
                bool ok = espeak.say(ttsMsg.text);
                ttsOut.flushFrame();
                Serial.printf("[TTS] Finished (ok=%d, Stack HWM: %u, Free Heap: %u)\n",
                              ok ? 1 : 0,
                              (unsigned int)uxTaskGetStackHighWaterMark(NULL),
                              (unsigned int)ESP.getFreeHeap());
                if (!ok) {
                    Serial.println("[TTS] ERROR: espeak.say() failed.");
                }
                idleSinceMs = millis();
            }
            continue;
        }

        if (currentSound == SOUND_NONE) {
            if (audioActive) {
                if ((millis() - idleSinceMs) >= IDLE_MUTE_DELAY_MS) {
                    muteAudio();
                } else {
                    memset(buffer, 0, sizeof(buffer));
                    if (out.write((const uint8_t *)buffer, sizeof(buffer)) == 0) {
                        Serial.println("[Audio] ERROR: out.write() timed out, recovering audio...");
                        restartAudio();
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            continue;
        }

        wakeAudio();

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
            } else if (currentSound == SOUND_CRICKET) {
                if (soundSampleIndex == 0) {
                    cricketPhase = 0.0f;
                    cricketEnv = 0.0f;
                    cricketCurrentPulse = 0;
                }

                while (cricketCurrentPulse < cricketTotalPulses && soundSampleIndex >= cricketPulses[cricketCurrentPulse].endSample) {
                    cricketCurrentPulse++;
                    cricketEnv = 0.0f;
                }

                if (cricketCurrentPulse < cricketTotalPulses && soundSampleIndex >= cricketPulses[cricketCurrentPulse].startSample) {
                    CricketPulse &p = cricketPulses[cricketCurrentPulse];
                    if (soundSampleIndex < p.attackEndSample) {
                        cricketEnv += p.attackStep;
                        if (cricketEnv > 1.0f) cricketEnv = 1.0f;
                    } else if (soundSampleIndex >= p.decayStartSample) {
                        cricketEnv -= p.decayStep;
                        if (cricketEnv < 0.0f) cricketEnv = 0.0f;
                    }

                    cricketPhase += cricketPhaseStep;
                    if (cricketPhase >= 2.0f * M_PI) cricketPhase -= 2.0f * M_PI;
                    outSample = sinf(cricketPhase) * cricketEnv * 0.02f;//0.65f;
                }

                soundSampleIndex = soundSampleIndex + 1;
                if (cricketTotalPulses > 0 && soundSampleIndex > cricketPulses[cricketTotalPulses - 1].endSample + (uint32_t)(0.05f * SAMPLE_RATE)) {
                    currentSound = SOUND_NONE;
                }
            } else if (currentSound == SOUND_CHICKEN) {
                float mix = 0.0f;
                for (int cIdx = 0; cIdx < chickenTotalClucks; cIdx++) {
                    ChickenCluck &c = chickenClucks[cIdx];
                    if (soundSampleIndex < c.startSample || soundSampleIndex >= c.endSample) {
                        continue;
                    }

                    // Dual detuned triangle oscillators
                    float tri1 = (c.phase1 < 0.5f) ? (4.0f * c.phase1 - 1.0f) : (3.0f - 4.0f * c.phase1);
                    float tri2 = (c.phase2 < 0.5f) ? (4.0f * c.phase2 - 1.0f) : (3.0f - 4.0f * c.phase2);
                    float x = tri1 + tri2;

                    // 3kHz Q=2 bandpass biquad (Transposed Direct Form II)
                    float y = bpB0 * x + c.z1;
                    c.z1 = -bpA1 * y + c.z2;
                    c.z2 = bpB2 * x - bpA2 * y;

                    mix += y * c.gain;

                    // Advance oscillator phases
                    c.phase1 += c.freq * invSampleRate;
                    if (c.phase1 >= 1.0f) c.phase1 -= 1.0f;
                    c.phase2 += c.freq * chickenDetuneRatio * invSampleRate;
                    if (c.phase2 >= 1.0f) c.phase2 -= 1.0f;

                    // Exponential pitch profile (rise to 2.2x, then drop to 0.7x)
                    if (soundSampleIndex < c.peakSample) {
                        c.freq *= c.freqRiseMult;
                    } else {
                        c.freq *= c.freqFallMult;
                    }

                    // Snappy linear attack + exponential decay envelope
                    if (soundSampleIndex < c.attackEndSample) {
                        c.gain += c.attackStep;
                    } else {
                        c.gain *= c.decayMult;
                    }
                }

                // Makeup gain so filtered upper harmonics match the volume of other effects
                outSample = mix * 2.5f;
                soundSampleIndex = soundSampleIndex + 1;
                if (soundSampleIndex > chickenEndSample) {
                    currentSound = SOUND_NONE;
                }
            }

            // Convert [-1.0, 1.0] float signal to 16-bit signed PCM
            buffer[i] = (int16_t)constrain(outSample * 32767.0f, -32768.0f, 32767.0f);
        }

        // Push samples to analog audio stream (internal DAC)
        if (out.write((const uint8_t *)buffer, sizeof(buffer)) == 0) {
            Serial.println("[Audio] ERROR: out.write() timed out, recovering audio...");
            restartAudio();
        }
        idleSinceMs = millis();
    }
}

void triggerDoorBell() {
    Serial.println("Doorbell");
    soundSampleIndex = 0;
    currentSound = SOUND_DOORBELL;
}

void triggerAlert() {
    Serial.println("Alert");
    soundSampleIndex = 0;
    currentSound = SOUND_ALERT;
}

// Generates randomized chirps for the Birdsong effect (executed once outside the audio task)
void triggerBirdSong() {
    Serial.println("Birdsong");
    currentSound = SOUND_NONE; // Silence audio briefly while recomputing parameters
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

// Generates a realistic field-cricket chirp burst (2-3 chirps of 3-4 rapid syllables)
void triggerCricket() {
    Serial.println("Cricket");
    currentSound = SOUND_NONE;

    float carrierFreq = 4200.0f + ((float)rand() / RAND_MAX) * 300.0f;
    cricketPhaseStep = 2.0f * M_PI * carrierFreq / SAMPLE_RATE;

    int chirpCount = 1 + (rand() % 2); // 1 to 2 chirps
    int pulsesPerChirp = 3 + (rand() % 2); // 3 to 4 syllables per chirp
    if (chirpCount * pulsesPerChirp > MAX_CRICKET_PULSES) {
        pulsesPerChirp = MAX_CRICKET_PULSES / chirpCount;
    }

    const uint32_t pulseSamples = (uint32_t)(0.018f * SAMPLE_RATE); // 18ms syllable
    const uint32_t attackSamples = (uint32_t)(0.003f * SAMPLE_RATE); // 3ms ramp up
    const uint32_t decaySamples = (uint32_t)(0.005f * SAMPLE_RATE); // 5ms ramp down
    const uint32_t pulseGapSamples = (uint32_t)(0.014f * SAMPLE_RATE); // 14ms between syllables
    const uint32_t chirpGapSamples = (uint32_t)(0.200f * SAMPLE_RATE); // 200ms between chirps

    uint32_t curSample = (uint32_t)(0.02f * SAMPLE_RATE);
    int idx = 0;
    for (int c = 0; c < chirpCount; c++) {
        for (int p = 0; p < pulsesPerChirp; p++) {
            CricketPulse &pulse = cricketPulses[idx++];
            pulse.startSample = curSample;
            pulse.attackEndSample = curSample + attackSamples;
            pulse.decayStartSample = curSample + pulseSamples - decaySamples;
            pulse.endSample = curSample + pulseSamples;
            pulse.attackStep = 1.0f / (float)attackSamples;
            pulse.decayStep = 1.0f / (float)decaySamples;
            curSample = pulse.endSample + pulseGapSamples;
        }
        curSample += chirpGapSamples;
    }

    cricketTotalPulses = idx;
    soundSampleIndex = 0;
    currentSound = SOUND_CRICKET;
}

// Generates a randomized burst of angry chicken clucks (5-6 clucks)
void triggerChicken() {
    Serial.println("Chicken");
    currentSound = SOUND_NONE;

    int burstCount = 5 + (rand() % 2); // 5 to 6 clucks
    if (burstCount > MAX_CHICKEN_CLUCKS) burstCount = MAX_CHICKEN_CLUCKS;

    uint32_t curSample = (uint32_t)(0.02f * SAMPLE_RATE);
    uint32_t maxEnd = curSample;

    for (int i = 0; i < burstCount; i++) {
        ChickenCluck &c = chickenClucks[i];
        float baseFreq = 400.0f + ((float)rand() / RAND_MAX) * 100.0f;
        float duration = 0.06f + ((float)rand() / RAND_MAX) * 0.04f;
        uint32_t durationSamples = (uint32_t)(duration * SAMPLE_RATE);
        if (durationSamples < 200) durationSamples = 200;

        uint32_t riseSamples = (uint32_t)(durationSamples * 0.3f);
        if (riseSamples < 10) riseSamples = 10;
        uint32_t fallSamples = durationSamples - riseSamples;
        if (fallSamples < 10) fallSamples = 10;

        uint32_t attackSamples = (uint32_t)(0.005f * SAMPLE_RATE);
        if (attackSamples < 5) attackSamples = 5;
        if (attackSamples >= durationSamples) attackSamples = durationSamples / 2;
        uint32_t decaySamples = durationSamples - attackSamples;

        c.startSample = curSample;
        c.peakSample = curSample + riseSamples;
        c.attackEndSample = curSample + attackSamples;
        c.endSample = curSample + durationSamples;
        c.freq = baseFreq;
        c.freqRiseMult = powf(2.2f, 1.0f / (float)riseSamples);
        c.freqFallMult = powf(0.7f / 2.2f, 1.0f / (float)fallSamples);
        c.gain = 0.0f;
        c.attackStep = 0.5f / (float)attackSamples;
        c.decayMult = powf(0.001f / 0.5f, 1.0f / (float)decaySamples);
        c.phase1 = 0.0f;
        c.phase2 = 0.0f;
        c.z1 = 0.0f;
        c.z2 = 0.0f;

        if (c.endSample > maxEnd) maxEnd = c.endSample;

        float spacing = 0.08f + ((float)rand() / RAND_MAX) * 0.12f;
        curSample += (uint32_t)(spacing * SAMPLE_RATE);
    }

    chickenTotalClucks = burstCount;
    chickenEndSample = maxEnd + (uint32_t)(0.05f * SAMPLE_RATE);
    soundSampleIndex = 0;
    currentSound = SOUND_CHICKEN;
}

void enqueueTTS(const char* text, unsigned int length) {
    if (ttsQueue == nullptr || !espeakReady || text == nullptr || length == 0) return;

    TtsMessage msg;
    unsigned int copyLen = length < (TTS_MAX_LEN - 1) ? length : (TTS_MAX_LEN - 1);
    unsigned int outIdx = 0;
    for (unsigned int i = 0; i < copyLen; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            msg.text[outIdx++] = ' ';
        } else if (c >= 32 && c != 127) {
            msg.text[outIdx++] = (char)c;
        }
    }
    while (outIdx > 0 && msg.text[outIdx - 1] == ' ') {
        outIdx--;
    }
    msg.text[outIdx] = '\0';

    if (outIdx == 0) return;

    if (xQueueSend(ttsQueue, &msg, 0) != pdTRUE) {
        Serial.println("[TTS] WARNING: TTS queue full, dropping message.");
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] Message received on topic: %s (len: %u)\n", topic, length);
    if (length > 0) {
        Serial.print("[MQTT] Payload: ");
        for (unsigned int i = 0; i < length && i < 128; i++) {
            Serial.print((char)payload[i]);
        }
        if (length > 128) Serial.print("...");
        Serial.println();
    }

    if (strcmp(topic, MQTT_TOPIC_TTS) == 0) {
        Serial.println("[MQTT] -> TTS topic matched, queuing speech.");
        enqueueTTS((const char*)payload, length);
        return;
    } 
    /* else if (strncmp(topic, MQTT_TOPIC, sizeof(MQTT_TOPIC) - 1) == 0) {
        Serial.println("[MQTT] -> Topic matches.");
        char payload_str[16];
        if (length >= sizeof(payload_str)) {
            length = sizeof(payload_str) - 1; 
        }
        memcpy(payload_str, payload, length);
        payload_str[length] = '\0'; // CRITICAL: This makes it a valid C-string
        if (atol(payload_str) < 70) return;
        Serial.println("[MQTT] Triggering Birdsong!");
        triggerBirdSong();
    } */

    static const std::unordered_map<std::string, std::function<void()>> commands = {
        {MQTT_TOPIC_DOORBELL, triggerDoorBell},
        {MQTT_TOPIC_ALERT, triggerAlert},
        {MQTT_TOPIC_BIRDSONG, triggerBirdSong},
        {MQTT_TOPIC_CRICKET, triggerCricket},
        {MQTT_TOPIC_CHICKEN, triggerChicken}};

    auto command = commands.find(topic);
    if (command != commands.end()) {
        command->second();
        return;
    }

}

void setupWiFi() {
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("[WiFi] No SSID configured. Skipping Wi-Fi.");
        return;
    }
    Serial.printf("[WiFi] Connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); //power isn't an issue. Avoid spikes which cause audio pops.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void handleNetwork() {
    if (strlen(WIFI_SSID) == 0) return;

    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiConnectedReported) {
            wifiConnectedReported = true;
            Serial.print("[WiFi] Connected! IP Address: ");
            Serial.println(WiFi.localIP());
        }

        if (!mqttClient.connected()) {
            unsigned long now = millis();
            if (now - lastMqttReconnectAttempt > 5000) {
                lastMqttReconnectAttempt = now;
                Serial.printf("[MQTT] Connecting to %s:%d...\n", MQTT_BROKER, MQTT_PORT);

                bool connected = false;
                if (strlen(MQTT_USER) > 0) {
                    connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD);
                } else {
                    connected = mqttClient.connect(MQTT_CLIENT_ID);
                }

                if (connected) {
                    Serial.println("[MQTT] Connected successfully!");
                    if (mqttClient.subscribe(MQTT_TOPIC_SUB)) {
                        Serial.printf("[MQTT] Subscribed to topic: %s\n", MQTT_TOPIC_SUB);
                    } else {
                        Serial.printf("[MQTT] Failed to subscribe to topic: %s\n", MQTT_TOPIC_SUB);
                    }
                    if (mqttClient.subscribe(MQTT_TOPIC_TTS)) {
                        Serial.printf("[MQTT] Subscribed to topic: %s\n", MQTT_TOPIC_TTS);
                    } else {
                        Serial.printf("[MQTT] Failed to subscribe to topic: %s\n", MQTT_TOPIC_TTS);
                    }
                } else {
                    Serial.printf("[MQTT] Connect failed, state=%d. Retrying in 5s...\n", mqttClient.state());
                }
            }
        } else {
            mqttClient.loop();
        }
    } else {
        if (wifiConnectedReported) {
            wifiConnectedReported = false;
            Serial.println("[WiFi] Connection lost. Reconnecting...");
        }
    }
}

void printNetworkStatus() {
    Serial.println("\n--- Network Status ---");
    Serial.printf("WiFi SSID: %s\n", WIFI_SSID);
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi Status: Connected (IP: %s, RSSI: %d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.printf("WiFi Status: Disconnected (%d)\n", WiFi.status());
    }
    Serial.printf("MQTT Broker: %s:%d\n", MQTT_BROKER, MQTT_PORT);
    Serial.printf("MQTT Status: %s\n", mqttClient.connected() ? "Connected" : "Disconnected");
    Serial.printf("MQTT Subscribed Topics: %s, %s\n", MQTT_TOPIC_SUB, MQTT_TOPIC_TTS);
    Serial.printf("TTS Engine: %s (Free Heap: %u bytes, Largest Block: %u bytes)\n",
                  espeakReady ? "Ready" : "Failed",
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    Serial.println("----------------------");
}

void printMenu() {
    Serial.println("\n=== Remote Alert Sound Menu ===");
    Serial.println(" [1] Play Doorbell (Ding-Dong)");
    Serial.println(" [2] Play Double Alert Beep");
    Serial.println(" [3] Play Random Birdsong (Synthesized Chirps)");
    Serial.println(" [4] Play Cricket");
    Serial.println(" [5] Play Angry Chicken Clucking");
    Serial.println(" [6] Test TTS (\"Alert system ready\")");
    Serial.println(" [S] Network / MQTT Status");
    Serial.println(" [0] Stop / Silence");
    Serial.println("-------------------------------");
    Serial.print("Enter choice: ");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== ESP32 Remote Alert Initialized ===");
    ttsQueue = xQueueCreate(TTS_QUEUE_LEN, sizeof(TtsMessage));
    if (ttsQueue == nullptr) {
        Serial.println("[TTS] ERROR: Failed to create TTS queue!");
    }
    setupAudio();
    xTaskCreatePinnedToCore(AudioTask, "AudioTask", 8192, NULL, 5, NULL, 1);

    setupWiFi();
    mqttClient.setBufferSize(512);
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);

    printMenu();
}

void loop() {
    handleNetwork();

    if (Serial.available() > 0) {
        char ch = Serial.read();
        if (ch == '\r' || ch == '\n' || ch == ' ') return;

        switch (ch) {
            case '1':
                triggerDoorBell();
                break;
            case '2':
                triggerAlert();
                break;
            case '3':
                triggerBirdSong();
                break;
            case '4':
                triggerCricket();
                break;
            case '5':
                triggerChicken();
                break;
            case '6':
                Serial.println("6 -> Queuing TTS test...");
                enqueueTTS("Alert system ready", 18);
                break;
            case '0':
                Serial.println("0 -> Stopped / Silence.");
                currentSound = SOUND_NONE;
                break;
            case 'S':
            case 's':
                printNetworkStatus();
                break;
            default:
                printMenu();
                break;
        }
    }
    delay(10);
}

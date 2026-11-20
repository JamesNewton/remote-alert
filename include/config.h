#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h not found. Please copy include/secrets.h.template to include/secrets.h"
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define MQTT_USER ""
#define MQTT_PASSWORD ""
#endif

// MQTT Broker Configuration
// Hostname "aaeon-desktop" or IP "192.168.0.163"
#define MQTT_BROKER "aaeon-desktop"
#define MQTT_PORT 1883
#define MQTT_CLIENT_ID "ESP32-RemoteAlert"

// MQTT Topic Subscriptions
#define MQTT_TOPIC "sensors/radiation/cpm"
#define MQTT_TOPIC_SUB MQTT_TOPIC "/#"
#define MQTT_TOPIC_TTS "alert/tts"

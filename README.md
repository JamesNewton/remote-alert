
This is a ESP32-WROOM-32 coupled to a cheap bluetooth speaker to make a remote audio alert device. 

- Instead of using pre-recorded audio, it generates sounds onboard via synthesis. 
- The bluetooth function of the speaker isn't used. 

## Install

I wrote this using platformio in vscode with the Arduino ecosystem

If you haven't used platformio in vscode, see: 
https://github.com/platformio/platformio-core-installer/issues/1774 
You have to close VS Code, install this:
`apt-get install python3-venv`
and then start VS Code again and select the PlatformIO extension. Takes bloody forever to install. Be patient. 

Then if you are on linux, you have to setup the udev rules:
```bash
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```
Then you might be able to select the stupid alien cockroach icon from the left and clcik "Upload" under esp32dev / General. 

## Configuration

Copy `include/secrets.h.template` to `include/secrets.h` (this file is git-ignored to prevent credentials from being committed) and enter your Wi-Fi credentials:

- **WIFI_SSID** and **WIFI_PASSWORD**: Your local Wi-Fi credentials.
- **MQTT_USER** and **MQTT_PASSWORD**: Optional MQTT credentials (leave blank if not using broker authentication).

General broker settings and topic subscriptions can be customized in [`include/config.h`](include/config.h):
- **MQTT_BROKER**: Hostname or IP of the MQTT broker (e.g. `aaeon-desktop` or `192.168.0.163`).
- **MQTT_PORT**: Default is `1883`.
- **MQTT_SUB_TOPIC**: Topic filter to subscribe to (defaults to `sensor/#`).

## MQTT Alert Triggering

When connected to Wi-Fi and the MQTT broker:
- The ESP32 subscribes to `sensor/#`.
- Any MQTT message published to a topic starting with `sensor/` automatically triggers the synthesized Birdsong sound effect.
- You can check network and MQTT connection status at any time in the Serial Monitor by pressing `4`.
 

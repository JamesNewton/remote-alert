
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

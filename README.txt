# ONKEY: A 3D-Printed ESP32 Virtual Pet

![ONKEY Placeholder - Replace with a photo of the device]

ONKEY is a physical, wearable digital pet built entirely from scratch. Inspired by the classic Tamagotchi, it runs on modern IoT architecture. Cramming a 24/7 game into a tiny 3D-printed box meant getting creative with power management, hardware space, and firmware logic.

> **Note on Mechanical Assembly:** The provided `.stl` 3D print files are custom-toleranced to my specific point-to-point perfboard soldering. Due to the dense, multi-layered wire routing and lateral component placement (like the piezo buzzer), your physical footprint will vary. Expect to modify the CAD files to fit your specific circuit dimensions.

## The Hardware

To make this pocket-sized, every millimeter of space and milliamp of power matters. 

* **The Brain (ESP32-C3 SuperMini):** Chosen because it is roughly the size of a thumb, has built-in Wi-Fi, and supports extreme deep-sleep modes. I physically removed the board's power LED to eliminate constant parasitic milliamp drain.
* **The Display (0.96" SSD1306 OLED):** This screen uses the I2C protocol. It only requires two data wires (SDA and SCL), which is crucial for saving physical space when routing wires.
* **Power & Charging (TP4056 BMS):** The ESP32 doesn't have a safe, built-in LiPo charger. Routing USB power directly to the ESP32 while a battery is attached creates a fire hazard. Instead, the battery routes through a TP4056 charging board. The 3D case exposes the TP4056’s USB port for daily charging and permanently seals the ESP32’s USB port inside.
* **Battery Telemetry (Voltage Divider):** To let the pet calculate its battery percentage, I built a 100kΩ/100kΩ voltage divider. This safely steps the battery's voltage down to a level the ESP32 can read without actively draining the battery.
* **Audio & Controls:** 3 tactile buttons and a piezo buzzer. I used the ESP32's internal pull-up resistors for the buttons to avoid soldering external ones. I also wired a 10kΩ "bleed" resistor in parallel with the buzzer to ground—this prevents parasitic electricity from causing annoying "ghost beeps" when the pet is sleeping.

## The Software

* **Time-Travel Math (Deep Sleep):** Leaving a processor running 24/7 will kill a small battery in hours. After 15 seconds of inactivity, ONKEY turns off the screen and puts the CPU to sleep. To keep the game going, it leaves its internal clock running. When awoken via a button or an internal timer, the code calculates exactly how much time has passed in microseconds (`getNowUs()`) and retroactively deducts the pet's hunger and happiness all at once.
* **Smoothing the Battery Meter (ADC Filtering):** The voltage divider is highly sensitive to electrical noise. Every time the screen refreshed, the voltage dipped, causing fake "Low Battery" warnings. Since there was no room for physical filtering capacitors in the box, I fixed it in software: the code takes 5 rapid analog readings in 10 milliseconds and averages them out to stabilize the meter.
* **The Single-Use Save File (NVS Memory):** ONKEY updates its firmware wirelessly over Wi-Fi. To prevent losing stats during an update, the game saves to flash memory. However, if the physical battery died naturally, a normal reboot would load that Wi-Fi save file and accidentally resurrect a dead pet! To fix this, the code uses a "Single-Use Checkpoint." It only saves exactly when a Wi-Fi transfer begins, and upon rebooting, it immediately deletes the backup file so actions have permanent consequences again.

## How to Flash / OTA Updates

Because the ESP32's USB port is physically blocked by the enclosure, all firmware updates post-assembly must be done Over-The-Air (OTA).

1. In Arduino IDE, select **Sketch > Export compiled Binary**.
2. Wake up ONKEY and input the Admin sequence: **Press SELECT 3 times, press BACK 4 times, press SCROLL 5 times**.
3. In the Admin Menu, select **Upload OTA**.
4. Connect your PC/Phone to the **ONKEY_UPDATE** Wi-Fi access point.
5. Navigate to `http://192.168.4.1` in your browser and upload the `.bin` file.

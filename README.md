# ESPHome Emporia Vue Utility Connect Unofficial Firmware

This is an unauthorized and unoffical firmware for the Emporia View Utility Connect device that reports energy usage to Home Assistant and completely divorces the device from Emporia's servers.

## Disclaimer

This software is of generally poor quality and should not be used by anyone.  When you install the software on your device, it
will no longer report data to Emporia.  You should backup the original Emporia firmware before installing this.

## Installation

It is assumed you already have a working installation of [ESPHome](https://esphome.io/), a USB-Serial adapter, basic soldering skills and supplied, etc.

Connect a your USB to serial adapter to the port marked "P3" as follows:

| Pin | Description | USB-Serial port |
| --- |  ---------  | --------------- |
|  1  |        IO0  |             RTS |
|  2  |         EN  |             DTR |
|  3  |        GND  |             GND |
|  4  |         TX  |              RX |
|  5  |         RX  |              TX |
|  6  |        +5v  |             +5v |

Note that pin 6 (the pin just above the text "EmporiaEnergy") is 5 volts, not 3.3v.  **Use caution to not apply 5V to the wrong pin or
the magic smoke may come out.**  You may opt to not connect pin 6 at all and instead plug the device into a usb port to provide power,
a portable USB battery pack also works well for this.

Instead of connecting IO0 and EN, you can simply short IO0 to ground while connecting power to get the device into bootloader mode.

Download [emporia_vue_utility.h](src/emporia_vue_utility.h) and [vue-utility.yaml](src/vue-utility.yaml).  If you don't already have a [secrets.yaml](src/secrets.yaml) file, download that too and edit it to set your credentials.

Execute `esphome run vue-utility.yaml` to build and install

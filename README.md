> [!WARNING]
> ### No longer actively maintained!
>
> Due to conflicts with my employment, I am no longer able to maintain or support this.  However, @nekorevend has a fork that is actively maintained and I recommend:
> ### https://github.com/nekorevend/esphome-emporia-vue-utility ###
>
> -Joe


# ESPHome Emporia Vue Utility Connect Unofficial Firmware

This is an unauthorized and unoffical firmware for the Emporia View Utility Connect device that reports energy usage to Home Assistant and completely divorces the device from Emporia's servers.

## Disclaimer

This software is of generally poor quality and should not be used by anyone.  When you install the software on your device, it
will no longer report data to Emporia.  You should backup the original Emporia firmware before installing this.

## Installation

Connect a your USB to serial adapter to the port marked "P3" as follows:

| Pin | Description | USB-Serial port |
| --- |  ---------  | --------------- |
|  1  |        IO0  |             RTS |
|  2  |         EN  |             DTR |
|  3  |        GND  |             GND |
|  4  |         TX  |              RX |
|  5  |         RX  |              TX |
|  6  |        +5v  |             +5v |

Note that pin 6 (the pin just above the text "EmporiaEnergy") is 5 volts, not 3.3v.  Use caution not to apply 5V to the wrong pin or
the magic smoke may come out.  You may want to not connect pin 6 at all and instead plug the device into a usb port to provide power,
a portable USB battery pack works well for this.

Instead of connecting IO0 and EN, you can simply short IO0 to ground while connecting power to get the device into bootloader mode.

Download one of the [releases](https://github.com/jrouvier/esphome-emporia-vue-utility/releases) and extract.  Edit either `vue-utility.yaml` or `vue-utility-solar.yaml` if you have local energy production and modify to your liking.

Execute `esphome run vue-utility.yaml` or `esphome run vue-utility-solar.yaml` to build and install.

## Meaning of LEDs

There are three LEDs on the device, which have "power", "wifi" and "link" icons stenciled on the case.
* **Power** = An ESPHome status led.  Slowly flashing means warning, quickly flashing means error, solid on means OK.  See [status_led](https://esphome.io/components/status_led.html) docs.
* **Wifi** = Normally solid on, will briefly flash each time a meter rejoin is attempted which indicates poor signal from the meter.
* **Link** = Flashes off briefly about once every 5 seconds.  More specifically, the LED turns off when a reading from the meter is requested and turns back on when a response is received.  If no response is received then the LED will remain off.  If this LED is never turning on then no readings are being returned by the meter.

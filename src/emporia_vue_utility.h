#include "esphome.h"
#include "sensor.h"

// Extra meter reading response debugging
#define DEBUG_VUE_RESPONSE true

// If the instant watts being consumed meter reading is outside of these ranges,
// the sample will be ignored which helps prevent garbage data from polluting
// home assistant graphs.  Note this is the instant watts value, not the
// watt-hours value, which has smarter filtering.  The defaults of 131kW
// should be fine for most people.  (131072 = 0x20000)
#define WATTS_MIN -131072 
#define WATTS_MAX  131072

// How much the watt-hours consumed value can change between samples.
// Values that change by more than this over the avg value across the
// previous 5 samples will be discarded.  
#define MAX_WH_CHANGE 2000

// How many samples to average the watt-hours value over. 
#define MAX_WH_CHANGE_ARY 5

// How often to request a reading from the meter in seconds.
// Meters typically update the reported value only once every
// 10 to 30 seconds, so "5" is usually fine.
// You might try setting this to "1" to see if your meter has
// new values more often
#define METER_READING_INTERVAL 5

// How often to attempt to re-join the meter when it hasn't
// been returning readings
#define METER_REJOIN_INTERVAL 30

// On first startup, how long before trying to start to talk to meter
#define INITIAL_STARTUP_DELAY 10

// Should this code manage the "wifi" and "link" LEDs?
// set to false if you want manually manage them elsewhere
#define USE_LED_PINS true

#define LED_PIN_LINK 32
#define LED_PIN_WIFI 33

class EmporiaVueUtility : public Component,  public UARTDevice {
    public:
        EmporiaVueUtility(UARTComponent *parent): UARTDevice(parent) {}
        Sensor *kWh_net      = new Sensor();
        Sensor *kWh_consumed = new Sensor();
        Sensor *kWh_returned = new Sensor();
        Sensor *W       = new Sensor();

        const char *TAG = "Vue";

        struct MeterReading {
            char header;
            char is_resp;
            char msg_type;
            uint8_t data_len;
            byte unknown1[4];
            uint32_t watt_hours;
            byte unknown2[48];
            uint32_t watts;
            byte unknown3[88];
            uint32_t ms_since_reset;
        };

        // A Mac Address or install code response
        struct Addr {
            char header;
            char is_resp;
            char msg_type;
            uint8_t data_len;
            byte addr[8];
            char newline;
        };

        // Firmware version response
        struct Ver {
            char header;
            char is_resp;
            char msg_type;
            uint8_t data_len;
            uint8_t value;
            char newline;
        };

        union input_buffer {
            byte data[260]; // 4 byte header + 255 bytes payload + 1 byte terminator
            struct MeterReading mr;
            struct Addr addr;
            struct Ver ver;
        } input_buffer;

        char mgm_mac_address[25] = "";
        char mgm_install_code[25] = "";
        int mgm_firmware_ver = 0;

        uint16_t pos = 0;
        uint16_t data_len;

        time_t last_meter_reading = 0;
        time_t now;

        // Turn the wifi led on/off
        void led_wifi(bool state) {
#if USE_LED_PINS
            if (state) digitalWrite(LED_PIN_WIFI, 0);
            else       digitalWrite(LED_PIN_WIFI, 1);
#endif
            return;
        }

        // Turn the link led on/off
        void led_link(bool state) {
#if USE_LED_PINS
            if (state) digitalWrite(LED_PIN_LINK, 0);
            else       digitalWrite(LED_PIN_LINK, 1);
#endif
            return;
        }

        // Reads and logs everything from serial until it runs
        // out of data or encounters a 0x0d byte (ascii CR)
        void dump_serial_input(bool logit) {
            while (available()) {
                if (input_buffer.data[pos] == 0x0d) {
                    break;
                }
                input_buffer.data[pos] = read();
                if (pos == sizeof(input_buffer.data)) {
                    if (logit) {
                        ESP_LOGE(TAG, "Filled buffer with garbage:");
                        ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
                    }
                    pos = 0;
                } else {
                    pos++;
                }
            }
            if (pos > 0 && logit) {
                ESP_LOGE(TAG, "Skipped input:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos-1, ESP_LOG_ERROR);
            }
            pos = 0;
            data_len = 0;
        }

        size_t read_msg() {
            if (!available()) {
                return 0;
            }

            while (available()) {
                char c = read();
                uint16_t prev_pos = pos;
                input_buffer.data[pos] = c;
                pos++;

                switch (prev_pos) {
                    case 0:
                        if (c != 0x24 ) { // 0x24 == "$", the start of a message
                            ESP_LOGE(TAG, "Invalid input at position %d: 0x%x", pos, c);
                            dump_serial_input(true);
                            pos = 0;
                            return 0;
                        }
                        break;
                    case 1:
                        if (c != 0x01 ) { // 0x01 means "response"
                            ESP_LOGE(TAG, "Invalid input at position %d 0x%x", pos, c);
                            dump_serial_input(true);
                            pos = 0;
                            return 0;
                        }
                        break;
                    case 2:
                        // This is the message type byte
                        break;
                    case 3:
                        // The 3rd byte should be the data length
                        data_len = c;
                        break;
                    case sizeof(input_buffer.data) - 1:
                        ESP_LOGE(TAG, "Buffer overrun");
                        dump_serial_input(true);
                        return 0;
                    default:
                        if (pos < data_len + 5) {

                            ;
                        } else if (c == 0x0d) { // 0x0d == "/r", which should end a message
                            return pos;
                        } else {
                            ESP_LOGE(TAG, "Invalid terminator at pos %d 0x%x", pos, c);
                            ESP_LOGE(TAG, "Following char is 0x%x", read());
                            dump_serial_input(true);
                            return 0;
                        }
                }
            } // while(available())

            return 0;
        }

        // Byte-swap a 32 bit int in the proprietary format
        // used by the MGS111
        int32_t bswap32(uint32_t in) {
            uint32_t x = 0;
            x += (in & 0x000000FF) << 24;
            x += (in & 0x0000FF00) <<  8;
            x += (in & 0x00FF0000) >>  8;
            x += (in & 0xFF000000) >> 24;
            return x;
        }

        void handle_resp_meter_reading() {
            int32_t input_value;
            struct MeterReading *mr;
            mr = &input_buffer.mr;

            // Make sure the packet is as long as we expect
            if (pos < sizeof(struct MeterReading)) {
                ESP_LOGE(TAG, "Short meter reading packet");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
                return;
            }

            parse_meter_watt_hours(mr);
            parse_meter_watts(mr);

            // Unlike the other values, ms_since_reset is in our native byte order
            ESP_LOGD(TAG, "Seconds since meter watt-hour reset: %.3f", float(mr->ms_since_reset) / 1000.0 );

            // Extra debugging of non-zero bytes, only on first packet or if DEBUG_VUE_RESPONSE is true
            if ((DEBUG_VUE_RESPONSE) || (last_meter_reading == 0)) {
                for (int x = 1 ; x < pos / 4 ; x++) {
                    int y = x * 4;
                    if (       (input_buffer.data[y])
                            || (input_buffer.data[y+1])
                            || (input_buffer.data[y+2])
                            || (input_buffer.data[y+3])) {
                        ESP_LOGD(TAG, "Meter Response Bytes %3d to %3d: %02x %02x %02x %02x", y-4, y-1,
                                input_buffer.data[y], input_buffer.data[y+1],
                                input_buffer.data[y+2], input_buffer.data[y+3]);
                    }
                }
            }
        }

        void ask_for_bug_report() {
            ESP_LOGE(TAG, "If you continue to see this, please file a bug at");
            ESP_LOGE(TAG, "  https://forms.gle/duMdU2i7wWHdbK5TA");
            ESP_LOGE(TAG, "and include a few lines above this message and the data below until \"EOF\":");
            ESP_LOGE(TAG, "Full packet:");
            ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
            ESP_LOGI(TAG, "MGM Firmware Version: %d",      mgm_firmware_ver);
            ESP_LOGE(TAG, "EOF");
        }


        void parse_meter_watt_hours(struct MeterReading *mr) {
            // Keep the last N watt-hour samples so invalid new samples can be discarded
            static int32_t history[MAX_WH_CHANGE_ARY];
            static uint8_t  history_pos;
            static bool not_first_run;

            // Counters for deriving consumed and returned separately
            static int32_t consumed;
            static int32_t returned;

            // So we can avoid updating when no change
            static int32_t prev_reported_net;

            int32_t watt_hours;
            int32_t wh_diff;
            int32_t history_avg;
            int8_t x;

            watt_hours = bswap32(mr->watt_hours);
            if (
                      (watt_hours == 4194304) //  "missing data" message (0x00 40 00 00)
                   || (watt_hours == 0)) { 
                ESP_LOGI(TAG, "Watt-hours value missing");
                ask_for_bug_report();
                return;
            }

            if (!not_first_run) {
                // Initialize watt-hour filter on first run
                for (x = MAX_WH_CHANGE_ARY ; x != 0 ; x--) {
                    history[x-1] = watt_hours;
                }

                not_first_run = 1;
            }

            // Insert a new value into filter array
            history_pos++;
            if (history_pos == MAX_WH_CHANGE_ARY) {
                history_pos = 0;
            }
            history[history_pos] = watt_hours;

            history_avg = 0;
            // Calculate avg watt_hours over previous N samples
            for (x = MAX_WH_CHANGE_ARY ; x != 0 ; x--) {
                history_avg += history[x-1] / MAX_WH_CHANGE_ARY;
            }

            // Get the difference of current value from avg
            wh_diff = history_avg - watt_hours;

            if (abs(wh_diff) > MAX_WH_CHANGE) {
                ESP_LOGE(TAG, "Unreasonable watt-hours data of %d, +%d from moving avg",
                        watt_hours, wh_diff);
                ESP_LOG_BUFFER_HEXDUMP(TAG, (void *)&mr->watt_hours, 4, ESP_LOG_ERROR);
                ask_for_bug_report();
                return;
            }

            // Change wd_diff to difference from previously reported value
            // instead of diff from average
            wh_diff = watt_hours - prev_reported_net;
            prev_reported_net = watt_hours;

            // On a reset of the meter net value and also on first boot
            // we don't want the consumed and returned values to be insane.
            if (abs(wh_diff) > MAX_WH_CHANGE) {
                if (wh_diff != watt_hours) {
                    ESP_LOGE(TAG, "Skipping absurd watt-hour delta of +%d", wh_diff);
                    ask_for_bug_report();
                }
                return;
            }

            if (wh_diff > 0) { // Energy consumed from grid
                if (consumed > UINT32_MAX - wh_diff) {
                    consumed -= UINT32_MAX - wh_diff;
                } else {
                    consumed += wh_diff;
                }
            }
            if (wh_diff < 0) { // Energy sent to grid
                if (returned > UINT32_MAX - wh_diff) {
                    returned -= UINT32_MAX - wh_diff;
                } else {
                    returned += wh_diff;
                }
            }

            kWh_consumed->publish_state(float(consumed) / 1000.0);
            kWh_returned->publish_state(float(returned) / 1000.0);
            kWh_net->publish_state(float(watt_hours) / 1000.0);
        }

        void parse_meter_watts(struct MeterReading *mr) {
            int32_t watts;

            // Read the instant watts value
            // (it's actually a 24-bit int)
            watts = (bswap32(mr->watts) & 0xFFFFFF);

            // Bit 1 of the left most byte indicates a negative value
            if (watts & 0x800000) {
                if (watts == 0x800000) {
                    // Exactly "negative zero", which means "missing data"
                    ESP_LOGI(TAG, "Instant Watts value missing");
                    return;
                } else if (watts & 0xC00000) {
                    // This is either more than 12MW being returned,
                    // or it's a negative number in 1's complement.
                    // Since the returned value is a 24-bit value
                    // and "watts" is a 32-bit signed int, we can
                    // get away with this.
                    watts -= 0xFFFFFF;
                } else {
                    // If we get here, then hopefully it's a negative
                    // number in signed magnitude format
                    watts = (watts ^ 0x800000) * -1;
                }
            }

            if ((watts >= WATTS_MAX) || (watts < WATTS_MIN)) {
                ESP_LOGE(TAG, "Unreasonable watts value %d", watts);
                ESP_LOG_BUFFER_HEXDUMP(TAG, (void *)&mr->watts, 4, ESP_LOG_ERROR);
                ask_for_bug_report();
                return;
            }
            W->publish_state(watts);
        }

        void handle_resp_meter_join() {
            ESP_LOGD(TAG, "Got meter join response");
        }

        int handle_resp_mac_address() {
            ESP_LOGD(TAG, "Got mac addr response");
            struct Addr *mac;
            mac = &input_buffer.addr;

            snprintf(mgm_mac_address, sizeof(mgm_mac_address), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                    mac->addr[7],
                    mac->addr[6],
                    mac->addr[5],
                    mac->addr[4],
                    mac->addr[3],
                    mac->addr[2],
                    mac->addr[1],
                    mac->addr[0]);
            ESP_LOGI(TAG, "MGM Mac Address: %s", mgm_mac_address);
            return(0);
        }

        int handle_resp_install_code() {
            ESP_LOGD(TAG, "Got install code response");
            struct Addr *code;
            code = &input_buffer.addr;

            snprintf(mgm_install_code, sizeof(mgm_install_code), "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                    code->addr[0],
                    code->addr[1],
                    code->addr[2],
                    code->addr[3],
                    code->addr[4],
                    code->addr[5],
                    code->addr[6],
                    code->addr[7]);
            ESP_LOGI(TAG, "MGM Install Code: %s (secret)", mgm_install_code);
            return(0);
        }

        int handle_resp_firmware_ver() {
            struct Ver *ver;
            ver = &input_buffer.ver;
           
            mgm_firmware_ver = ver->value;

            ESP_LOGI(TAG, "MGM Firmware Version: %d", mgm_firmware_ver);
            return(0);
        }

        void send_meter_request() {
            const byte msg[] = { 0x24, 0x72, 0x0d };
            ESP_LOGD(TAG, "Sending request for meter reading");
            write_array(msg, sizeof(msg));
            led_link(false);
        }

        void send_meter_join() {
            const byte msg[] = { 0x24, 0x6a, 0x0d };
            ESP_LOGI(TAG, "MGM Firmware Version: %d",      mgm_firmware_ver);
            ESP_LOGI(TAG, "MGM Mac Address:  %s",           mgm_mac_address);
            ESP_LOGI(TAG, "MGM Install Code: %s (secret)", mgm_install_code);
            ESP_LOGI(TAG, "Trying to re-join the meter.  If you continue to see this message");
            ESP_LOGI(TAG, "you may need to move the device closer to your power meter or");
            ESP_LOGI(TAG, "contact your utililty and ask them to reprovision the device.");
            ESP_LOGI(TAG, "Also confirm that the above mac address & install code match");
            ESP_LOGI(TAG, "what is printed on your device.");
            ESP_LOGE(TAG, "You can also file a bug at");
            ESP_LOGE(TAG, "  https://forms.gle/duMdU2i7wWHdbK5TA");
            write_array(msg, sizeof(msg));
            led_wifi(false);
        }

        void send_mac_req() {
            const byte msg[] = { 0x24, 0x6d, 0x0d };
            ESP_LOGD(TAG, "Sending mac addr request");
            write_array(msg, sizeof(msg));
            led_wifi(false);
        }

        void send_install_code_req() {
            const byte msg[] = { 0x24, 0x69, 0x0d };
            ESP_LOGD(TAG, "Sending install code request");
            write_array(msg, sizeof(msg));
            led_wifi(false);
        }

        void send_version_req() {
            const byte msg[] = { 0x24, 0x66, 0x0d };
            ESP_LOGD(TAG, "Sending firmware version request");
            write_array(msg, sizeof(msg));
            led_wifi(false);
        }

        void clear_serial_input() {
            write(0x0d);
            flush();
            delay(100);
            while (available()) {
                while (available()) read();
                delay(100);
            }
        }

        void setup() override {
#if USE_LED_PINS
            pinMode(LED_PIN_LINK, OUTPUT);
            pinMode(LED_PIN_WIFI, OUTPUT);
#endif
            led_link(false);
            led_wifi(false);
            clear_serial_input();
        }

        void loop() override {
            static time_t next_meter_request;
            static time_t next_meter_join;
            static uint8_t startup_step;
            char msg_type = 0;
            size_t msg_len = 0;
            byte inb;

            msg_len = read_msg();
            now = time(&now);
            if (msg_len != 0) {

                msg_type = input_buffer.data[2];

                switch (msg_type) {
                    case 'r': // Meter reading
                        led_link(true);
                        handle_resp_meter_reading();
                        last_meter_reading = now;
                        next_meter_join = now + METER_REJOIN_INTERVAL;
                        break;
                    case 'j': // Meter reading
                        handle_resp_meter_join();
                        led_wifi(true);
                        if (startup_step == 3) {
                            send_meter_request();
                            startup_step++;
                        }
                        break;
                    case 'f':
                        if (!handle_resp_firmware_ver()) {
                            led_wifi(true);
                            if (startup_step == 0) {
                                startup_step++;
                                send_mac_req();
                                next_meter_request = now + METER_READING_INTERVAL;
                            }
                        }
                        break;
                    case 'm': // Mac address
                        if (!handle_resp_mac_address()) {
                            led_wifi(true);
                            if (startup_step == 1) {
                                startup_step++;
                                send_install_code_req();
                                next_meter_request = now + METER_READING_INTERVAL;
                            }
                        }
                        break;
                    case 'i':
                        if (!handle_resp_install_code()) {
                            led_wifi(true);
                            if (startup_step == 2) {
                                startup_step++;
                                send_meter_request();
                                next_meter_request = now + METER_READING_INTERVAL;
                            }
                        }
                        break;
                    default:
                        ESP_LOGE(TAG, "Unhandled response type '%c'", msg_type);
                        ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, msg_len, ESP_LOG_ERROR);
                        break;
                }
                pos = 0;
            }

            if (now >= next_meter_request) {

                // Handle initial startup delay 
                if (next_meter_request == 0) {                    
                    next_meter_request = now + INITIAL_STARTUP_DELAY;
                    next_meter_join    = next_meter_request + METER_REJOIN_INTERVAL;
                    return;
                }

                // Schedule the next MGM message
                next_meter_request = now + METER_READING_INTERVAL;

                if (now > next_meter_join) {
                    startup_step = 9; // Cancel startup messages
                    send_meter_join();
                    next_meter_join = now + METER_REJOIN_INTERVAL;
                    return;
                }
               
                if      (startup_step == 0) send_version_req();
                else if (startup_step == 1) send_mac_req();
                else if (startup_step == 2) send_install_code_req();
                else if (startup_step == 3) send_meter_join();
                else                        send_meter_request();
                
            }
        }
};

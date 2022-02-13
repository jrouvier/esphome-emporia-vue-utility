#include "esphome.h"
#include "sensor.h"
#include "esphome/components/gpio/output/gpio_binary_output.h"

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
// Meters typically only have new data every few seconds,
// so "5" is usually fine.  You might try setting this to "1"
// to see if your meter has new values more often
#define METER_READING_INTERVAL 5

// How often to attempt to re-join the meter when it hasn't
// been returning readings
#define METER_REJOIN_INTERVAL 30

class EmporiaVueUtility : public Component,  public UARTDevice {
    public:
        EmporiaVueUtility(UARTComponent *parent): UARTDevice(parent) {}
        Sensor *kWh_net      = new Sensor();
        Sensor *kWh_consumed = new Sensor();
        Sensor *kWh_produced = new Sensor();
        Sensor *W       = new Sensor();

        const char *TAG = "EmporiaVue";

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

        union input_buffer {
            byte data[260]; // 4 byte header + 255 bytes payload + 1 byte terminator
            struct MeterReading mr;
        } input_buffer;

        uint16_t pos = 0;
        uint16_t data_len;

        time_t last_meter_reading = 0;
        time_t last_meter_requested = 0;
        time_t last_meter_join = 0;

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

        void parse_meter_watt_hours(struct MeterReading *mr) {
            // Keep the last N watt-hour samples so invalid new samples can be discarded
            static int32_t history[MAX_WH_CHANGE_ARY];
            static uint8_t  history_pos;
            static bool not_first_run;

            // Counters for deriving consumed and produced separately
            static int32_t consumed;
            static int32_t produced;

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
                ESP_LOGD(TAG, "Watt-hours value missing");
                return;
            }

            // Initialize watt-hour filter on first run
            if (!not_first_run) {
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
                ESP_LOGE(TAG, "Full packet:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
                return;
            }

            // Change wd_diff to difference from previously reported value
            // instead of diff from average
            wh_diff = watt_hours - prev_reported_net;
            prev_reported_net = watt_hours;

            // On a reset of the meter net value and also on first boot
            // we don't want the consumed and produced values to be insane.
            if (abs(wh_diff) > MAX_WH_CHANGE) {
                ESP_LOGE(TAG, "Skipping absurd watt-hour delta of +%d", wh_diff);
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
                if (produced > UINT32_MAX - wh_diff) {
                    produced -= UINT32_MAX - wh_diff;
                } else {
                    produced += wh_diff;
                }
            }

            kWh_consumed->publish_state(float(consumed) / 1000.0);
            kWh_produced->publish_state(float(produced) / 1000.0);
            kWh_net->publish_state(float(watt_hours) / 1000.0);
        }

        void parse_meter_watts(struct MeterReading *mr) {
            int32_t watts;

            // Read the instant watts value
            watts = bswap32(mr->watts);

            // Bit 1 of the left most byte indicates a negative value
            if (watts & 0x800000) {
                if (watts == 0x800000) {
                    // Exactly "negative zero", which means "missing data"
                    ESP_LOGD(TAG, "Instant Watts value missing");
                    return;
                }
                watts -= 0x800000;
            }

            if ((watts >= WATTS_MAX) || (watts < WATTS_MIN)) {
                ESP_LOGE(TAG, "Unreasonable watts value %d", watts);
                ESP_LOG_BUFFER_HEXDUMP(TAG, (void *)&mr->watts, 4, ESP_LOG_ERROR);
                ESP_LOGE(TAG, "Full packet:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
                return;
            }
            W->publish_state(watts);
        }

        void handle_resp_meter_join() {
            ESP_LOGD(TAG, "Got meter join response");
        }

        void send_meter_request() {
            const byte msg[] = { 0x24, 0x72, 0x0d };
            ESP_LOGD(TAG, "Sending request for meter reading");
            write_array(msg, sizeof(msg));
        }
        void send_meter_join() {
            const byte msg[] = { 0x24, 0x6a, 0x0d };
            ESP_LOGD(TAG, "Sending meter join");
            write_array(msg, sizeof(msg));
        }

        void setup() override {
            write(0x0d);
            dump_serial_input(false);
        }

        void loop() override {
            char msg_type = 0;
            size_t msg_len = 0;
            byte inb;
            time_t now;

            msg_len = read_msg();
            now = time(&now);
            if (msg_len != 0) {

                msg_type = input_buffer.data[2];

                switch (msg_type) {
                    case 'r': // Meter reading
                        handle_resp_meter_reading();
                        last_meter_reading = now;
                        break;
                    case 'j': // Meter reading
                        handle_resp_meter_join();
                        break;
                    default:
                        ESP_LOGE(TAG, "Unhandled response type '%c'", msg_type);
                        ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, msg_len, ESP_LOG_ERROR);
                        break;
                }
                pos = 0;
            }

            // Every meter_reading_interval seconds, request a new meter reading
            if (now - last_meter_requested >= METER_READING_INTERVAL) {
                send_meter_request();
                last_meter_requested = now;
            }

            // If we haven't received a meter reading after about 5 attempts,
            // attempt to re-join the meter
            if ((now - last_meter_reading >= (METER_READING_INTERVAL * 5)) 
                    && (now - last_meter_join >= METER_REJOIN_INTERVAL)) {
                send_meter_join();
                last_meter_join = now;
            }
        }
};

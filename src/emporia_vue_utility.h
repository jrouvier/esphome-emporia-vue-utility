#include "esphome.h"
#include "sensor.h"
#include "esphome/components/gpio/output/gpio_binary_output.h"

class EmporiaVueUtility : public Component,  public UARTDevice {
    public:
        EmporiaVueUtility(UARTComponent *parent): UARTDevice(parent) {}
        Sensor *kWh = new Sensor();
        Sensor *W   = new Sensor();

        const char *TAG = "EmporiaVue";

        struct MeterReading {
            char header;
            char is_resp;
            char msg_type;
            uint8_t data_len;
            byte unknown1[4];
            byte watt_hours[4];
            byte unknown2[36];
            byte unknown3[4];
            byte unknown4[4];
            byte unknown5[4];
            byte watts[4];
            byte unknown6[48];
            byte unknown7[4];
        };

        union input_buffer {
            byte data[260];
            struct Message msg;
            struct MeterReading mr;
        } input_buffer;

        uint16_t pos = 0;
        uint16_t data_len;

        time_t last_meter_reading = 0;
        time_t last_meter_requested = 0;
        time_t last_meter_join = 0;
        const time_t meter_reading_interval = 5;
        const time_t meter_join_interval = 30;

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

        uint32_t byte_32_read(const byte *inb) {
            uint32_t x = 0;
            x += inb[0] << 24;
            x += inb[1] << 16;
            x += inb[2] <<  8;
            x += inb[3];
            return x;
        }

        void handle_resp_meter_reading() {
            uint32_t input_value;
            struct MeterReading *mr;
            mr = &input_buffer.mr;

            // Read the watt-hours value
            input_value  = byte_32_read(mr->watt_hours);
            if (input_value >= 4194304) {
                ESP_LOGE(TAG, "Invalid watt-hours data");
                ESP_LOG_BUFFER_HEXDUMP(TAG, mr->watt_hours, 4, ESP_LOG_ERROR);
                ESP_LOGE(TAG, "Full packet:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
            } else {
                kWh->publish_state(float(input_value) / 1000.0);
            }

            // Read the instant watts value
            input_value = byte_32_read(mr->watts);
            if (input_value == 8388608) { // Appears to be "missing data" message (0x00 80 00 00)
                ESP_LOGD(TAG, "Instant Watts value missing");
            } 
            else if (input_value >= 131072) {
                ESP_LOGE(TAG, "Unreasonable watts value %d", input_value);
                ESP_LOG_BUFFER_HEXDUMP(TAG, mr->watts, 4, ESP_LOG_ERROR);
                ESP_LOGE(TAG, "Full packet:");
                ESP_LOG_BUFFER_HEXDUMP(TAG, input_buffer.data, pos, ESP_LOG_ERROR);
            } else {
                W->publish_state(input_value);
            }
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
            sleep(1);
            dump_serial_input(false);
            sleep(1);
            dump_serial_input(false);
        }

        void loop() override {
            float curr_kWh;
            uint32_t curr_Watts;
            int packet_len = 0;
            int bytes_rx;
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

            if (now - last_meter_requested > meter_reading_interval) {
                send_meter_request();
                last_meter_requested = now;
            }
            if ((now - last_meter_reading > (meter_reading_interval * 5)) 
                    && (now - last_meter_join > meter_join_interval)) {
                send_meter_join();
                last_meter_join = now;
            }
        }
};

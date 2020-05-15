/*
 *
 * Watchdog_ESP32-IDF
 *
 * This is a program for the ESP32 based CurrentReader board.
 *
 * It monitors MQTT messages for a request to enable a relay
 * controlling a device, as well as switches used to manually control
 * the relay.
 *
 * It monitors one of four ADC ports that is connected to a current
 * tranformer to measure AC current used by the device being controlled.
 * It will send MQTT messages based on the state of the device being controlled.
 *
 * It will enforce cycle time and max time constraints on that device, sending
 * MQTT messages if either is exceeded as well as turning off the device via
 * relay.
 *
 */
#include <stdio.h>
#include <string.h>
#include "esp_types.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#include "driver/adc.h"


#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include <soc/sens_reg.h>
#include <soc/sens_struct.h>

#include "wificonfig.h"

// Timer constants
#define TIMER_DIVIDER 80   // timer clock divider --> 1 MHz count rate
#define TIMER_INTERVAL 333 // interrupt rate --> every 333 uSec
#define SAMPLES_PER_CYCLE 50

// ADC constants
static const adc_channel_t channel0 = ADC_CHANNEL_4;
static const adc_channel_t channel1 = ADC_CHANNEL_5;
static const adc_channel_t channel2 = ADC_CHANNEL_6;
static const adc_channel_t channel3 = ADC_CHANNEL_7;

#define RING_SIZE 32 // number of cycles to average


// Board-specific constants
//
#define GPIO_INPUT_GPIO0          0
#define GPIO_INPUT_ON_SWITCH      2
#define GPIO_INPUT_OFF_SWITCH     4
#define GPIO_INPUT_PIN_SEL ((1ULL<<GPIO_INPUT_GPIO0) | (1ULL<<GPIO_INPUT_ON_SWITCH) | (1ULL<<GPIO_INPUT_OFF_SWITCH))

#define GPIO_OUTPUT_RELAY_POWER   14
#define GPIO_OUTPUT_CONNECTED_LED 25
#define GPIO_OUTPUT_ACCESS_LED    26
#define GPIO_OUTPUT_SENSE_LED      27
#define GPIO_OUTPUT_PIN_SEL ((1ULL<<GPIO_OUTPUT_RELAY_POWER) | (1ULL<<GPIO_OUTPUT_ACCESS_LED) | (1ULL<<GPIO_OUTPUT_CONNECTED_LED) | (1ULL<<GPIO_OUTPUT_SENSE_LED))

const char *TAG = "Watchdog";

static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static int relay_state = 0;    // Is relay on?
static int on_by_button = 0;
static int on_by_mqtt = 0;
int64_t button_on_time = 0;
int64_t mqtt_on_time = 0;

static int running_state = 0;  // Is device using current above threshold?

static int alarm_state = 0;  // Has watchdog detected an alarm condition?
static int alarm_type = 0;   // x1: maxtime, 1x: duty-cycle, 0: none

#define ALARM_TYPE_MAXTIME   0x1
#define ALARM_TYPE_DUTYCYCLE 0x2

enum relay_source_t {
    RELAY_BUTTON = 0,
    RELAY_MQTT = 1,
    RELAY_ALARM = 2,
};

static void initialize_pins (void) {
    gpio_config_t io_conf;

    relay_state = 0;
    running_state = 0;
    alarm_state = 0;
    alarm_type = 0;
    on_by_mqtt = 0;
    on_by_button = 0;

    // Configure Inputs

    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //bit mask of the pins that you want to set,e.g.GPIO21/32
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);


    // Configure Outputs

    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO21/32
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    // configure ADC
    //
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(channel0, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(channel1, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(channel2, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(channel3, ADC_ATTEN_DB_11);

    // allow ADC configuration to happen before
    // our cache-safe local routine reads
    //
    printf ("Initial read channel0: %d\n", adc1_get_raw(channel0));
    printf ("initial read channel1: %d\n", adc1_get_raw(channel1));
    printf ("initial read channel2: %d\n", adc1_get_raw(channel2));
    printf ("initial read channel3: %d\n", adc1_get_raw(channel3));


    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 0);
    gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 0);
    gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 0);
    gpio_set_level(GPIO_OUTPUT_SENSE_LED, 0);

    // Flash all LEDs on for 500ms
    //
    gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 1);
    gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 1);
    gpio_set_level(GPIO_OUTPUT_SENSE_LED, 1);
    vTaskDelay(500 / portTICK_RATE_MS);
    gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 0);
    gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 0);
    gpio_set_level(GPIO_OUTPUT_SENSE_LED, 0);
}

// "safe" code to read adc from interrupt handler
//
// Copied from https://www.toptal.com/embedded/esp32-audio-sampling
//
static int IRAM_ATTR local_adc1_read(int channel) {
    uint16_t adc_value;
    SENS.sar_meas_start1.sar1_en_pad = (1 << channel); // only one channel is selected
    while (SENS.sar_slave_addr1.meas_status != 0);
    SENS.sar_meas_start1.meas1_start_sar = 0;
    SENS.sar_meas_start1.meas1_start_sar = 1;
    while (SENS.sar_meas_start1.meas1_done_sar == 0);
    adc_value = SENS.sar_meas_start1.meas1_data_sar;
    return adc_value;
}

int amplitude_ring0[RING_SIZE];
int amplitude_ring1[RING_SIZE];
int amplitude_ring2[RING_SIZE];
int amplitude_ring3[RING_SIZE];
int ring_pos = 0;
int sample_count = 0;

int channel0_max = 0;
int channel0_min = 4096;
int channel1_max = 0;
int channel1_min = 4096;
int channel2_max = 0;
int channel2_min = 4096;
int channel3_max = 0;
int channel3_min = 4096;

static int *sensor_ring;

/*
 * Timer group0 ISR handler
 *
 * Read ADC values and find max, min, and store amplitude of input for the
 * most recent cycle. Called every 167 uSec, which means there are 100
 * samples per 60Hz AC power cycle
 *
 * Note:
 * We don't call the timer API here because they are not declared with IRAM_ATTR.
 * If we're okay with the timer irq not being serviced while SPI flash cache is disabled,
 * we can allocate this interrupt without the ESP_INTR_FLAG_IRAM flag and use the normal API.
 */

void IRAM_ATTR timer_group0_isr(void *para)
{
    int val0 = local_adc1_read(channel0);
    int val1 = local_adc1_read(channel1);
    int val2 = local_adc1_read(channel2);
    int val3 = local_adc1_read(channel3);

    //int val0 = 0;
    //int val1 = 0;
    //int val2 = 0;
    //int val3 = 0;

    if (val0 > channel0_max)
        channel0_max = val0;
    if (val0 < channel0_min)
        channel0_min = val0;

    if (val1 > channel1_max)
        channel1_max = val1;
    if (val1 < channel1_min)
        channel1_min = val1;

    if (val2 > channel2_max)
        channel2_max = val2;
    if (val2 < channel2_min)
        channel2_min = val2;

    if (val3 > channel3_max)
        channel3_max = val3;
    if (val3 < channel3_min)
        channel3_min = val3;

    sample_count++;
    if (sample_count >= SAMPLES_PER_CYCLE) {
        sample_count = 0;
        amplitude_ring0[ring_pos] = channel0_max - channel0_min;
        amplitude_ring1[ring_pos] = channel1_max - channel1_min;
        amplitude_ring2[ring_pos] = channel2_max - channel2_min;
        amplitude_ring3[ring_pos] = channel3_max - channel3_min;
        channel0_max = 0;
        channel1_max = 0;
        channel2_max = 0;
        channel3_max = 0;
        channel0_min = 4096;
        channel1_min = 4096;
        channel2_min = 4096;
        channel3_min = 4096;

        ring_pos++;
        if (ring_pos >= RING_SIZE) {
            ring_pos = 0;
        }
    }

    timer_group_intr_clr_in_isr(0, 0);
    
    /* After the alarm has been triggered
      we need enable it again, so it is triggered the next time */
    timer_group_enable_alarm_in_isr(0, 0);

}

// initialize amplitude rings
static void initialize_rings (void)
{
    int i;
    for (i=0; i<RING_SIZE; i++) {
        amplitude_ring0[i] = 0;
        amplitude_ring1[i] = 0;
        amplitude_ring2[i] = 0;
        amplitude_ring3[i] = 0;
    }
}

static void initialize_timer (void)
{
    initialize_rings();

    timer_config_t config;
    config.divider = TIMER_DIVIDER;
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en = TIMER_PAUSE;
    config.alarm_en = TIMER_ALARM_EN;
    config.intr_type = TIMER_INTR_LEVEL;
    config.auto_reload = 1;
#ifdef CONFIG_IDF_TARGET_ESP32S2BETA
    config.clk_sel = TIMER_SRC_CLK_APB;
#endif
    timer_init(0, 0, &config);

    // inital value for counter (also reload value)
    timer_set_counter_value(0, 0, 0x00000000ULL);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(0, 0, TIMER_INTERVAL);
    timer_enable_intr(0, 0);
    timer_isr_register(0, 0, timer_group0_isr,
        (void *) 0, ESP_INTR_FLAG_IRAM, NULL);

    // launch!
    timer_start(0, 0);
}

static int get_average_amplitude (int* ring) {
    int i;
    int sum = 0;
    for (i=0; i<RING_SIZE; i++)
        sum += ring[i];
    return (sum / RING_SIZE);
}

void read_sensors (int *array) {
    array[0] = get_average_amplitude (amplitude_ring0);
    array[1] = get_average_amplitude (amplitude_ring1);
    array[2] = get_average_amplitude (amplitude_ring2);
    array[3] = get_average_amplitude (amplitude_ring3);
}

static void strobe_leds (void *pvParameters) {
    while (1) {
        gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 1);
        vTaskDelay(200 / portTICK_RATE_MS);
        gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 0);
        gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 1);
        vTaskDelay(200 / portTICK_RATE_MS);
        gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 0);
        gpio_set_level(GPIO_OUTPUT_SENSE_LED, 1);
        vTaskDelay(200 / portTICK_RATE_MS);
        gpio_set_level(GPIO_OUTPUT_SENSE_LED, 0);
        vTaskDelay(200 / portTICK_RATE_MS);
    }
}

static void check_gpio0 (void *pvParameters) {
    int last_level = 1;
    int64_t pressed_time = 0;
    while (1) {
        int new_level = gpio_get_level(GPIO_INPUT_GPIO0);
        if (new_level != last_level) {
            int64_t curr_time = esp_timer_get_time ();
            if (new_level == 1) {
                int64_t curr_time = esp_timer_get_time ();
                if ((curr_time - pressed_time) > 3000000) {
                    trigger_wificonfig();
                }
            } else {
                pressed_time = curr_time;
            }
        }
        last_level = new_level;
        vTaskDelay(100 / portTICK_RATE_MS);
    }
}

// list of access points to try
//
struct ap_entry {
    char *ssid;
    char *password;
};
struct ap_entry *ap_list = NULL;
int ap_count = 0;
int ap_idx = 0;

// load AP credentials into list from wifi configuration structure
static void load_aps (void) {
    // first, find number of APs
    if (strlen(wificonfig_vals_wifi.ap1_ssid) > 0) ap_count++;
    if (strlen(wificonfig_vals_wifi.ap2_ssid) > 0) ap_count++;
    if (strlen(wificonfig_vals_wifi.ap3_ssid) > 0) ap_count++;
    if (strlen(wificonfig_vals_wifi.ap4_ssid) > 0) ap_count++;

    if (ap_count > 0)
        ap_list = malloc (sizeof(struct ap_entry) * ap_count);

    ap_idx = 0;
    if (strlen(wificonfig_vals_wifi.ap1_ssid) > 0) {
       ap_list[ap_idx].ssid = wificonfig_vals_wifi.ap1_ssid;
       ap_list[ap_idx++].password = wificonfig_vals_wifi.ap1_pswd;
    }
    if (strlen(wificonfig_vals_wifi.ap2_ssid) > 0) {
       ap_list[ap_idx].ssid = wificonfig_vals_wifi.ap2_ssid;
       ap_list[ap_idx++].password = wificonfig_vals_wifi.ap2_pswd;
    }
    if (strlen(wificonfig_vals_wifi.ap3_ssid) > 0) {
       ap_list[ap_idx].ssid = wificonfig_vals_wifi.ap3_ssid;
       ap_list[ap_idx++].password = wificonfig_vals_wifi.ap3_pswd;
    }
    if (strlen(wificonfig_vals_wifi.ap4_ssid) > 0) {
       ap_list[ap_idx].ssid = wificonfig_vals_wifi.ap4_ssid;
       ap_list[ap_idx++].password = wificonfig_vals_wifi.ap4_pswd;
    }
}

static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    ESP_LOGI(TAG, "event_handler: Event dispatched from event loop base=%s, event_id=%d", event_base, event_id);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, wificonfig_vals_wifi.hostname);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Got disconnected");
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);

        // connect to next AP on list
        ap_idx++;
        if (ap_idx >= ap_count) {
            ap_idx = 0;
        }
        wifi_config_t wifi_config = {
            .sta = {
                .ssid = "",
                .password = "",
            },
        };
        strncpy((char *)wifi_config.sta.ssid, ap_list[ap_idx].ssid, 32);
        strncpy((char *)wifi_config.sta.password, ap_list[ap_idx].password, 64);

        ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
        ESP_ERROR_CHECK( esp_wifi_connect() );
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    }
}


static void initialize_wifi (void) {
    //tcpip_adapter_init();
    esp_netif_init();
    wifi_event_group = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
        },
    };

    ap_idx = 0;
    strncpy((char *)wifi_config.sta.ssid, (char *)ap_list[ap_idx].ssid, 32);
    strncpy((char *)wifi_config.sta.password, (char *)ap_list[ap_idx].password, 64);

    ESP_LOGI(TAG, "Setting WiFi configuration SSID '%s'...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}


static esp_mqtt_client_handle_t mqtt_client;
static int mqtt_connected = false;

static void publish_status (char *subtopic, int val) {
    if ((mqtt_client == NULL) || !mqtt_connected) {
        return;
    }
    char topic[128];
    sprintf (topic, "stat/%s/%s", wificonfig_vals_mqtt.topic, subtopic);
    int msg_id = esp_mqtt_client_publish(mqtt_client, topic, val ? "ON" : "OFF", 0, 1, 0);
    ESP_LOGI(TAG, "publish successful, msg_id=%d", msg_id);
}

static void switch_relay (int val, enum relay_source_t src) {
    bool send_msg = false;
    switch (src) {
        case RELAY_BUTTON:
            if (val == 0) {
                on_by_button = 0;
                if (!alarm_state && (on_by_mqtt == 0)) {
                    ESP_LOGI(TAG, "Turning relay off");
                    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 0);
                    relay_state = 0;
                    send_msg = true;
                }
            } else if (!alarm_state) {
                on_by_button = 1;
                button_on_time = esp_timer_get_time ();
                if (!relay_state) {
                    ESP_LOGI(TAG, "Turning relay on");
                    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 1);
                    relay_state = 1;
                    send_msg = true;
                }
            }
            break;

        case RELAY_MQTT:
            if (val == 0) {
                on_by_mqtt = 0;
                if (!alarm_state && (on_by_button == 0)) {
                    ESP_LOGI(TAG, "Turning relay off");
                    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 0);
                    relay_state = 0;
                    send_msg = true;
                }
            } else if (!alarm_state) {
                on_by_mqtt = 1;
                mqtt_on_time = esp_timer_get_time ();
                if (!relay_state) {
                    ESP_LOGI(TAG, "Turning relay on");
                    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 1);
                    relay_state = 1;
                    send_msg = true;
                }
            }
            break;

        case RELAY_ALARM:
            if (val == 0) {
                if (relay_state) {
                    ESP_LOGI(TAG, "ALARM: Turning relay off");
                    gpio_set_level(GPIO_OUTPUT_RELAY_POWER, 0);
                    relay_state = 0;
                    send_msg = true;
                 }
                 alarm_state = 1;
                 on_by_mqtt = 0;
                 on_by_button = 0;
            }
            break;

        default:
            break;
    }

    if (send_msg) {
        publish_status ("POWER", relay_state);
    }

}

static void mqtt_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    ESP_LOGI(TAG, "mqtt_event_handler: Event dispatched from event loop base=%s, event_id=%d", event_base, event_id);

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) event_data;
    int msg_id;

    char full_topic[128];

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            mqtt_connected = true;
            sprintf (full_topic, "cmnd/%s/POWER", wificonfig_vals_mqtt.topic);
            msg_id = esp_mqtt_client_subscribe (mqtt_client, full_topic, 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            mqtt_connected = false;
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
            if (event->data_len > 0) {
                if (strncmp (event->data, "ON", event->data_len) == 0) {
                    switch_relay (1, RELAY_MQTT);
                } else {
                    switch_relay (0, RELAY_MQTT);
                }
            }
            break;

        default:
            ESP_LOGI(TAG, "Other event id:%d", event_id);
            break;
    }
}


static void initialize_mqtt () {

    char uri[128];

    mqtt_client = NULL;
    mqtt_connected = false;
    if (strcmp (wificonfig_vals_mqtt.host, "") == 0) {
        return;
    }

    sprintf (uri, "mqtt://%s", wificonfig_vals_mqtt.host);

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = uri,
        .port = wificonfig_vals_mqtt.port,
        .client_id = wificonfig_vals_mqtt.client,
        .username = wificonfig_vals_mqtt.user,
        .password = wificonfig_vals_mqtt.pswd,
    };

    // wait for Wifi connection
    ESP_LOGI(TAG, "initialize_mqtt: Waiting for wifi to go up");
    while ((xEventGroupGetBits (wifi_event_group) & CONNECTED_BIT) == 0) {
        vTaskDelay(100 / portTICK_RATE_MS);
    }
    ESP_LOGI(TAG, "initialize_mqtt: Wifi is up");


    vTaskDelay(1000 / portTICK_RATE_MS);
    //ESP_LOGI(TAG, "initialize_mqtt: Calling esp_mqtt_client_init");
    mqtt_client = esp_mqtt_client_init (&mqtt_cfg);
    if (mqtt_client != NULL) {
        //ESP_LOGI(TAG, "initialize_mqtt: Calling esp_mqtt_client_register_event");
        esp_mqtt_client_register_event (mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
        ESP_LOGI(TAG, "initialize_mqtt: Calling esp_mqtt_client_start");
        if (esp_mqtt_client_start (mqtt_client) != ESP_OK) {
            ESP_LOGE(TAG, "initialize_mqtt: Unable to start MQTT client");
        }
    } else {
        ESP_LOGE(TAG, "initialize_mqtt: Unable to initialize MQTT client");
    }
}

int64_t alarm_time = 0;
int ran_this_minute = 0;
static void watchdog_main_loop (void *pvParameters) {
    int last_on_val = 1;
    int last_off_val = 1;
    int last_running = 0;
    int64_t start_time = 0;
    int conn_flashing = 0;
    int access_flashing = 0;

    while (1) {

        int64_t curr_time = esp_timer_get_time();

        // see if alarm has timed out (cooldown)
        //
        if (alarm_state && ((curr_time - alarm_time)/60000000 >= (wificonfig_vals_watchdog.cooldown))) {
            alarm_state = 0;
            alarm_type = 0;
            ESP_LOGI (TAG, "Alarm cooldown time has passed");
            publish_status ("ALARM", 0);
            publish_status ("ALARM-MAXTIME", 0);
            publish_status ("ALARM-DUTYCYCLE", 0);
        }

        // Check status of buttons
        //
        int new_on_val = gpio_get_level(GPIO_INPUT_ON_SWITCH);
        int new_off_val = gpio_get_level(GPIO_INPUT_OFF_SWITCH);

        // check for "ON" button pressed
        if ((new_on_val == 0) && (last_on_val == 1)) {
            ESP_LOGI (TAG, "Saw ON button press");
            switch_relay (1, RELAY_BUTTON);
        }

        // check for "OFF" button pressed
        if ((new_off_val == 0) && (last_off_val == 1)) {
            ESP_LOGI (TAG, "Saw OFF button press");
            switch_relay (0, RELAY_BUTTON);
        }

        // see if last "ON" button has timed out
        if (on_by_button && ((curr_time - button_on_time)/60000000 >= wificonfig_vals_watchdog.button_to)) {
            ESP_LOGI(TAG, "ON-button timeout reached");
            switch_relay (0, RELAY_BUTTON);
        }

        // see if last "ON" mqtt has timed out
        if (on_by_mqtt && ((curr_time - mqtt_on_time)/60000000 >= wificonfig_vals_watchdog.mqtt_to)) {
            ESP_LOGI(TAG, "ON-mqtt timeout reached");
            switch_relay (0, RELAY_MQTT);
        }

        last_on_val = new_on_val;
        last_off_val = new_off_val;


        // Check current sensor
        //
        running_state = (get_average_amplitude(sensor_ring) >= wificonfig_vals_watchdog.thresh);
        gpio_set_level(GPIO_OUTPUT_SENSE_LED, running_state);
        if (running_state != last_running) {
            if (running_state) {
                start_time = curr_time;
            }
            publish_status ("RUNNING", running_state);
        }
        last_running = running_state;

        // See if we've blown MAXTIME requirement
        //
        if (((alarm_type & ALARM_TYPE_MAXTIME) == 0) && running_state && ((curr_time - start_time)/60000000 >= wificonfig_vals_watchdog.maxtime)) {
            ESP_LOGI (TAG, "MAXTIME alarm condition!");
            alarm_time = curr_time;
            alarm_type |= ALARM_TYPE_MAXTIME;
            switch_relay (0, RELAY_ALARM);
            publish_status ("ALARM", alarm_type);
            publish_status ("ALARM-MAXTIME", 1);
        }

        // record running state for duty cycle check
        if (running_state)
            ran_this_minute = 1;

        // take care of "connected" led
        // - off if not connected
        // - on if connected
        // - flashing if MQTT error
        
        if ((xEventGroupGetBits (wifi_event_group) & CONNECTED_BIT) == 0) {
            gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 0);
        } else if ((mqtt_client != NULL) & mqtt_connected) {
            gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 1);
        } else {
            gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, (conn_flashing == 0));
            conn_flashing = (conn_flashing + 1) % 2;
        }

        // take care of "access" led
        // - off if relay is off
        // - on if relay is on
        // - flashing if in alarm: number of flashes indicates alarm type
        
        if (!alarm_state) {
            gpio_set_level(GPIO_OUTPUT_ACCESS_LED, relay_state);
        } else {
            int i = access_flashing >> 1;
            gpio_set_level(GPIO_OUTPUT_ACCESS_LED, ((i == 0) || ((i == 2) && (alarm_type > 1)) || ((i == 4) && (alarm_type > 2))));
            access_flashing = (access_flashing + 1) % (alarm_type*4 + 4);
        }

        vTaskDelay(100 / portTICK_RATE_MS);
    }
}

// See if we've blown duty cycle requirement
// - checked every minute
//
static void dutycycle_loop (void *pvParameters) {

    int window = wificonfig_vals_watchdog.window;
    int8_t *duty_ring = malloc (window * sizeof (int8_t));
    int duty_pnt = 0;

    for (int i = 0; i<window; i++)
        duty_ring[i] = 0;

    while (1) {
        vTaskDelay(60000/ portTICK_RATE_MS); // wait one minute

        // record running/not running in ring
        //
        if (ran_this_minute)
            ESP_LOGI (TAG, "Running at minute %d", duty_pnt);
        duty_ring[duty_pnt++] = ran_this_minute;
        if (duty_pnt > window)
            duty_pnt = 0;
        ran_this_minute = 0;

        // make dutycycle check
        int sum = 0;
        for (int i=0; i<window; i++) {
            sum += duty_ring[i];
            if (((alarm_type & ALARM_TYPE_DUTYCYCLE) == 0) && running_state && (sum > (wificonfig_vals_watchdog.dutycycle * window / 100))) {
                ESP_LOGI (TAG, "Duty cycle alarm condition!");
                alarm_time = esp_timer_get_time();
                alarm_type |= ALARM_TYPE_DUTYCYCLE;
                switch_relay (0, RELAY_ALARM);
                publish_status ("ALARM", alarm_type);
                publish_status ("ALARM-DUTYCYCLE", 1);
            }
        }
    }
}


// send periodic updates
//
static void update_loop (void *pvParameters) {

    while (1) {
        if (wificonfig_vals_mqtt.update != 0) {
            publish_status ("POWER",   relay_state);
            publish_status ("RUNNING", running_state);
            publish_status ("ALARM",   alarm_type);
            vTaskDelay((60000 * wificonfig_vals_mqtt.update) / portTICK_RATE_MS);
        } else {
         // paranoia (should never get here)
         vTaskDelay(60000 / portTICK_RATE_MS);
        }
    }
}


void app_main(void) {
    TaskHandle_t xBlinkHandle = NULL;

    initialize_pins();
    initialize_timer();

    // tasks related to wifi-based configuration
    xTaskCreate(&strobe_leds, "strobe_leds", 4096, NULL, 5, &xBlinkHandle);
    wificonfig();
    if (xBlinkHandle != NULL) {
        vTaskDelete (xBlinkHandle);
        gpio_set_level(GPIO_OUTPUT_CONNECTED_LED, 0);
        gpio_set_level(GPIO_OUTPUT_ACCESS_LED, 0);
        gpio_set_level(GPIO_OUTPUT_SENSE_LED, 0);
    }
    load_aps();
    if (ap_count == 0) {
        // if no valid ssids -> go back to config mode
        ESP_LOGI (TAG, "Whoa! No ssids configured");
        trigger_wificonfig();
    }
    xTaskCreate(&check_gpio0, "check_gpio0", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK( esp_event_loop_create_default() );
    initialize_wifi();
    initialize_mqtt();

    // point to sensor in use
    switch (wificonfig_vals_watchdog.sensor) {
        case 0:
            sensor_ring = amplitude_ring0;
            break;
        case 1:
            sensor_ring = amplitude_ring1;
            break;
        case 2:
            sensor_ring = amplitude_ring2;
            break;
        case 3:
            sensor_ring = amplitude_ring3;
            break;
    }

    xTaskCreate(&watchdog_main_loop, "watchdog_main_loop", 4096, NULL, 5, NULL);
    xTaskCreate(&dutycycle_loop, "dutycycle_loop", 4096, NULL, 5, NULL);

    if ((mqtt_client != NULL) && (wificonfig_vals_mqtt.update != 0)) {
        xTaskCreate(&update_loop, "update_loop", 4096, NULL, 5, NULL);
    }
}

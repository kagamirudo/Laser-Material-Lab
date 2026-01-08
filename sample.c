int laserPin = 6;
int lightPin = 0;
// #define cyclenum 10
// int count;

void setup() {
    Serial.begin(115200);
    pinMode(laserPin, OUTPUT);
    analogWrite(laserPin, 255);
    // count = 0;
}

void loop() {
    /*
    count += 1;

    if (count > cyclenum) {
        analogWrite(laserPin, 0);
    } else {
        analogWrite(laserPin, 255);
    }

    if (count == cyclenum * 2) {
        count = 0;
    }
    */
    Serial.println(analogRead(lightPin));
}


#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

static const char *TAG = "LASER_ADC";

// Pin mapping (ESP32-S3)
#define LASER_GPIO   5    // PWM output to laser
// GPIO44 is not ADC-capable on ESP32-S3. Use a real ADC pin (e.g. GPIO4/ADC1_CH3).
#define ADC_GPIO     4    // ADC input from photodiode/sensor (ADC1_CH3 on S3)

// LEDC config
#define LASER_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LASER_LEDC_TIMER  LEDC_TIMER_0
#define LASER_LEDC_CH     LEDC_CHANNEL_0
#define LASER_LEDC_FREQ   5000       // 5 kHz
#define LASER_LEDC_RES    LEDC_TIMER_8_BIT
#define LASER_DUTY_FULL   ((1 << LASER_LEDC_RES) - 1)  // 255

static adc_oneshot_unit_handle_t adc_handle;
static adc_channel_t adc_chan;
static adc_unit_t adc_unit;

static void laser_init_full_on(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode      = LASER_LEDC_MODE,
        .duty_resolution = LASER_LEDC_RES,
        .timer_num       = LASER_LEDC_TIMER,
        .freq_hz         = LASER_LEDC_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .speed_mode = LASER_LEDC_MODE,
        .channel    = LASER_LEDC_CH,
        .timer_sel  = LASER_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LASER_GPIO,
        .duty       = LASER_DUTY_FULL,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
    ESP_ERROR_CHECK(ledc_update_duty(LASER_LEDC_MODE, LASER_LEDC_CH));
    ESP_LOGI(TAG, "Laser PWM set full on @ GPIO%d", LASER_GPIO);
}

static void adc_init_gpio44(void) {
    // Map GPIO to ADC unit/channel dynamically
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &adc_unit, &adc_chan));

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = adc_unit,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,  // up to ~3.3V
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, adc_chan, &chan_cfg));
    ESP_LOGI(TAG, "ADC initialized on GPIO%d (unit %d, channel %d)",
             ADC_GPIO, adc_unit, adc_chan);
}

void app_main(void) {
    ESP_LOGI(TAG, "Laser PWM + ADC sample (GPIO5 PWM, GPIO4 ADC)");

    laser_init_full_on();  // set laser full duty (like analogWrite 255)
    adc_init_gpio44();     // prepare ADC input

    // Detection thresholds based on your observations
    #define BEAM_DETECTED_MIN  1400  // beam hitting receiver
    #define BEAM_DETECTED_MAX  2000
    #define SAMPLES_TO_AVG     16    // average multiple samples to reduce noise

    while (1) {
        // Take multiple samples and average
        int32_t sum = 0;
        int valid_samples = 0;
        for (int i = 0; i < SAMPLES_TO_AVG; i++) {
            int raw_single = 0;
            esp_err_t err = adc_oneshot_read(adc_handle, adc_chan, &raw_single);
            if (err == ESP_OK) {
                sum += raw_single;
                valid_samples++;
            }
        }

        if (valid_samples > 0) {
            int avg_raw = (int)(sum / valid_samples);
            
            // Check if beam is detected
            if (avg_raw >= BEAM_DETECTED_MIN && avg_raw <= BEAM_DETECTED_MAX) {
                printf("ADC raw=%d [BEAM DETECTED]\n", avg_raw);
            } else {
                printf("ADC raw=%d\n", avg_raw);
            }
        } else {
            ESP_LOGE(TAG, "ADC read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


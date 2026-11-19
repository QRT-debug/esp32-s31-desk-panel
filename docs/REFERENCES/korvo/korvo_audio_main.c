#include <assert.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_audio_playback";

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_GPIO GPIO_NUM_0
#define I2C_SCL_GPIO GPIO_NUM_1

#define I2S_PORT I2S_NUM_0
#define I2S_BCLK_GPIO GPIO_NUM_3
#define I2S_WS_GPIO GPIO_NUM_4
#define I2S_DOUT_GPIO GPIO_NUM_5

#define PA_CTRL_GPIO GPIO_NUM_7

#define SAMPLE_RATE_HZ 48000
#define TONE_HZ 1000
#define TONE_AMPLITUDE 12000
#define CHANNEL_COUNT 2
#define CHANNEL_MASK 0x03
#define MCLK_MULTIPLE 256
#define TEST_VOLUME 65
#define TONE_SECONDS 5
#define TONE_BUFFER_FRAMES 256

static i2s_chan_handle_t s_tx_handle;

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL), TAG, "create I2S TX channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_16BIT;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init I2S STD mode failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable I2S TX failed");

    ESP_LOGI(TAG, "I2S STD TX: BCLK GPIO%d, WS GPIO%d, DOUT GPIO%d, MCLK disabled, %d Hz stereo 16-bit",
             I2S_BCLK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO, SAMPLE_RATE_HZ);
    return ESP_OK;
}

static esp_codec_dev_handle_t init_codec(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    audio_codec_i2c_cfg_t codec_i2c_cfg = {
        .port = I2C_PORT,
        .addr = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c_cfg);
    assert(ctrl_if);

    audio_codec_i2s_cfg_t codec_i2s_cfg = {
        .port = I2S_PORT,
        .rx_handle = NULL,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&codec_i2s_cfg);
    assert(data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    es8389_codec_cfg_t es8389_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_CTRL_GPIO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
        .mclk_div = MCLK_MULTIPLE,
    };
    const audio_codec_if_t *codec_if = es8389_codec_new(&es8389_cfg);
    assert(codec_if);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t codec = esp_codec_dev_new(&dev_cfg);
    assert(codec);

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = 16,
        .channel = CHANNEL_COUNT,
        .channel_mask = CHANNEL_MASK,
        .sample_rate = SAMPLE_RATE_HZ,
        .mclk_multiple = MCLK_MULTIPLE,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(codec, &sample_cfg));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(codec, TEST_VOLUME));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_mute(codec, false));

    ESP_LOGI(TAG, "ES8389 DAC opened at 8-bit I2C address 0x%02x, PA GPIO%d active high, volume %d",
             ES8389_CODEC_DEFAULT_ADDR, PA_CTRL_GPIO, TEST_VOLUME);
    return codec;
}

static int16_t make_triangle_sample(size_t frame_index)
{
    const int period = SAMPLE_RATE_HZ / TONE_HZ;
    const int half_period = period / 2;
    const int phase = frame_index % period;
    int value = 0;

    if (phase < half_period) {
        value = -TONE_AMPLITUDE + (2 * TONE_AMPLITUDE * phase) / half_period;
    } else {
        value = TONE_AMPLITUDE - (2 * TONE_AMPLITUDE * (phase - half_period)) / half_period;
    }
    return (int16_t)value;
}

static void fill_1khz_stereo_tone(int16_t *buffer, size_t frames)
{
    static size_t frame_index;

    for (size_t i = 0; i < frames; i++) {
        int16_t sample = make_triangle_sample(frame_index++);
        buffer[i * CHANNEL_COUNT] = sample;
        buffer[i * CHANNEL_COUNT + 1] = sample;
    }
}

static void play_test_tone(esp_codec_dev_handle_t codec)
{
    int16_t pcm[TONE_BUFFER_FRAMES * CHANNEL_COUNT];
    const int loops = (SAMPLE_RATE_HZ * TONE_SECONDS) / TONE_BUFFER_FRAMES;

    ESP_LOGI(TAG, "Playing %d Hz stereo test tone for %d seconds", TONE_HZ, TONE_SECONDS);
    for (int i = 0; i < loops; i++) {
        fill_1khz_stereo_tone(pcm, TONE_BUFFER_FRAMES);
        ESP_ERROR_CHECK(esp_codec_dev_write(codec, pcm, sizeof(pcm)));
    }
    ESP_LOGI(TAG, "Tone finished");
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_i2s());
    esp_codec_dev_handle_t codec = init_codec();

    while (1) {
        play_test_tone(codec);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

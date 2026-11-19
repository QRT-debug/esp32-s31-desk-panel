/**
 ******************************************************************************
 * @file        myes8389.c
 * @version     V1.0 
 * @brief       ES8389驱动代码
 * *****************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板            
 ******************************************************************************
 */

#include "myes8389.h"
#include "driver/gpio.h"

const char* es8389_tag = "es8389";

/* 运行期句柄 */
static const audio_codec_ctrl_if_t *s_es8389_ctrl_if   = NULL;   /* I2C控制接口 */
static const audio_codec_if_t       *s_es8389_codec_if = NULL;   /* 编解码器接口 */
static const audio_codec_data_if_t  *s_es8389_data_if  = NULL;   /* I2S数据接口 */

/* 两套 esp_codec_dev 包装句柄 (录音用 IN, 播放用 OUT) */
static esp_codec_dev_handle_t s_record_dev = NULL;
static esp_codec_dev_handle_t s_play_dev  = NULL;

/* 当前状态 */
static bool s_recording_active = false;
static bool s_playing_active   = false;
static int  s_volume = ES8389_DEFAULT_VOLUME;                  /* 播放音量 0~100, 运行时可通过 API 修改 */


/* ---- 底层寄存器操作 ---- */
/**
 * @brief   写ES8389寄存器 (内部使用)
 * @param   reg    寄存器地址
 * @param   value  写入值
 * @retval  0=成功  -1=控制接口未初始化
 */
static int es8389_reg_write(int reg, int value)
{
    if (s_es8389_ctrl_if == NULL)
    {
        return -1;
    } 
    return s_es8389_ctrl_if->write_reg(s_es8389_ctrl_if, reg, 1, &value, 1);
}

/**
 * @brief   读ES8389寄存器
 * @param   reg  寄存器地址
 * @retval  寄存器值 (接口未初始化时返回 0)
 */
int myes8389_reg_read(int reg)
{
    int value = 0;
    if (s_es8389_ctrl_if == NULL)
    {
        return 0;
    } 
    s_es8389_ctrl_if->read_reg(s_es8389_ctrl_if, reg, 1, &value, 1);
    return value;
}


/**
 * @brief       切换 ES8389 PLL ref 和时钟载荷 (运行时手动覆盖)
 * @note          enable=false (录音态, PLL ×2):
 *                0x23 bit7=0, 0xF0 bit3=0,
 *                0x05=0x55, 0x06=0x84, 0x07=0xD0, 0x09=0xC1
 *
 *                enable=true (播放态, PLL ×4):
 *                0x23 bit7=1, 0xF0 bit3=1,
 *                0x05=0x45, 0x06=0x24, 0x07=0xC0, 0x09=0xD1
 * @param       enable : true=播放态 (DAC设备, PLL×4), false=录音态 (ADC设备, PLL×2)
 * @retval      无
 */
static void myes8389_set_dac_ref(bool enable)
{
    if (s_es8389_ctrl_if == NULL) return;

    if (enable) {
        /* DAC 设备态: 开 ADC TDM + DAC ref, PLL ×4 */
        es8389_reg_write(0x23, myes8389_reg_read(0x23) | 0x80);
        es8389_reg_write(0xF0, myes8389_reg_read(0xF0) | 0x08);
        es8389_reg_write(0x05, 0x45); 
        es8389_reg_write(0x06, 0x24);
        es8389_reg_write(0x07, 0xC0);
        es8389_reg_write(0x09, 0xD1); 
    } else {
        /* ADC 设备态: 关 TDM + ref, PLL ×2 */
        es8389_reg_write(0x23, myes8389_reg_read(0x23) & ~0x80);
        es8389_reg_write(0xF0, myes8389_reg_read(0xF0) & ~0x08);
        es8389_reg_write(0x05, 0x55); 
        es8389_reg_write(0x06, 0x84); 
        es8389_reg_write(0x07, 0xD0); 
        es8389_reg_write(0x09, 0xC1); 
    }
}


/* ---- 初始化 ---- */
uint8_t myes8389_init(void)
{
    /* 1. I2C 总线 */
    if (bus_handle == NULL) {
        ESP_ERROR_CHECK(myiic_init());
    }
    i2c_master_bus_handle_t i2c_bus_handle = bus_handle;

    /* 2. I2C 控制接口 */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr       = ES8389_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus_handle,
    };
    s_es8389_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(s_es8389_ctrl_if);

    /* 3. I2S 数据接口 */
    audio_codec_i2s_cfg_t i2s_cfg_i2s = {
        .port      = I2S_NUM,
        .rx_handle = rx_handle,
        .tx_handle = tx_handle,
    };
    s_es8389_data_if = audio_codec_new_i2s_data(&i2s_cfg_i2s);
    assert(s_es8389_data_if);

    /* 4. GPIO 接口 */
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    assert(gpio_if);

    /* 5. ES8389 codec 接口 (默认 ADC 设备态, ref_enable=false) */
    es8389_codec_cfg_t es8389_cfg = {
        .ctrl_if = s_es8389_ctrl_if,
        .gpio_if = gpio_if,
        .sys_cfg = {
            .is_master = false,     /* 从机模式 */
            .no_mclk   = true,      /* 无MCLK,时钟从BCLK派生 */
        },
        .adc_cfg = {
            .digital_mic = false,   /* 模拟麦克风 */
            .label       = "FL,FR",
        },
        .dac_cfg = {
            .ref_enable = false,    /* 默认 ADC 设备态 (运行时 myes8389_set_dac_ref 切换) */
        },
        .pa_cfg = {
            .pa_pin        = -1,    /* PA 由应用层 GPIO7 单独管理 */
            .pa_active_low = false,
        },
    };
    s_es8389_codec_if = es8389_codec_new(&es8389_cfg);
    assert(s_es8389_codec_if);

    /* 6. 创建两套 esp_codec_dev 包装 */
    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_es8389_codec_if,
        .data_if  = s_es8389_data_if,
    };
    s_record_dev = esp_codec_dev_new(&rec_cfg);
    assert(s_record_dev);

    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = s_es8389_codec_if,
        .data_if  = s_es8389_data_if,
    };
    s_play_dev = esp_codec_dev_new(&play_cfg);
    assert(s_play_dev);

    /* 7. 切 I2S TX 时钟到 APLL (精确 16kHz) */
    i2s_set_tx_clock_apll();

    /* 8. PA 控制 GPIO (GPIO7) */
    gpio_config_t pa_io = {
        .pin_bit_mask = 1ULL << PA_CTRL_GPIO_PIN,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_io);
    gpio_set_level(PA_CTRL_GPIO_PIN, 0);

    ESP_LOGI(es8389_tag, "ES8389 init ok (slave, no_mclk=true, record_dev + play_dev)");
    return 0;
}


/* ---- 录音: open / close ---- */

/**
 * @brief       开始录音 (开 ADC + 配 PLL ×2 + enable I2S)
 * @param       sample_rate : 采样率 (通常 16000)
 * @param       bits        : 位宽 (通常 16)
 * @retval      0=成功, ESP_FAIL=失败
 */
int myes8389_record_start(int sample_rate, int bits)
{
    if (s_recording_active) return 0;
    if (s_playing_active) {
        /* 先关播放再录音 */
        myes8389_play_stop();
    }

    /* PLL 切到 ADC 设备态 (×2), 必须在 open 之前调 */
    myes8389_set_dac_ref(false);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel         = 2,
        .channel_mask    = 0x03,
        .sample_rate     = sample_rate,
    };

    int ret = esp_codec_dev_open(s_record_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(es8389_tag, "record_dev open failed: %d", ret);
        return ESP_FAIL;
    }

    /* 覆盖 PLL  */
    myes8389_set_dac_ref(false);

    /* ADC 增益 */
    esp_codec_dev_set_in_gain(s_record_dev, 30.0f);

    s_recording_active = true;
    ESP_LOGI(es8389_tag, "ADC open: %dHz/%dbit, PLL ×2, 30dB gain", sample_rate, bits);
    return 0;
}


/**
 * @brief       停止录音 (关 ADC + I2S)
 */
void myes8389_record_stop(void)
{
    if (!s_recording_active) return;  
    esp_codec_dev_close(s_record_dev);

    s_recording_active = false;
    ESP_LOGI(es8389_tag, "ADC close");
}


/* ---- 播放: open / close ---- */

/**
 * @brief       开始播放 (开 DAC + 配 PLL ×4 + enable I2S)
 * @param       sample_rate : 采样率 (WAV 文件的原始采样率)
 * @param       bits        : 位宽
 * @retval      0=成功, ESP_FAIL=失败
 */
int myes8389_play_start(int sample_rate, int bits)
{
    if (s_playing_active) return 0;   
    if (s_recording_active) {
        myes8389_record_stop();       /* 先关录音再播放 */
    }

    /* PLL 切到 DAC 设备态 (×4), 必须在 open 之前调 */
    myes8389_set_dac_ref(true);

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel         = 2,
        .channel_mask    = 0x03,
        .sample_rate     = sample_rate,
    };

    int ret = esp_codec_dev_open(s_play_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(es8389_tag, "play_dev open failed: %d", ret);
        return ESP_FAIL;
    }

    myes8389_set_dac_ref(true);

    /* DAC 输出音量 (取运行时音量, 切换曲目不会重置) */
    esp_codec_dev_set_out_vol(s_play_dev, s_volume);

    /* 开 PA (GPIO7) */
    gpio_set_level(PA_CTRL_GPIO_PIN, 1);

    ESP_LOGI(es8389_tag, "PA ON, DAC open: %dHz/%dbit, PLL ×4, vol=%d", sample_rate, bits, s_volume);
    s_playing_active = true;
    return 0;
}


/**
 * @brief       停止播放 (关 DAC + PA + I2S)
 */
void myes8389_play_stop(void)
{
    if (!s_playing_active) return;  

    /* 先关 PA 再关 DAC  */
    gpio_set_level(PA_CTRL_GPIO_PIN, 0);
    
    esp_codec_dev_close(s_play_dev);

    s_playing_active = false;
    ESP_LOGI(es8389_tag, "DAC close, PA off");
}


/**
 * @brief       兼容 ADC 开关
 * @note        直接委托 record_start/stop (自动配 PLL + I2S)
 */
void myes8389_adc_enable(bool enable)
{
    if (enable) {
        myes8389_record_start(I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);
    } else {
        myes8389_record_stop();
    }
}

/**
 * @brief       兼容: DAC 开关
 * @note        直接委托 play_start/stop
 */
void myes8389_dac_enable(bool enable)
{
    if (enable) {
        myes8389_play_start(I2S_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT);
    } else {
        myes8389_play_stop();
    }
}

/**
 * @brief       兼容: 同步采样率/位宽 (只动 ES8389 codec, 不动 I2S)
 * @param       sample_rate      : 采样率 (Hz)
 * @param       bits_per_sample  : 位宽 (如 16)
 * @retval      无
 */
void myes8389_set_sample_info(int sample_rate, int bits_per_sample)
{
    if (s_es8389_codec_if == NULL) return;

    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = bits_per_sample,
        .channel         = 2,
        .channel_mask    = 0x03,
        .sample_rate     = sample_rate,
    };
    s_es8389_codec_if->hw_base.set_fs(&s_es8389_codec_if->hw_base,
                                       &sample_cfg,
                                       ESP_CODEC_DEV_TYPE_IN_OUT);

    /* 覆盖 PLL: set_fs 按 ref_enabled=false 写 ×2 */
    if (s_playing_active) {
        myes8389_set_dac_ref(true);  /* 播放态需要 ×4 */
    } else if (s_recording_active) {
        myes8389_set_dac_ref(false); /* 录音态就是 ×2, 保持不变 */
    }
}

/**
 * @brief       兼容包装: 供 wavplay.c 调用 (设置 codec 采样率/位宽 + 覆盖 PLL)
 * @param       sample_rate      : 采样率 (Hz)
 * @param       bits_per_sample  : 位宽 (如 16)
 * @retval      固定返回 0
 */
uint8_t myes8389_set_fs(uint32_t sample_rate, uint16_t bits_per_sample)
{
    myes8389_set_sample_info((int)sample_rate, (int)bits_per_sample);
    return 0;
}

/* ---- 音量 ---- */
/**
 * @brief   设置播放音量
 * @note    超出 0~100 自动钳位; 播放中立即写入 DAC, 未播放时只记录,
 *          下次 myes8389_play_start() 生效
 * @param   vol  音量 0~100
 * @retval  无
 */
void myes8389_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;

    if (s_playing_active && s_play_dev) {
        esp_codec_dev_set_out_vol(s_play_dev, s_volume);
    }
}

/**
 * @brief   读取当前播放音量
 * @param   无
 * @retval  音量 0~100
 */
int myes8389_get_volume(void)
{
    return s_volume;
}

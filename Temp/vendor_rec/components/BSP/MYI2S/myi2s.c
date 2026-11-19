/**
 ******************************************************************************
 * @file        i2s.c
 * @version     V1.0
 * @brief       I2S驱动代码
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:     慧勤智远 ESP32-S31 开发板
 ******************************************************************************
 */

#include "myi2s.h"
#include "esp_log.h"
#include "esp_err.h"


i2s_chan_handle_t tx_handle = NULL;     /* I2S发送通道句柄 */
i2s_chan_handle_t rx_handle = NULL;     /* I2S接收通道句柄 */
i2s_std_config_t my_std_cfg;            /* 标准I2S模式配置结构体 */

/* 通道使能状态 */
static bool s_tx_enabled = false;
static bool s_rx_enabled = false;

/*
 * @brief       初始化I2S
 * @param       无
 * @retval      ESP_OK:初始化成功;其他:失败
 */
esp_err_t myi2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);  /* 默认通道配置(I2S0,主机) */
    chan_cfg.auto_clear = true;                                                         /* 自动清除DMA缓冲区遗留数据 */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));                /* 分配新的I2S通道(全双工) */

    /* I2S配置 */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),                         /* 采样率16kHz,默认时钟源 */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {                                                                   /* 引脚配置 */
            .mclk = I2S_MCK_IO,                                                         /* 无MCLK,时钟由BCLK派生 */
            .bclk = I2S_BCK_IO,                                                         /* 位时钟线 */
            .ws   = I2S_WS_IO,                                                          /* 字(声道)选择线 */
            .dout = I2S_DO_IO,                                                          /* 串行数据输出线 */
            .din  = I2S_DI_IO,                                                          /* 串行数据输入线 */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    my_std_cfg = std_cfg;
    i2s_std_config_t rx_cfg = std_cfg;
    rx_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));                   /* 初始化TX通道(标准I2S,APLL) */
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_cfg));                    /* 初始化RX通道(标准I2S,默认时钟) */
    ESP_LOGI("myi2s", "I2S channels initialized in STD mode (sample_rate=%d)",
             (int)std_cfg.clk_cfg.sample_rate_hz);

    i2s_trx_start();                                                                   /* 启动TX+RX */

    return ESP_OK;
}

/**
 * @brief       I2S TRX启动
 * @param       无
 * @retval      无
 */
void i2s_trx_start(void)
{
    if (!s_tx_enabled)
    {
        esp_err_t ret = i2s_channel_enable(tx_handle);
        if (ret == ESP_OK)
        {
            s_tx_enabled = true;
        }
        else
        {
            ESP_LOGW("myi2s", "tx enable: %s (continue)", esp_err_to_name(ret));
        }
    }
    if (!s_rx_enabled)
    {
        esp_err_t ret = i2s_channel_enable(rx_handle);
        if (ret == ESP_OK)
        {
            s_rx_enabled = true;
        }
        else
        {
            ESP_LOGW("myi2s", "rx enable: %s (continue)", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief       I2S TRX停止
 * @param       无
 * @retval      无
 */
void i2s_trx_stop(void)
{
    if (s_rx_enabled)
    {
        esp_err_t ret = i2s_channel_disable(rx_handle);
        if (ret == ESP_OK)
        {
            s_rx_enabled = false;
        }
        else
        {
            ESP_LOGW("myi2s", "rx disable: %s (continue)", esp_err_to_name(ret));
        }
    }
    if (s_tx_enabled)
    {
        esp_err_t ret = i2s_channel_disable(tx_handle);
        if (ret == ESP_OK)
        {
            s_tx_enabled = false;
        }
        else
        {
            ESP_LOGW("myi2s", "tx disable: %s (continue)", esp_err_to_name(ret));
        }
    }
}

/**
 * @brief       I2S卸载
 * @param       无
 * @retval      无
 */
void i2s_deinit(void)
{
    ESP_ERROR_CHECK(i2s_del_channel(tx_handle));
    ESP_ERROR_CHECK(i2s_del_channel(rx_handle));
}

/**
 * @brief       设置采样率和位宽(标准I2S模式)
 * @param       sampleRate  :采样率
 * @param       bits_sample :位宽
 * @retval      无
 */
void i2s_set_samplerate_bits_sample(int samplerate, int bits_sample)
{
    i2s_trx_stop();

    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits_sample, I2S_SLOT_MODE_STEREO);
    slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    my_std_cfg.slot_cfg = slot_cfg;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle, &my_std_cfg.slot_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(rx_handle, &my_std_cfg.slot_cfg));

    my_std_cfg.clk_cfg.sample_rate_hz = samplerate;                       /* 设置采样率 */
    my_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &my_std_cfg.clk_cfg));  /* TX(主机):APLL */

    i2s_std_clk_config_t rx_clk = my_std_cfg.clk_cfg;
    rx_clk.clk_src = I2S_CLK_SRC_DEFAULT;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(rx_handle, &rx_clk));  /* RX(从机):默认时钟,跟随TX */
}

/**
 * @brief       将TX(主机)时钟源重配为APLL(精确采样率)
 * @param       无
 * @retval      无
 */
void i2s_set_tx_clock_apll(void)
{
    bool was_enabled = s_tx_enabled;
    if (was_enabled)
    {
        i2s_channel_disable(tx_handle);
        s_tx_enabled = false;
    }

    my_std_cfg.clk_cfg.sample_rate_hz = I2S_SAMPLE_RATE;
    my_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(tx_handle, &my_std_cfg.clk_cfg));

    if (was_enabled)
    {
        i2s_channel_enable(tx_handle);
        s_tx_enabled = true;
    }
}

/**
 * @brief       I2S传输数据
 * @param       buffer: 数据存储区的首地址
 * @param       frame_size: 数据大小
 * @retval      发送的数据长度
 */
size_t i2s_tx_write(uint8_t *buffer, uint32_t frame_size)
{
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(tx_handle, buffer, frame_size, &bytes_written, 1000);

    if (ret != ESP_OK)  
    {
        ESP_LOGW("myi2s", "i2s tx write failed: %s", esp_err_to_name(ret));
    }

    return bytes_written;
}

/**
 * @brief       I2S读取数据
 * @param       buffer: 读取数据存储区的首地址
 * @param       frame_size: 读取数据大小
 * @retval      接收的数据长度
 */
size_t i2s_rx_read(uint8_t *buffer, uint32_t frame_size)
{
    size_t bytes_read = 0;
    esp_err_t ret = i2s_channel_read(rx_handle, buffer, frame_size, &bytes_read, 1000);

    if (ret != ESP_OK)  
    {
        ESP_LOGW("myi2s", "i2s rx read failed: %s", esp_err_to_name(ret));
    }

    return bytes_read;
}

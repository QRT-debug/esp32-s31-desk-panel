/**
 * @file    touch_service.c
 * @brief   触摸服务实现：10 ms 轮询 + 2 次一致去抖 + 事件队列
 */
#include "touch_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "touch.h"

static const char *TAG = "touch_svc";

#define TOUCH_POLL_MS       10
#define TOUCH_DEBOUNCE_CNT  2      /* 连续 2 次读到同一状态才算数（20 ms） */
#define TOUCH_EVT_Q_LEN     16

static QueueHandle_t     s_q    = NULL;
static TaskHandle_t      s_task = NULL;
static volatile uint16_t s_x    = 0;
static volatile uint16_t s_y    = 0;
static volatile bool     s_down = false;

static void touch_task(void *arg)
{
    (void)arg;

    bool     stable  = false;
    int      same    = 0;
    uint16_t x = 0, y = 0;
    int      up_hold = 0;   /* UP 确认计数 */

    while (1)
    {
        touch_state_t ts = {0};
        bool     cur     = false;
        uint16_t cx = s_x, cy = s_y;

        if (touch_read(&ts) == ESP_OK && ts.pressed && ts.count > 0)
        {
            cur = true;
            cx  = ts.pt[0].x;
            cy  = ts.pt[0].y;
        }

        if (cur == stable)
        {
            same = 0;
        }
        else if (++same >= TOUCH_DEBOUNCE_CNT)
        {
            stable = cur;
            same   = 0;

            touch_evt_t e = {
                .type = cur ? TOUCH_EV_DOWN : TOUCH_EV_UP,
                .x    = cx,
                .y    = cy,
            };
            /* 队列满就丢弃：宁可丢一次点击，也不能阻塞扫描 */
            xQueueSend(s_q, &e, 0);
            ESP_LOGD(TAG, "evt %s (%u,%u)", cur ? "DOWN" : "UP", cx, cy);
        }

        /* ★ 状态路径的 UP 消抖（DOWN 不消抖，灵敏优先）：
         *   GT1151 对轻点的报点延迟大——手指抬起后"按下帧"才吐出来，
         *   s_down=true 的窗口只有一两个轮询周期（10~20ms），而 LVGL indev
         *   30ms 才采样一次 ⇒ 轻点被完美错过（长按灵敏/轻点完全不灵的根源）。
         *   修法：cur==false 需连续 3 次（30ms）才把 s_down 清掉，
         *   按下窗口被拉长到 LVGL 必然读到；松开的 30ms 延迟人无感知。 */
        if (cur)
        {
            up_hold = 0;
        }
        else if (s_down && ++up_hold < 3)
        {
            cur = true;    /* UP 延迟确认：保持按下（坐标沿用旧值） */
        }

        if (cur)
        {
            s_x = cx;
            s_y = cy;
        }
        s_down = cur;

        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

esp_err_t touch_service_start(void)
{
    if (s_task != NULL)
    {
        return ESP_OK;      /* 已经启动过 */
    }

    esp_err_t err = touch_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "touch_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_q = xQueueCreate(TOUCH_EVT_Q_LEN, sizeof(touch_evt_t));
    if (s_q == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(touch_task, "touch", 3072, NULL, 6, &s_task);
    if (ok != pdPASS)
    {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "started, poll %d ms, debounce %d", TOUCH_POLL_MS, TOUCH_DEBOUNCE_CNT);
    return ESP_OK;
}

bool touch_get_event(touch_evt_t *evt)
{
    if (s_q == NULL || evt == NULL)
    {
        return false;
    }
    return xQueueReceive(s_q, evt, 0) == pdTRUE;
}

void touch_get_last(uint16_t *x, uint16_t *y, bool *pressed)
{
    if (x) *x = s_x;
    if (y) *y = s_y;
    if (pressed) *pressed = s_down;
}

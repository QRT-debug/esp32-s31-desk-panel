/**
 * @file    touch.c
 * @brief   触摸抽象层：当前实现为 GT1151Q / GT1158
 */
#include "touch.h"

#include <stdio.h>

#include "gt1151.h"

esp_err_t touch_init(void)
{
    return gt1151_init();
}

esp_err_t touch_read(touch_state_t *st)
{
    if (st == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t xs[TOUCH_MAX_POINTS] = {0};
    uint16_t ys[TOUCH_MAX_POINTS] = {0};
    uint8_t  n = 0;

    esp_err_t err = gt1151_read(&n, xs, ys, TOUCH_MAX_POINTS);
    if (err != ESP_OK)
    {
        st->count   = 0;
        st->pressed = false;
        return err;
    }

    st->count   = n;
    st->pressed = (n > 0);

    for (uint8_t i = 0; i < n; i++)
    {
        st->pt[i].x = xs[i];
        st->pt[i].y = ys[i];
    }
    return ESP_OK;
}

const char *touch_id(void)
{
    return gt1151_id();
}

const char *touch_debug_str(void)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "raw=0x%02X n=%u i2c_err=%lu",
             gt1151_raw_status(), gt1151_touch_count(),
             (unsigned long)gt1151_err_count());
    return buf;
}

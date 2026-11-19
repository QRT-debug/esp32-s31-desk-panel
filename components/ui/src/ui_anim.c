/**
 * @file    ui_anim.c
 * @brief   页面过渡动画
 *
 * ★ 位移只做 160px（不是整屏 800px）：位移越大，需要重绘的面积越大，
 *   在 PSRAM 直驱的 RGB 屏上会明显掉帧（带宽是瓶颈，不是 CPU）。
 */
#include "ui_anim.h"

void ui_slide_in(lv_obj_t *root)
{
    if (root == NULL)
    {
        return;
    }

    lv_obj_set_x(root, 160);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_values(&a, 160, 0);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_start(&a);
}

/**
 * 语音页：大按钮触发录音，结果大字显示。
 *
 * ★ 分层约定：本文件只做 UI。录音/识别全在 voice 组件里完成，
 *   事件通过订阅回调进来；回调在 voice_task 上下文，**只写 volatile 标志**，
 *   由本场景 draw（LVGL 上下文）轮询消费——绝不从回调里碰 LVGL（async_call 也死锁过）。
 */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "ui_scenes.h"
#include "ui_fonts.h"
#include "ui_theme.h"
#include "lvgl_port.h"
#include "voice.h"
#include "ui_voice_intent.h"

static lv_obj_t *s_btn_mic;
static lv_obj_t *s_lbl_btn;
static lv_obj_t *s_lbl_state;
static lv_obj_t *s_lbl_result;

static char          s_buf[256];
static volatile bool s_ready;
static bool          s_subscribed;

/* ★ 意图动作桥：回调（voice_task）只写，draw（LVGL 上下文）执行。
 *   绝不能在回调里调 ui_goto_scene/backlight_set —— 那会跨线程碰 UI。 */
static volatile ui_vact_t s_act     = UI_VACT_NONE;
static volatile int       s_act_arg = 0;

static void on_voice_evt(const voice_evt_t *e)
{
    if (e->type == VOICE_EV_TEXT || e->type == VOICE_EV_ERROR)
    {
        snprintf(s_buf, sizeof(s_buf), "%s", e->text ? e->text : "(空)");
        s_ready = true;

        /* 意图解析是纯函数，这里做；执行留给 draw（LVGL 上下文） */
        if (e->type == VOICE_EV_TEXT)
        {
            ui_vact_t act;
            int       arg;
            if (ui_voice_intent(e->text, &act, &arg))
            {
                s_act_arg = arg;
                s_act     = act;      /* 最后写：保证 arg 先就绪 */
            }
        }
    }
}

static void mic_cb(lv_event_t *e)
{
    (void)e;
    if (!voice_listen())
    {
        snprintf(s_buf, sizeof(s_buf), "(设备忙：放歌中或已在听)");
        s_ready = true;
    }
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_goto_scene(UI_SCENE_APPS);   /* 本页从应用页进入，返回也回应用页（原为 SYSTEM） */
}

void scene_voice(uint16_t *fb, int frame)
{
    (void)fb;

    if (!lvgl_port_ready())
    {
        return;
    }

    if (frame == 0)
    {
        lv_obj_clean(lv_scr_act());
        ui_statusbar_show(true);

        if (!s_subscribed)
        {
            voice_subscribe(on_voice_evt);
            s_subscribed = true;
        }

        lv_obj_t *scr = lv_scr_act();

        /* 返回按钮 */
        lv_obj_t *back = lv_obj_create(scr);
        lv_obj_set_size(back, 88, 30);
        lv_obj_set_pos(back, 16, 12);
        lv_obj_set_style_bg_color(back, lv_color_hex(ui_theme()->card_line), 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(back, 8, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        lv_obj_set_style_pad_all(back, 0, 0);
        lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *bl = lv_label_create(back);
        lv_obj_set_style_text_font(bl, &ui_font_14, 0);
        lv_obj_set_style_text_color(bl, lv_color_hex(ui_theme()->title), 0);
        lv_label_set_text(bl, "BACK");
        lv_obj_center(bl);

        /* 标题 */
        lv_obj_t *title = lv_label_create(scr);
        lv_obj_set_style_text_font(title, &ui_font_28, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(ui_theme()->title), 0);
        lv_label_set_text(title, "VOICE");
        lv_obj_set_pos(title, 120, 12);

        /* 大录音按钮 */
        s_btn_mic = lv_obj_create(scr);
        lv_obj_set_size(s_btn_mic, 168, 96);
        lv_obj_align(s_btn_mic, LV_ALIGN_TOP_MID, 0, 76);
        lv_obj_set_style_bg_color(s_btn_mic, lv_color_hex(0xFF9A3D), 0);
        lv_obj_set_style_bg_opa(s_btn_mic, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_btn_mic, 20, 0);
        lv_obj_set_style_border_width(s_btn_mic, 0, 0);
        lv_obj_set_style_pad_all(s_btn_mic, 0, 0);
        lv_obj_clear_flag(s_btn_mic, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_btn_mic, mic_cb, LV_EVENT_CLICKED, NULL);

        s_lbl_btn = lv_label_create(s_btn_mic);
        lv_obj_set_style_text_font(s_lbl_btn, &ui_font_20, 0);
        lv_obj_set_style_text_color(s_lbl_btn, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text(s_lbl_btn, "TALK");
        lv_obj_center(s_lbl_btn);

        /* 状态行 */
        s_lbl_state = lv_label_create(scr);
        lv_obj_set_style_text_font(s_lbl_state, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_state, lv_color_hex(ui_theme()->sub), 0);
        lv_label_set_text(s_lbl_state, "点 TALK 开始说话");
        lv_obj_align(s_lbl_state, LV_ALIGN_TOP_MID, 0, 190);

        /* 识别结果（大字、可换行） */
        s_lbl_result = lv_label_create(scr);
        lv_obj_set_style_text_font(s_lbl_result, &ui_font_cn18, 0);
        lv_obj_set_style_text_color(s_lbl_result, lv_color_hex(ui_theme()->title), 0);
        lv_label_set_text(s_lbl_result, "");
        lv_obj_set_width(s_lbl_result, 720);
        lv_obj_set_pos(s_lbl_result, 40, 240);
        lv_label_set_long_mode(s_lbl_result, LV_LABEL_LONG_WRAP);
    }

    /* 轮询（LVGL 上下文） */
    if (s_ready && s_lbl_result != NULL)
    {
        s_ready = false;
        lv_label_set_text(s_lbl_result, s_buf);
    }

    /* 执行语音命令（LVGL 上下文）：切场景后本页随即被新场景 clean 掉 */
    if (s_act != UI_VACT_NONE)
    {
        ui_vact_t act = s_act;
        int       arg = s_act_arg;
        s_act = UI_VACT_NONE;
        ui_voice_intent_apply(act, arg);
        return;      /* 场景可能已切换/重建，本帧不再动控件 */
    }
    if (s_lbl_state != NULL)
    {
        bool listening = voice_is_listening();
        lv_label_set_text(s_lbl_state, listening ? "正在听…（说完停一秒自动结束）"
                                                 : "点 TALK 开始说话");
        if (s_lbl_btn != NULL)
        {
            lv_label_set_text(s_lbl_btn, listening ? "..." : "TALK");
        }
    }

    lvgl_port_task();
}

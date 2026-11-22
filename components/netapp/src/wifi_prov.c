/**
 * @file    wifi_prov.c
 * @brief   配网实现：SoftAP(APSTA 共存) + esp_http_server + 内嵌表单
 *
 * 设计取舍：
 *   - **不用文件系统存页面**：HTML 内嵌成字符串（约 4KB flash），省掉"文件系统挂载失败
 *     就打不开配网页"这条额外故障路径。配置页是救命的，越少依赖越好。
 *   - **AP 与 STA 共存**：已联网的设备改配置时不断网，保存后直接切过去；
 *     新设备连不上时 AP 单独工作，行为一致。
 *   - **保存后重启**：WiFi 模式/凭据变更后，重启是最干净的状态（避免
 *     "改完一半生效"这类难查的问题）。配置页就一次操作，多等 3 秒无所谓。
 */
#include "wifi_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_cfg.h"
#include "backlight.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "prov";

/* ★ 统一用 strlcpy 而不是 snprintf：目标缓冲比源小，snprintf 会被
 *   -Werror=format-truncation 判定"可能截断"（源是 128 字节的 tmp）。 */

static httpd_handle_t s_srv         = NULL;
static char           s_ssid[33]    = {0};
static bool           s_ap_netif_ok = false;

/* ================= 小工具 ================= */

/** HTML 属性值转义：只处理会破坏属性/文本的四个字符 */
static void html_esc(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 6 < dstsz; i++)
    {
        switch (src[i])
        {
            case '&':  memcpy(dst + o, "&amp;", 5);  o += 5; break;
            case '<':  memcpy(dst + o, "&lt;", 4);   o += 4; break;
            case '>':  memcpy(dst + o, "&gt;", 4);   o += 4; break;
            case '"':  memcpy(dst + o, "&quot;", 6); o += 6; break;
            default:   dst[o++] = src[i];                     break;
        }
    }
    dst[o] = '\0';
}

/** application/x-www-form-urlencoded 解码：%XX 与 '+' */
static void url_decode(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] != '\0' && o + 1 < dstsz; i++)
    {
        if (src[i] == '+')
        {
            dst[o++] = ' ';
        }
        else if (src[i] == '%' && src[i + 1] != '\0' && src[i + 2] != '\0')
        {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        else
        {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

/**
 * 从 urlencoded body 里取出某个 key 的值（已解码）。
 * 找不到返回 false，此时 out 置空串（表单允许留空）。
 */
static bool form_get(const char *body, const char *key, char *out, size_t outsz)
{
    out[0] = '\0';

    size_t      klen = strlen(key);
    const char *p    = body;

    while (p != NULL && *p != '\0')
    {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=')
        {
            const char *v    = p + klen + 1;
            const char *end  = strchr(v, '&');
            size_t      vlen = (end != NULL) ? (size_t)(end - v) : strlen(v);

            char raw[160];
            if (vlen >= sizeof(raw))
            {
                vlen = sizeof(raw) - 1;
            }
            memcpy(raw, v, vlen);
            raw[vlen] = '\0';

            url_decode(raw, out, outsz);
            return true;
        }

        const char *amp = strchr(p, '&');
        p = (amp != NULL) ? amp + 1 : NULL;
    }
    return false;
}

/* ================= 页面 ================= */

/** 拼出配置页（当前值预填）。返回 malloc 的字符串，调用方 free。 */
static char *build_page(void)
{
    const app_cfg_t *c = app_cfg_get();

    char esc_ssid[160], esc_city[80], esc_host[160];
    html_esc(c->wifi_ssid, esc_ssid, sizeof(esc_ssid));
    html_esc(c->city, esc_city, sizeof(esc_city));
    html_esc(c->pc_host, esc_host, sizeof(esc_host));

    const size_t cap = 6144;
    char        *p   = malloc(cap);
    if (p == NULL)
    {
        return NULL;
    }

    int n = snprintf(p, cap,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        /* ★ 必须禁缓存：手机浏览器会把 192.168.4.1 的页面缓存下来，
         *   而 ESP32 的 SoftAP 默认 IP 都是这个 —— 用户打开的可能是一台
         *   **完全不同的设备**（或别的项目）留下的旧页面。 */
        "<meta http-equiv=Cache-Control content='no-store,no-cache,must-revalidate'>"
        "<title>SmartPanel 配置</title><style>"
        ":root{color-scheme:dark}"
        "body{margin:0;padding:18px;background:#0f1420;color:#e6edf7;"
        "font:15px/1.55 -apple-system,system-ui,'Segoe UI',sans-serif}"
        "h1{font-size:19px;margin:0 0 4px}"
        "p.sub{margin:0 0 18px;color:#8a97ad;font-size:13px}"
        "form{max-width:520px}"
        "fieldset{border:1px solid #263149;border-radius:12px;padding:14px 14px 6px;margin:0 0 14px}"
        "legend{padding:0 6px;color:#7c8aa5;font-size:12px;letter-spacing:.08em}"
        "label{display:block;margin:10px 0 4px;font-size:13px;color:#a9b6cc}"
        "input,select{width:100%%;box-sizing:border-box;padding:9px 11px;border-radius:8px;"
        "border:1px solid #2b3752;background:#161d2b;color:#e6edf7;font-size:15px}"
        "input:focus,select:focus{outline:none;border-color:#5b7cfa}"
        "input[type=range]{padding:0}"
        "small{display:block;margin-top:4px;color:#77859c;font-size:12px}"
        "button{width:100%%;padding:13px;border:0;border-radius:10px;"
        "background:#5b7cfa;color:#fff;font-size:16px;font-weight:600}"
        "button:active{background:#4a68d8}"
        "</style>"
        "<h1>SmartPanel 配置</h1>"
        "<p class=sub>当前设备：<b>%s</b><br>改完点保存，设备会自动重启连上新的网络。</p>"
        "<form method=post action=/save>"

        "<fieldset><legend>WiFi</legend>"
        "<label>名称 SSID</label>"
        "<input name=wifi_ssid value='%s' maxlength=32 required>"
        "<label>密码</label>"
        "<input name=wifi_pass type=password value='%s' maxlength=64 placeholder='开放网络请留空'>"
        "</fieldset>"

        "<fieldset><legend>位置（天气 / 空气质量）</legend>"
        "<label>城市</label>"
        "<input name=city value='%s' maxlength=23 placeholder='如 北京'>"
        "<small>留空 = 按公网 IP 自动定位。天气是按 3~11km 网格提供的，填到市级即可，区/街道不会更准。</small>"
        "<label>经纬度（可选，优先级高于城市）</label>"
        "<input name=lat value='%.4f' placeholder='39.95'>"
        " <input name=lon value='%.4f' placeholder='116.32'>"
        "<small>在手机地图上长按你家位置可读到坐标；填了这里就忽略上面的城市。</small>"
        "</fieldset>"

        "<fieldset><legend>PC 中转</legend>"
        "<label>PC 地址</label>"
        "<input name=pc_host value='%s' maxlength=39 placeholder='192.168.1.10'>"
        "<label>端口</label>"
        "<input name=pc_port type=number min=1 max=65535 value=%u>"
        "<small>先在 PC 上运行 tools/pc_monitor_server.py，它会打印本机 IP。B 站热搜和 PC 状态共用这个地址。</small>"
        "</fieldset>"

        "<fieldset><legend>显示</legend>"
        "<label>背光亮度：<span id=bv>%u</span>%%</label>"
        "<input name=backlight type=range min=10 max=100 value=%u "
        "oninput=\"document.getElementById('bv').textContent=this.value\">"
        "<label>时区（UTC 偏移小时）</label>"
        "<input name=tz type=number min=-12 max=14 value=%d>"
        "</fieldset>"

        "<button type=submit>保存并重启</button>"
        "</form>",

        s_ssid,
        esc_ssid, c->wifi_pass, esc_city, (double)c->lat, (double)c->lon,
        esc_host, (unsigned)c->pc_port,
        (unsigned)c->backlight, (unsigned)c->backlight, (int)c->tz);

    if (n < 0 || (size_t)n >= cap)
    {
        ESP_LOGW(TAG, "page truncated (%d/%u)", n, (unsigned)cap);
    }
    return p;
}

/* ================= 路由 ================= */

static esp_err_t h_root(httpd_req_t *req)
{
    char *page = build_page();
    if (page == NULL)
    {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* ★ 双保险：meta 之外再发响应头，防止浏览器"启发式缓存"直接给旧页面 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t h_save(httpd_req_t *req)
{
    /* ★ content_len 上限保护：配置页只有十来个字段，2KB 足够，超了直接拒 */
    if (req->content_len <= 0 || req->content_len > 2048)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }

    char *body = calloc(1, (size_t)req->content_len + 1);
    if (body == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0)
    {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }
    body[got] = '\0';

    app_cfg_t *c   = app_cfg_mut();
    char       tmp[128];

    if (form_get(body, "wifi_ssid", tmp, sizeof(tmp)) && tmp[0] != '\0')
    {
        strlcpy(c->wifi_ssid, tmp, sizeof(c->wifi_ssid));
    }
    if (form_get(body, "wifi_pass", tmp, sizeof(tmp)))
    {
        strlcpy(c->wifi_pass, tmp, sizeof(c->wifi_pass));
    }
    if (form_get(body, "city", tmp, sizeof(tmp)))
    {
        strlcpy(c->city, tmp, sizeof(c->city));
    }
    if (form_get(body, "lat", tmp, sizeof(tmp)))
    {
        c->lat = (float)atof(tmp);
    }
    if (form_get(body, "lon", tmp, sizeof(tmp)))
    {
        c->lon = (float)atof(tmp);
    }
    if (form_get(body, "pc_host", tmp, sizeof(tmp)))
    {
        strlcpy(c->pc_host, tmp, sizeof(c->pc_host));
    }
    if (form_get(body, "pc_port", tmp, sizeof(tmp)))
    {
        int port = atoi(tmp);
        if (port > 0 && port <= 65535)
        {
            c->pc_port = (uint16_t)port;
        }
    }
    if (form_get(body, "backlight", tmp, sizeof(tmp)))
    {
        int bl = atoi(tmp);
        c->backlight = (uint8_t)((bl < 10) ? 10 : (bl > 100 ? 100 : bl));
    }
    if (form_get(body, "tz", tmp, sizeof(tmp)))
    {
        int tz = atoi(tmp);
        c->tz = (int8_t)((tz < -12) ? -12 : (tz > 14 ? 14 : tz));
    }

    free(body);

    /* ★★★ 写 NVS 前先把背光关掉 —— 不是"礼貌"，是遮一个真实的时序故障：
     *
     *   nvs_commit() 写 flash 期间会**禁用 instruction cache**，而 LVGL 的渲染代码住在
     *   flash 里 ⇒ 任务一被调度就 stall。偏偏 flush_cb 等 vsync 只有 200ms 超时，
     *   一旦超时就放弃等待继续渲染，画面时序立刻错乱、出现撕裂（"失真"）。
     *   实测用户看到的就是"点保存 → 屏幕花掉 → 然后重启"。
     *
     *   既然紧接着就要重启，先把背光关掉：用户看到的是"黑屏 → 重启"，
     *   这既符合"保存"的心理预期，也把那段异常藏起来。
     *   ★ 重启后 backlight_init 的 duty=0 + 首帧后 backlight_default() 会自动恢复 75%。
     */
    const int bl_prev = backlight_get();      /* ★ 必须先存旧值：backlight_set(0) 会把 s_percent 也改成 0 */
    backlight_set(0);
    vTaskDelay(pdMS_TO_TICKS(80));            /* 让背光先真正灭掉，别在亮着的时候开始写 */

    esp_err_t err = app_cfg_save();
    if (err != ESP_OK)
    {
        backlight_set(bl_prev);               /* 失败就还回去，别留个黑屏给用户 */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs save failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved from web form");
    app_cfg_dump();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style=\"background:#0f1420;color:#e6edf7;font-family:system-ui;"
        "padding:40px 20px;text-align:center\">"
        "<h2>已保存</h2><p style=color:#8a97ad>设备正在重启并连接新网络…</p>",
        HTTPD_RESP_USE_STRLEN);

    /* ★ 不能立刻 restart（响应可能还没从 TCP 缓冲发出去）。
     *   给 1.2s 让浏览器收到，再重启；配置页不追求响应速度。 */
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;   /* 不会执行到这里 */
}

/* ================= 对外 ================= */

bool wifi_prov_running(void)
{
    return s_srv != NULL;
}

const char *wifi_prov_ssid(void)
{
    return s_ssid;
}

esp_err_t wifi_prov_start(void)
{
    if (s_srv != NULL)
    {
        return ESP_OK;
    }

    /* ---- AP 的 netif 只能创建一次 ---- */
    if (!s_ap_netif_ok)
    {
        if (esp_netif_create_default_wifi_ap() == NULL)
        {
            ESP_LOGE(TAG, "create ap netif failed");
            return ESP_FAIL;
        }
        s_ap_netif_ok = true;
    }

    /* ---- SSID 带 MAC 后 2 字节，多台设备放一起不会串 ---- */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), PROV_AP_PREFIX "%02X%02X", mac[4], mac[5]);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, s_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len       = (uint8_t)strlen(s_ssid);
    wc.ap.channel        = 1;
    wc.ap.max_connection = 4;
    wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    strlcpy((char *)wc.ap.password, PROV_AP_PASS, sizeof(wc.ap.password));

    /* ★ APSTA：已联网的设备改配置时不断网。set_mode 会让 WiFi 重启一次，
     *   STA 会短暂断开再自动重连（凭据没变，所以能连回来）。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 4;
    hc.stack_size       = 6144;
    hc.lru_purge_enable = true;

    if (httpd_start(&s_srv, &hc) != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd start failed");
        s_srv = NULL;
        return ESP_FAIL;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = h_root, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = h_save, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_srv, &uri_root);
    httpd_register_uri_handler(s_srv, &uri_save);

    ESP_LOGI(TAG, "AP \"%s\" up, open http://%s/", s_ssid, PROV_AP_IP);
    return ESP_OK;
}

esp_err_t wifi_prov_stop(void)
{
    if (s_srv != NULL)
    {
        httpd_stop(s_srv);
        s_srv = NULL;
    }
    /* 回到纯 STA（不重启，STA 连接保持） */
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

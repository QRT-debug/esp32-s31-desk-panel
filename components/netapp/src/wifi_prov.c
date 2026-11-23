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

#include "wifi_sta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

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

    char esc_ssid[160], esc_city[80], esc_host[160], esc_asr_url[200], esc_asr_key[140];
    html_esc(c->wifi_ssid, esc_ssid, sizeof(esc_ssid));
    html_esc(c->city, esc_city, sizeof(esc_city));
    html_esc(c->pc_host, esc_host, sizeof(esc_host));
    html_esc(c->asr_url, esc_asr_url, sizeof(esc_asr_url));
    html_esc(c->asr_key, esc_asr_key, sizeof(esc_asr_key));

    const size_t cap = 8192;
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

        "<fieldset><legend>壁纸（屏保用）</legend>"
        "<input type=file id=wf accept='image/jpeg'>"
        "<button type=button onclick=up() style='margin-top:10px'>上传到设备</button>"
        "<small>S31 的硬件解码器只认 JPEG。请先裁成 800×480（5:3），"
        "否则会变形。最多存 10 张，满了覆盖最旧的一张。</small>"
        "<button type=button onclick=wpclr() style='margin-top:6px'>清空全部壁纸</button>"
        "<div id=ur style='margin-top:8px;color:#8a97ad;font-size:13px'></div>"
        "</fieldset>"
        "<fieldset><legend>语音服务（录音识别）</legend>"
        "<label>ASR 地址（留空 = PC 中转 /api/asr）<input name=asr_url value='%s' placeholder='https://api.openai.com/v1/audio/transcriptions'></label>"
        "<label>ASR Key（PC 中转模式可留空）<input name=asr_key type=password value='%s' placeholder='sk-...'></label>"
        "<label>VAD 灵敏度（越小越灵敏，50~20000）<input name=vad_thresh type=number value=%u></label>"
        "<label>最长录音秒数（1~30）<input name=rec_max_sec type=number value=%u></label>"
        "</fieldset>"

        "<button type=submit>保存并重启</button>"
        "</form>"
        "<script>"
        "function up(){"
        "var f=document.getElementById('wf').files[0];"
        "var r=document.getElementById('ur');"
        "if(!f){r.textContent='请先选一张 JPG';return;}"
        "r.textContent='上传中…（'+Math.round(f.size/1024)+' KB）';"
        "fetch('/upload',{method:'POST',body:f})"
        ".then(function(x){return x.text()})"
        ".then(function(t){r.textContent=t})"
        ".catch(function(e){r.textContent='失败：'+e});"
        "}"
        "function wpclr(){"
        "var r=document.getElementById('ur');"
        "r.textContent='清空中…';"
        "fetch('/wpclear')"
        ".then(function(x){return x.text()})"
        ".then(function(t){r.textContent=t})"
        ".catch(function(e){r.textContent='失败：'+e});"
        "}"
        "</script>",

        s_ssid,
        esc_ssid, c->wifi_pass, esc_city, (double)c->lat, (double)c->lon,
        esc_host, (unsigned)c->pc_port,
        (unsigned)c->backlight, (unsigned)c->backlight, (int)c->tz,
        esc_asr_url, esc_asr_key,
        (unsigned)c->vad_thresh, (unsigned)c->rec_max_sec);

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

/**
 * 壁纸上传：POST /upload，body 直接是 JPEG 字节（不走 multipart，省掉解析）。
 *
 * ★★ 内存约束是这个函数唯一的设计要点：内部 RAM 只有 ~300KB，
 *   不能把整张图读进内存。所以用 4KB 缓冲**边收边写** SD 卡，
 *   内存占用恒定，与文件大小无关。
 * ★ 写 SD 卡走 SDMMC（不是 SPI flash），**不会禁用 i-cache**，不会造成画面撕裂；
 *   但它会阻塞当前任务几十毫秒，所以别在用户正在滑动时上传。
 */
#define WP_DIR     "/sdcard/wallpaper"
#define WP_SLOTS   10
#define WP_MAX_SZ  (512 * 1024)

static esp_err_t h_upload(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > WP_MAX_SZ)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "size must be 1..512KB");
        return ESP_FAIL;
    }

    /* 目录不存在就建（已存在时 mkdir 返回 EEXIST，忽略即可） */
    mkdir(WP_DIR, 0777);

    /* 找一个空槽；10 个都占满就覆盖 up_0 */
    char path[64];
    int  slot = 0;
    for (; slot < WP_SLOTS; slot++)
    {
        snprintf(path, sizeof(path), WP_DIR "/up_%d.jpg", slot);
        FILE *probe = fopen(path, "rb");
        if (probe == NULL)
        {
            break;                    /* 这个槽空着 */
        }
        fclose(probe);
    }
    if (slot >= WP_SLOTS)
    {
        slot = 0;
        snprintf(path, sizeof(path), WP_DIR "/up_%d.jpg", slot);
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "open %s failed", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sd write failed");
        return ESP_FAIL;
    }

    uint8_t *buf = malloc(4096);
    if (buf == NULL)
    {
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t remaining = (size_t)req->content_len;
    size_t written   = 0;
    while (remaining > 0)
    {
        size_t want = (remaining > 4096) ? 4096 : remaining;
        int    got  = httpd_req_recv(req, (char *)buf, want);
        if (got <= 0)
        {
            break;                    /* 连接断了 */
        }
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got)
        {
            ESP_LOGE(TAG, "short write");
            break;
        }
        written   += (size_t)got;
        remaining -= (size_t)got;
    }

    free(buf);
    fclose(f);

    char msg[96];
    if (remaining == 0)
    {
        ESP_LOGI(TAG, "wallpaper saved: %s (%u KB)", path, (unsigned)(written / 1024));
        snprintf(msg, sizeof(msg), "OK  up_%d.jpg  %u KB", slot, (unsigned)(written / 1024));
    }
    else
    {
        snprintf(msg, sizeof(msg), "FAIL  only %u/%u bytes",
                 (unsigned)written, (unsigned)req->content_len);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/** 清空全部壁纸：GET /wpclear（用户要换一组图时，一张张传之前先清） */
static esp_err_t h_clear(httpd_req_t *req)
{
    DIR *d = opendir(WP_DIR);
    if (d == NULL)
    {
        httpd_resp_sendstr(req, "no wallpaper dir");
        return ESP_OK;
    }
    struct dirent *e;
    int    n    = 0;
    char   path[72];
    while ((e = readdir(d)) != NULL)
    {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".jpg") == 0)
        {
            snprintf(path, sizeof(path), WP_DIR "/%s", e->d_name);
            if (unlink(path) == 0)
            {
                n++;
            }
        }
    }
    closedir(d);

    char msg[64];
    snprintf(msg, sizeof(msg), "OK  deleted %d wallpapers", n);
    ESP_LOGI(TAG, "wallpaper cleared: %d", n);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/** 列出壁纸：GET /wplist → 每行 "up_2.jpg  124 KB"，末尾 "-- N file(s)" */
static esp_err_t h_list(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    DIR *d = opendir(WP_DIR);
    if (d == NULL)
    {
        return httpd_resp_sendstr(req, "no wallpaper dir");
    }

    struct dirent *e;
    char           line[80];
    char           path[96];
    int            n = 0;
    while ((e = readdir(d)) != NULL)
    {
        size_t l = strlen(e->d_name);
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".jpg") == 0)
        {
            snprintf(path, sizeof(path), WP_DIR "/%s", e->d_name);
            struct stat st;
            long        kb = (stat(path, &st) == 0) ? (long)(st.st_size / 1024) : -1;
            snprintf(line, sizeof(line), "%s  %ld KB\n", e->d_name, kb);
            httpd_resp_sendstr_chunk(req, line);
            n++;
        }
    }
    closedir(d);

    if (n == 0)
    {
        httpd_resp_sendstr_chunk(req, "(empty)\n");
    }
    snprintf(line, sizeof(line), "-- %d file(s)\n", n);
    httpd_resp_sendstr_chunk(req, line);
    return httpd_resp_sendstr_chunk(req, NULL);   /* NULL = 结束 chunked 响应 */
}

/** 删除指定的一张壁纸：GET /wpdel?f=up_2.jpg
 *  ★ 只允许删 WP_DIR 下的纯文件名：禁 "/" 与 ".."，
 *    否则 ?f=../../xxx 就能删到别的目录去。 */
static esp_err_t h_del(httpd_req_t *req)
{
    char query[80];
    char fname[48] = {0};

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (httpd_req_get_url_query_len(req) <= 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "f", fname, sizeof(fname)) != ESP_OK)
    {
        return httpd_resp_sendstr(req, "ERR usage: /wpdel?f=up_2.jpg");
    }

    if (strstr(fname, "..") || strchr(fname, '/') || strchr(fname, '\\'))
    {
        return httpd_resp_sendstr(req, "ERR bad name");
    }
    size_t l = strlen(fname);
    if (l < 5 || strcasecmp(fname + l - 4, ".jpg") != 0)
    {
        return httpd_resp_sendstr(req, "ERR not a .jpg");
    }

    char path[96];
    snprintf(path, sizeof(path), WP_DIR "/%s", fname);
    if (unlink(path) != 0)
    {
        return httpd_resp_sendstr(req, "ERR unlink failed (not found?)");
    }

    char msg[72];
    snprintf(msg, sizeof(msg), "OK  deleted %s", fname);
    ESP_LOGI(TAG, "wallpaper deleted: %s", fname);
    return httpd_resp_sendstr(req, msg);
}

/** 下载最近一次录音（调试用）：GET /voicedbg → /sdcard/voice_last.wav
 *  用来把真实录到的音频拷到 PC 分析（波形/频谱/过零率），
 *  区分"麦克风/采样率问题"和"模型识别问题"。 */
static esp_err_t h_voicedbg(httpd_req_t *req)
{
    FILE *f = fopen("/sdcard/voice_last.wav", "rb");
    if (f == NULL)
    {
        httpd_resp_sendstr(req, "no dump");
        return ESP_OK;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=voice_last.wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buf[2048];
    long sent = 0;
    while (sent < sz)
    {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
        {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
            break;
        }
        sent += (long)n;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "voice dump sent: %ld bytes", sz);
    return ESP_OK;
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
    /* 语音服务：asr_url 空 = 走 PC 中转（key 不出 PC） */
    if (form_get(body, "asr_url", tmp, sizeof(tmp)))
    {
        strlcpy(c->asr_url, tmp, sizeof(c->asr_url));
    }
    if (form_get(body, "asr_key", tmp, sizeof(tmp)))
    {
        strlcpy(c->asr_key, tmp, sizeof(c->asr_key));
    }
    if (form_get(body, "vad_thresh", tmp, sizeof(tmp)))
    {
        int v = atoi(tmp);
        c->vad_thresh = (uint16_t)((v < 50) ? 50 : (v > 20000 ? 20000 : v));
    }
    if (form_get(body, "rec_max_sec", tmp, sizeof(tmp)))
    {
        int r = atoi(tmp);
        c->rec_max_sec = (uint8_t)((r < 1) ? 1 : (r > 30 ? 30 : r));
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

    /* ★★★ 模式选择不是随便挑的（2026-11-22 实测踩到）：
     *
     *   APSTA 模式下 AP 与 STA **共用一个 radio，且必须同信道**。当 STA 连不上
     *   （配网场景下最常见：密码填错了）它会在 1~13 信道反复扫描，
     *   **AP 的 beacon 被反复打断** ⇒ 手机/电脑根本扫不到 SmartPanel-XXXX
     *   （实测：日志显示 "AP xxx up"，但 netsh 扫描十个网络里没有它）。
     *
     *   ⇒ 已联网时用 APSTA（改配置不断网）；
     *     未联网时用**纯 AP**（没有 STA 抢 radio，广播稳定）。
     *     配网本身就是"连不上才来做"的事，纯 AP 才是常态。 */
    wifi_mode_t mode = wifi_sta_connected() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_LOGW(TAG, "starting AP in %s mode",
             (mode == WIFI_MODE_APSTA) ? "APSTA (sta online, keeps internet)" : "AP-only (sta offline, stable beacon)");
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ★ httpd 独立启动：STA 已联网时也常驻监听（配网页随时可访问），
     *   见 main.c 里 WiFi 就绪后的调用 —— 不用再点 WIFI 按钮。 */
    if (wifi_prov_httpd_start() != ESP_OK)
    {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AP \"%s\" up, open http://%s/", s_ssid, PROV_AP_IP);
    return ESP_OK;
}

esp_err_t wifi_prov_httpd_start(void)
{
    if (s_srv != NULL)
    {
        return ESP_OK;      /* 已启动 */
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
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
    static const httpd_uri_t uri_upload = {
        .uri = "/upload", .method = HTTP_POST, .handler = h_upload, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_clear = {
        .uri = "/wpclear", .method = HTTP_GET, .handler = h_clear, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_list = {
        .uri = "/wplist", .method = HTTP_GET, .handler = h_list, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_del = {
        .uri = "/wpdel", .method = HTTP_GET, .handler = h_del, .user_ctx = NULL,
    };
    static const httpd_uri_t uri_vdbg = {
        .uri = "/voicedbg", .method = HTTP_GET, .handler = h_voicedbg, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_srv, &uri_root);
    httpd_register_uri_handler(s_srv, &uri_save);
    httpd_register_uri_handler(s_srv, &uri_upload);
    httpd_register_uri_handler(s_srv, &uri_clear);
    httpd_register_uri_handler(s_srv, &uri_list);
    httpd_register_uri_handler(s_srv, &uri_del);
    httpd_register_uri_handler(s_srv, &uri_vdbg);

    ESP_LOGI(TAG, "config page ready (STA 也可访问)");
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

#include "route_card.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 导航后端客户端：一次 GET，解析短页文案，填进 RouteCard。
// 明文 HTTP，不走 TLS —— 后端在局域网内，省掉 ~32 KB 握手内存。
namespace {

constexpr const char* TAG = "WS397.route";
constexpr int kTimeoutMs = 6000;  // 契约要求后端 <6s 返回；RouteFetch 整体不超 8s
constexpr int kFallbackTimeoutMs = 1500;  // 备用地址只为兜"网段变了"，连不上是立即失败
constexpr int kBodyCap = 2048;    // 只回短页文案，实测样例 226 B
constexpr int kUrlCap = 256;       // 后端 URL（含会话 ID）上限
constexpr int kSessionCap = 80;    // 与平台会话 ID 字段上限一致
constexpr int kTaskStack = 8192;  // 独立任务：语音任务仅余 452 B 栈，不能借用
constexpr UBaseType_t kTaskPrio = 4;
#if WS_ROUTE_HTTPS_PROBE
constexpr const char* kHttpsProbeUrls[] = {
    "https://www.baidu.com/robots.txt",
    "https://api.bilibili.com/x/web-interface/nav",
};
#endif

char s_body[kBodyCap];  // 静态缓冲，不占任务栈
char s_url[kUrlCap];
char s_session_id[kSessionCap];
int s_len = 0;
RouteCard* s_card = nullptr;
volatile bool s_busy = false;

void Copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = 0;
        return;
    }
    snprintf(dst, cap, "%s", src);
}

// 旧服务端没有 card_id 时，用响应正文指纹判断卡片是否变化。source 包含查询
// 时间，同一条路线重新查询也会得到新指纹，正好满足“新卡”语义。
uint32_t Fingerprint(const char* data, int len) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

esp_err_t OnEvent(esp_http_client_event_t* ev) {
    if (ev->event_id != HTTP_EVENT_ON_DATA || !ev->data || ev->data_len <= 0) return ESP_OK;
    const int room = kBodyCap - 1 - s_len;
    const int take = ev->data_len < room ? ev->data_len : room;
    if (take > 0) {
        memcpy(s_body + s_len, ev->data, take);
        s_len += take;
    }
    return ESP_OK;
}

esp_err_t HttpGet(const char* url, int timeout_ms) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.disable_auto_redirect = true;
    cfg.event_handler = OnEvent;
    // 公网 HTTPS 必须验证服务端证书。固件已启用公共 CA bundle；不接这里会在
    // 还没握手前因“无证书校验方式”失败，不能拿该结果判断 RAM 是否足够。
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    const int64_t started_us = esp_timer_get_time();
    const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const time_t now = time(nullptr);
    struct tm utc = {};
    gmtime_r(&now, &utc);
    ESP_LOGI(TAG, "https: begin url=%s utc=%04d-%02d-%02dT%02d:%02d:%02dZ heap=%u",
             url, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
             static_cast<unsigned>(heap_before));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    s_len = 0;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    const size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const int elapsed_ms = static_cast<int>((esp_timer_get_time() - started_us) / 1000);
    ESP_LOGI(TAG, "https: end err=%s status=%d bytes=%d elapsed_ms=%d heap_delta=%d heap_now=%u",
             esp_err_to_name(err), status, s_len, elapsed_ms,
             static_cast<int>(heap_after) - static_cast<int>(heap_before), static_cast<unsigned>(heap_after));
    if (err != ESP_OK) return err;
    if (status != 200) return ESP_ERR_INVALID_RESPONSE;
    if (s_len <= 0) return ESP_ERR_INVALID_SIZE;
    s_body[s_len] = 0;
    return ESP_OK;
}

// 字段缺失一律按失败处理，不猜内容：宁可显示"没有查到路线"，也不编一条假路线。
void Parse(RouteCard* card, const char* body, int len) {
    card->version = 0;
    card->card_id = 0;
    card->built_at[0] = 0;
    cJSON* root = cJSON_ParseWithLength(body, len);
    if (!root) {
        Copy(card->title, sizeof card->title, "路线查询失败");
        Copy(card->source, sizeof card->source, "响应格式错误");
        card->count = 0;
        card->state = RouteCard::State::kFailed;
        return;
    }

    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
        Copy(card->title, sizeof card->title, "路线查询失败");
        Copy(card->source, sizeof card->source,
             cJSON_IsString(message) ? message->valuestring : "网络不好，请再试");
        card->count = 0;
        card->state = RouteCard::State::kFailed;
        cJSON_Delete(root);
        return;
    }

    const cJSON* title = cJSON_GetObjectItemCaseSensitive(root, "title");
    Copy(card->title, sizeof card->title, cJSON_IsString(title) ? title->valuestring : "去医院");

    int count = 0;
    const cJSON* lines = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (cJSON_IsArray(lines)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, lines) {
            if (count >= RouteCard::kMaxLines) break;
            if (!cJSON_IsString(item)) continue;
            Copy(card->lines[count], sizeof card->lines[0], item->valuestring);
            ++count;
        }
    }
    card->count = count;

    const cJSON* source = cJSON_GetObjectItemCaseSensitive(root, "source");
    Copy(card->source, sizeof card->source, cJSON_IsString(source) ? source->valuestring : "");

    const cJSON* card_id = cJSON_GetObjectItemCaseSensitive(root, "card_id");
    const cJSON* built_at = cJSON_GetObjectItemCaseSensitive(root, "built_at");
    if (cJSON_IsNumber(card_id) && card_id->valueint > 0) card->card_id = card_id->valueint;
    if (cJSON_IsString(built_at)) Copy(card->built_at, sizeof card->built_at, built_at->valuestring);
    // card_id 是服务端权威版本；旧服务端不返回时退化为整段响应的指纹。
    card->version = card->card_id > 0 ? static_cast<uint32_t>(card->card_id) : Fingerprint(body, len);

    if (!count) {
        Copy(card->source, sizeof card->source, "没有查到路线");
        card->state = RouteCard::State::kFailed;
    } else {
        card->state = RouteCard::State::kOk;  // 最后写 state：读侧看到 kOk 时字段已就绪
    }
    cJSON_Delete(root);
}

const char* SessionUrl(const char* session_id) {
    const char* format = WS_ROUTE_SESSION_ENDPOINT;
    if (!session_id || !*session_id || !format || !format[0]) return nullptr;
    const int written = snprintf(s_url, sizeof s_url, format, session_id);
    return written > 0 && written < static_cast<int>(sizeof s_url) ? s_url : nullptr;
}

#if WS_ROUTE_HTTPS_PROBE
void RunHttpsProbe(RouteCard* card) {
    bool all_ok = true;
    card->count = 0;
    Copy(card->title, sizeof card->title, "HTTPS验证完成");
    for (const char* url : kHttpsProbeUrls) {
        const esp_err_t err = HttpGet(url, kTimeoutMs);
        const char* label = strstr(url, "baidu") ? "百度" : "B站";
        char line[112] = {};
        snprintf(line, sizeof line, "%s TLS：%s", label, err == ESP_OK ? "通过" : "失败");
        Copy(card->lines[card->count++], sizeof card->lines[0], line);
        if (err != ESP_OK) all_ok = false;
    }
    Copy(card->source, sizeof card->source, all_ok ? "看串口：时间/耗时/内存" : "失败详情见串口日志");
    if (!all_ok) Copy(card->title, sizeof card->title, "HTTPS验证失败");
    card->state = all_ok ? RouteCard::State::kOk : RouteCard::State::kFailed;
    card->updated_us = esp_timer_get_time();
}
#endif

void TaskMain(void*) {
    RouteFetch(s_card, s_session_id[0] ? s_session_id : nullptr);
    s_card = nullptr;
    s_session_id[0] = 0;
    s_busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

void RouteFetch(RouteCard* card, const char* session_id) {
    if (!card) return;
#if WS_ROUTE_HTTPS_PROBE
    ESP_LOGW(TAG, "https probe enabled; skipping route-card request");
    RunHttpsProbe(card);
    return;
#endif
    card->state = RouteCard::State::kLoading;
    card->count = 0;
    card->version = 0;
    card->card_id = 0;
    card->built_at[0] = 0;

    // 有会话 ID 且模板已配置时，优先取当前会话的最新路线卡；否则保留旧演示链路。
    const char* session_url = SessionUrl(session_id);
    const char* const candidates[] = {
        session_url ? session_url : WS_ROUTE_ENDPOINT,
        session_url ? WS_ROUTE_SESSION_FALLBACKS : WS_ROUTE_FALLBACKS,
    };
    const int candidate_count = session_url ? 2 : 2;
    esp_err_t err = ESP_FAIL;
    const char* hit = nullptr;
    for (int i = 0; i < candidate_count; ++i) {
        if (!candidates[i] || !candidates[i][0]) continue;
        err = HttpGet(candidates[i], i == 0 ? kTimeoutMs : kFallbackTimeoutMs);
        if (err == ESP_OK) {
            hit = candidates[i];
            break;
        }
        ESP_LOGW(TAG, "route: %s -> %s", candidates[i], esp_err_to_name(err));
    }

    card->error = err;
    if (err != ESP_OK) {
        Copy(card->title, sizeof card->title, "路线查询失败");
        Copy(card->source, sizeof card->source, "网络不好，请再试");
        card->state = RouteCard::State::kFailed;
        ESP_LOGW(TAG, "all endpoints failed");
        return;
    }
    ESP_LOGI(TAG, "route: hit %s%s", hit, session_url ? " (session)" : "");

    Parse(card, s_body, s_len);
    card->updated_us = esp_timer_get_time();
    ESP_LOGI(TAG, "card state=%d lines=%d title='%s' source='%s'", static_cast<int>(card->state), card->count,
             card->title, card->source);
}

bool RouteStart(RouteCard* card, const char* session_id) {
    if (!card) return false;
    if (s_busy) {
        ESP_LOGW(TAG, "request already running");
        return false;
    }
    s_busy = true;
    s_card = card;
    snprintf(s_session_id, sizeof s_session_id, "%s", session_id ? session_id : "");
    if (xTaskCreate(TaskMain, "route", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        s_busy = false;
        s_card = nullptr;
        s_session_id[0] = 0;
        Copy(card->title, sizeof card->title, "路线查询失败");
        Copy(card->source, sizeof card->source, "内存不足");
        card->state = RouteCard::State::kFailed;
        return false;
    }
    return true;
}

const char* RouteEndpoint() {
    return WS_ROUTE_SESSION_ENDPOINT[0] ? WS_ROUTE_SESSION_ENDPOINT : WS_ROUTE_ENDPOINT;
}

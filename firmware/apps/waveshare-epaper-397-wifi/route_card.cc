#include "route_card.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
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
constexpr int kTaskStack = 8192;  // 独立任务：语音任务仅余 452 B 栈，不能借用
constexpr UBaseType_t kTaskPrio = 4;

char s_body[kBodyCap];  // 静态缓冲，不占任务栈
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

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    s_len = 0;
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (status != 200) return ESP_ERR_INVALID_RESPONSE;
    if (s_len <= 0) return ESP_ERR_INVALID_SIZE;
    s_body[s_len] = 0;
    return ESP_OK;
}

// 字段缺失一律按失败处理，不猜内容：宁可显示"没有查到路线"，也不编一条假路线。
void Parse(RouteCard* card, const char* body, int len) {
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
    if (!count) {
        Copy(card->source, sizeof card->source, "没有查到路线");
        card->state = RouteCard::State::kFailed;
    } else {
        card->state = RouteCard::State::kOk;  // 最后写 state：读侧看到 kOk 时字段已就绪
    }
    cJSON_Delete(root);
}

void TaskMain(void*) {
    RouteFetch(s_card);
    s_card = nullptr;
    s_busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

void RouteFetch(RouteCard* card) {
    if (!card) return;
    card->state = RouteCard::State::kLoading;
    card->count = 0;

    // 主地址给足 6 秒（后端要在腾讯超时后回缓存，需要时间）；
    // 备用地址只在"网段变了"时才有用，连不上是立刻失败的，1.5 秒够。
    const char* const candidates[] = {WS_ROUTE_ENDPOINT, WS_ROUTE_FALLBACKS};
    esp_err_t err = ESP_FAIL;
    const char* hit = nullptr;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
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
    ESP_LOGI(TAG, "route: hit %s", hit);

    Parse(card, s_body, s_len);
    card->updated_us = esp_timer_get_time();
    ESP_LOGI(TAG, "card state=%d lines=%d title='%s' source='%s'", static_cast<int>(card->state), card->count,
             card->title, card->source);
}

void RouteStart(RouteCard* card) {
    if (!card) return;
    if (s_busy) {
        ESP_LOGW(TAG, "request already running");
        return;
    }
    s_busy = true;
    s_card = card;
    if (xTaskCreate(TaskMain, "route", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        s_busy = false;
        s_card = nullptr;
        Copy(card->title, sizeof card->title, "路线查询失败");
        Copy(card->source, sizeof card->source, "内存不足");
        card->state = RouteCard::State::kFailed;
    }
}

const char* RouteEndpoint() { return WS_ROUTE_ENDPOINT; }

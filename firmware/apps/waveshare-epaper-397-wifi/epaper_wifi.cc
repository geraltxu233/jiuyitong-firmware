#include "application.h"
#include "config.h"
#include "epaper_hardware.h"
#include "epaper_wifi_audio.h"
#include "epaper_wifi_ui.h"
#include "epaper_controls.h"
#include "agent_link_platform.h"
#include "dept_table.h"
#include "route_card.h"

#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG = "WS397.wifi";
// 绘制层有 11 个物理行槽（ui.cc 的 kBodyRows）。35 px 正文一条逻辑行能放 12 个汉字，
// 实测 7 条文案全部单行放下；留 2 个槽给偶发折行，所以一页排 9 条逻辑行。
constexpr int kRouteRowsPerPage = 9;
constexpr int kDeptRowsPerPage = 9;
enum class Page { kHome, kMenu, kSettings, kSessions, kReset, kRoute, kDeptList, kDeptDetail, kDeptHint };
enum class Action { kUp, kDown, kCenter, kMenu, kBack };
struct Input { Action action; uint32_t view; };
}

class WaveshareEpaperWifi : public Application {
public:
    WaveshareEpaperWifi() {
        ESP_ERROR_CHECK(nvs_flash_init());
        Load();
        const esp_err_t hardware = hardware_.Init(volume_);
        hardware_ok_ = hardware == ESP_OK;
        if (hardware != ESP_OK) ESP_LOGE(TAG, "Hardware init: %s", esp_err_to_name(hardware));
        if (audio_.Init(&hardware_) != ESP_OK) ESP_LOGE(TAG, "Audio unavailable");
        screen_ok_ = ui_.Start(&hardware_) == ESP_OK;
        events_ = xQueueCreate(8, sizeof(Input));
        ESP_ERROR_CHECK(events_ ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(ControlEntry, "ep397_control", 12288, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
        ESP_ERROR_CHECK(xTaskCreate(KeyEntry, "ep397_keys", 3072, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
    const char* Name() const override { return "WavePaper397"; }
    const char* Model() const override { return EPAPER_MODEL; }
    const agent_platform_t* Platform() const override {
        static const agent_platform_t platform = {
            .base_url = CLOUD_BASE_URL, .product_id = CLOUD_PRODUCT_ID, .chip_type = CLOUD_CHIP_TYPE,
        };
        return CLOUD_PRODUCT_ID > 0 ? &platform : nullptr;
    }
    uint32_t Capabilities() const override {
        uint32_t caps = AGENT_CAP_BUTTON;
        if (screen_ok_) caps |= AGENT_CAP_SCREEN;
        if (audio_.Ready()) caps |= AGENT_CAP_MIC | AGENT_CAP_SPEAKER;
        if (hardware_ok_) caps |= AGENT_CAP_BATTERY;
        return caps;
    }
    void OnLinkStatus(const agent_link_status_t& status) override { ui_.Status(status); }
    // 实时语音通道下发的文本（transcript.partial / transcript.final 都走这里）。
    // 打一条日志是为了确认：里面到底是"用户说的话"还是"AI 的回复"——
    // 这决定了"AI 决定后自动导航"这条路是否成立。
    void ShowText(const char* text) override {
        ESP_LOGI(TAG, "voice text: %s", text ? text : "(null)");
        RememberSpeech(text);
        ui_.Answer(text);
    }
    void PlayAudio(const uint8_t* pcm, size_t bytes) override { audio_.Play(pcm, bytes); }
    void AudioEnd() override { audio_.End(); ui_.EndAnswer(); }
    void AudioReset() override { audio_.Reset(); }
    void OnListen(bool start, uint32_t max_ms) override { audio_.Listen(start, max_ms); }
    int GetBatteryLevel() override { return battery_.load(); }
    bool IsCharging() override { return charging_.load(); }

private:
    void Load() {
        nvs_handle_t handle;
        if (nvs_open("ws397_wifi", NVS_READONLY, &handle) != ESP_OK) return;
        (void)nvs_get_i32(handle, "agent", &saved_agent_);
        size_t size = sizeof session_; (void)nvs_get_str(handle, "session", session_, &size);
        size = sizeof session_name_; (void)nvs_get_str(handle, "name", session_name_, &size);
        int32_t volume = AUDIO_DEFAULT_VOLUME;
        if (nvs_get_i32(handle, "volume", &volume) == ESP_OK && volume >= 0 && volume <= 100) volume_ = volume;
        nvs_close(handle);
        if (saved_agent_ <= 0) session_[0] = session_name_[0] = 0;
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
        // 直连验收固件：语音与路线卡必须共用 TiDB Link 会话，不能误读 NVS 里的旧平台会话。
        snprintf(session_, sizeof session_, "%s", CONFIG_AGENT_LINK_REALTIME_SESSION_ID);
        snprintf(session_name_, sizeof session_name_, "TiDB Link 语音验收");
        snprintf(agent_name_, sizeof agent_name_, "TiDB Link 直连");
        voice_ = true;
#endif
    }
    void Save() {
        nvs_handle_t handle;
        if (nvs_open("ws397_wifi", NVS_READWRITE, &handle) != ESP_OK) { Message("设置保存失败"); return; }
        esp_err_t error = nvs_set_i32(handle, "agent", saved_agent_);
        if (error == ESP_OK) error = nvs_set_str(handle, "session", session_);
        if (error == ESP_OK) error = nvs_set_str(handle, "name", session_name_);
        if (error == ESP_OK) error = nvs_set_i32(handle, "volume", volume_);
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
        if (error != ESP_OK) Message("设置保存失败");
    }
    void Message(const char* message) { snprintf(message_, sizeof message_, "%s", message); }
    bool Error(esp_err_t error) {
        if (error == ESP_OK) return false;
        snprintf(message_, sizeof message_, "请求失败：%s", esp_err_to_name(error));
        ESP_LOGW(TAG, "%s", message_); return true;
    }
    void Home() { page_ = Page::kHome; cursor_ = 0; ++view_; }
    void EndCall() {
        const bool was_call = call_;
        call_ = active_ = false; requested_ = 0;
        audio_.Stop(); (void)agent_link_end_realtime();
        // 上一轮对话的回复属于"这次通话"，不是首页的一部分。不清掉的话，
        // 按 BOOT 退出通话后首页会同时写着"请说一声你好"和上一轮的整段回答，
        // 老人会以为那是当前要说的话。Ui 那边同时会在回复消失时强制全刷，擦掉残影。
        ui_.Answer("");
        // 通话期间探针发现新卡时，挂断动作本身就是“确认看路线”，直接切页；
        // 仍保留长按菜单作为没有新卡时的兜底入口。
        if (was_call && auto_route_pending_ && page_ == Page::kHome) {
            auto_route_pending_ = false;
            ESP_LOGI(TAG, "auto route: switching after call end");
            RealRoute();
        }
    }
    bool SyncAgent(bool explain = false) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
        (void)explain;
        snprintf(session_, sizeof session_, "%s", CONFIG_AGENT_LINK_REALTIME_SESSION_ID);
        snprintf(session_name_, sizeof session_name_, "TiDB Link 语音验收");
        snprintf(agent_name_, sizeof agent_name_, "TiDB Link 直连");
        voice_ = true;
        return true;
#else
        agent_link_platform_agent_t bound{};
        const esp_err_t error = agent_link_platform_bound_agent(&bound);
        if (error != ESP_OK) {
            // A transient authentication outage must not erase a saved selection.
            if (agent_ && (call_ || page_ != Page::kHome)) {
                ESP_LOGW(TAG, "sync failed (%s) on page %d -> home", esp_err_to_name(error), static_cast<int>(page_));
                EndCall(); Home();
            }
            agent_ = 0; voice_ = false;
            if (explain) Message("请先完成激活，并在控制台绑定智能体");
            return false;
        }
        if (agent_ != bound.id) {
            EndCall(); Home();
            if (saved_agent_ != bound.id) {
                session_[0] = session_name_[0] = 0;
                Message("智能体已变更，请重试");
            }
            agent_ = saved_agent_ = bound.id;
            Save();
        }
        snprintf(agent_name_, sizeof agent_name_, "%s", bound.name);
        voice_ = bound.voice_supported;
        return true;
#endif
    }
    void Busy(const char* title) {
        epaper::Screen screen{}; screen.view = ++view_;
        snprintf(screen.title, sizeof screen.title, "%s", title);
        snprintf(screen.detail, sizeof screen.detail, "请稍候……");
        screen.footer[0] = 0; ui_.Publish(screen);
    }
    void Sessions(size_t offset = 0) {
        EndCall(); if (!SyncAgent(true)) return;
        Busy("正在加载会话");
        const int requested_agent = agent_;
        agent_link_platform_conversation_t fetched[3]{};
        size_t count = 0, total = 0;
        if (Error(agent_link_platform_conversations(requested_agent, offset, fetched, 3, &count, &total))) return;
        if (!count && offset) {
            offset = 0;
            if (Error(agent_link_platform_conversations(requested_agent, offset, fetched, 3, &count, &total))) return;
        }
        if (!SyncAgent(true) || requested_agent != agent_) { Home(); Message("智能体已变更，请重试"); return; }
        memcpy(sessions_, fetched, sizeof sessions_);
        count_ = count; offset_ = offset; more_ = offset + count < total;
        page_ = Page::kSessions; cursor_ = 0; ++view_;
        message_[0] = 0;
    }
    void NewSession() {
        EndCall(); if (!SyncAgent(true)) return;
        Busy("正在新建会话");
        const int requested_agent = agent_;
        char created[sizeof session_]{};
        if (Error(agent_link_platform_create_conversation(requested_agent, created, sizeof created))) return;
        if (!SyncAgent(true) || requested_agent != agent_) { Home(); Message("智能体已变更，请重试"); return; }
        if (!created[0]) { Message("平台未返回会话"); return; }
        snprintf(session_, sizeof session_, "%s", created);
        snprintf(session_name_, sizeof session_name_, "新会话");
        Save(); Home(); Message("会话已就绪，中键通话");
    }
    // H1：写死的样例卡，只用来验证大字排版、字库覆盖和单行裁切，不做任何网络请求。
    // 上屏内容必须自带"样例数据"标注，不能让人误以为是真实公交结果。
    void SampleRoute() {
        static const char* const kLines[] = {
            "出小区东门向东走200米",
            "乘58路，开往人民广场方向",
            "坐6站，中心广场站下车",
            "换乘地铁2号线（B口进站）",
            "坐3站，市一医院站下车",
            "从2号口出，向西步行150米",
            "末班车23:00，雨雪天停运",
        };
        EndCall();
        route_view_ = RouteCard{};
        route_view_.state = RouteCard::State::kOk;
        snprintf(route_view_.title, sizeof route_view_.title, "去市第一人民医院（东院）");
        for (const char* line : kLines) {
            if (route_view_.count >= RouteCard::kMaxLines) break;
            snprintf(route_view_.lines[route_view_.count], sizeof route_view_.lines[0], "%s", line);
            ++route_view_.count;
        }
        // 来源行与按键提示分开：来源是"这数据可不可信"，按键是"怎么操作"，不该挤在一行。
        snprintf(route_view_.source, sizeof route_view_.source, "样例数据 · 非真实公交");
        route_page_ = 0;
        page_ = Page::kRoute; cursor_ = 0; ++view_;
        message_[0] = 0;
        ESP_LOGI(TAG, "route: sample card, %d lines, %d pages", route_view_.count, RoutePages());
    }
    // 真实路线：先挂断语音（省出连接内存），再把请求丢给独立任务，界面立刻切到大字页显示"查询中"。
    // 结果由 route 任务写回 route_view_，ControlLoop 每秒重绘一次就会刷出来。
    void RealRoute() {
        EndCall();
        route_view_ = RouteCard{};
        route_view_.state = RouteCard::State::kLoading;
        snprintf(route_view_.title, sizeof route_view_.title, "正在查路线");
        route_page_ = 0;
        page_ = Page::kRoute; cursor_ = 0; ++view_;
        message_[0] = 0;
        RouteStart(&route_view_, session_[0] ? session_ : nullptr);
        ESP_LOGI(TAG, "route: request started%s", session_[0] ? " for selected session" : " using demo endpoint");
    }
    // 路线卡标题里只有医院，没有科室。这里按四家核准医院的简称匹配静态科室表；
    // 不匹配时明确显示“未收录”，绝不拿中山的楼层去冒充仁济。
    const char* RouteHospitalKey() const {
        const char* title = route_view_.title;
        if (strstr(title, "中山")) return "中山";
        if (strstr(title, "第六") || strstr(title, "市六")) return "市六";
        if (strstr(title, "仁济")) return "仁济";
        if (strstr(title, "肿瘤")) return "肿瘤";
        return "";
    }
    int DeptCountForKey(const char* key) const {
        if (!key || !*key) return 0;
        int count = 0;
        for (int i = 0; i < kDepartmentCount; ++i)
            if (strcmp(kDepartments[i].hospital_key, key) == 0) ++count;
        return count;
    }
    const DeptEntry* DeptAt(const char* key, int ordinal) const {
        if (!key || !*key || ordinal < 0) return nullptr;
        for (int i = 0; i < kDepartmentCount; ++i) {
            if (strcmp(kDepartments[i].hospital_key, key) != 0) continue;
            if (ordinal-- == 0) return &kDepartments[i];
        }
        return nullptr;
    }
    // partial 文本会反复下发同一句的增长版：本次以已有内容开头时直接覆盖，否则追加。
    // 这样既能拼出多段回复，又不会被 partial 重复撑爆缓冲。
    void RememberSpeech(const char* text) {
        if (!text || !*text) return;
        const size_t len = strlen(text);
        const size_t cur = strlen(spoken_);
        if (cur > 0 && len > cur && strncmp(text, spoken_, cur) == 0) {
            snprintf(spoken_, sizeof spoken_, "%s", text);
            return;
        }
        if (len >= sizeof spoken_) {
            snprintf(spoken_, sizeof spoken_, "%s", text + (len - sizeof spoken_ + 1));
            return;
        }
        if (cur + len + 1 > sizeof spoken_) {
            const size_t drop = cur + len + 1 - sizeof spoken_;
            memmove(spoken_, spoken_ + drop, cur - drop + 1);
        }
        const size_t used = strlen(spoken_);
        snprintf(spoken_ + used, sizeof spoken_ - used, "%s", text);
    }

    static const char* LastHit(const char* text, const char* needle) {
        if (!text || !*text || !needle || !*needle) return nullptr;
        const char* last = nullptr;
        for (const char* hit = strstr(text, needle); hit; hit = strstr(hit + 1, needle)) last = hit;
        return last;
    }

    // 人设说的是“消化内科、呼吸内科”，科室表里存的是“肠胃科、呼吸科”，
    // 所以别名必须在这里补齐，否则永远匹配不上。
    const char* DeptLatestMention(const char* name) const {
        struct Alias { const char* name; const char* extra[3]; };
        static const Alias kAlias[] = {
            {"肠胃科", {"消化内科", "消化科", nullptr}},
            {"呼吸科", {"呼吸内科", nullptr, nullptr}},
            {"心内科", {"心血管内科", "心脏内科", nullptr}},
            {"肾内科", {"肾脏内科", nullptr, nullptr}},
            {"普外科", {"普通外科", "普外科", nullptr}},
            {"神经内科", {"神经内科", nullptr, nullptr}},
            {"内分泌科", {"内分泌", nullptr, nullptr}},
            {"骨科", {"骨外科", nullptr, nullptr}},
            {"肿瘤科", {"肿瘤内科", "肿瘤外科", nullptr}},
        };
        const char* latest = LastHit(spoken_, name);
        for (const Alias& alias : kAlias) {
            if (strcmp(alias.name, name) != 0) continue;
            for (const char* extra : alias.extra) {
                if (!extra) break;
                const char* hit = LastHit(spoken_, extra);
                if (hit && (!latest || hit > latest)) latest = hit;
            }
        }
        return latest;
    }

    // 交通页长按：语音已经推荐了科室，就直接打开那个科室，不再让老人上下键翻全院列表。
    // 认不出来（或该院没有这个科）才退回列表，绝不猜一个科显示楼层。
    void DeptForRoute() {
        // 语音说了"分不清、去服务台"时，文中即便出现科名也只是鉴别诊断的可能性，
        // 不能当成推荐科室，否则会和刚播完的医嘱自相矛盾。
        static const char* const kNoDept[] = {"服务台", "分不清", "都有点关系", "都可能", "没有排名"};
        for (const char* word : kNoDept) {
            if (strstr(spoken_, word)) {
                ESP_LOGW(TAG, "dept: speech says no clear department (%s)", word);
                DeptHint();
                return;
            }
        }
        const char* key = RouteHospitalKey();
        int best = -1;
        const char* best_at = nullptr;
        int ordinal = -1;
        for (int i = 0; i < kDepartmentCount; ++i) {
            if (strcmp(kDepartments[i].hospital_key, key) != 0) continue;
            ++ordinal;
            const char* at = DeptLatestMention(kDepartments[i].name);
            if (!at) continue;
            // 取最后被提到的科：Agent 先列备选再给结论时，结论在最后一句。
            if (!best_at || at > best_at) { best_at = at; best = ordinal; }
        }
        if (best < 0) {
            // 语音里没有明确科室（例如"分不清，建议去一楼服务台"）时，绝不能列出全院科室
            // 让老人自己挑：那等于替他分诊，而且会和刚说完的医嘱自相矛盾。
            ESP_LOGW(TAG, "dept: no department mentioned, showing service-desk hint");
            DeptHint();
            return;
        }
        const DeptEntry* entry = DeptAt(key, best);
        dept_cursor_ = dept_selected_ = best;
        page_ = Page::kDeptDetail;
        cursor_ = 0;
        ++view_;
        message_[0] = 0;
        ESP_LOGI(TAG, "dept: direct open hospital=%s name=%s", key, entry ? entry->name : "?");
    }

    void DeptHint() {
        page_ = Page::kDeptHint;
        cursor_ = 0;
        ++view_;
        message_[0] = 0;
    }
    void PublishDeptHint(epaper::Screen& screen) {
        snprintf(screen.title, sizeof screen.title, "院内位置");
        screen.row_scale = 2;
        snprintf(screen.rows[0], sizeof screen.rows[0], "这次没定科室");
        snprintf(screen.rows[1], sizeof screen.rows[1], "请到一楼服务台问");
        snprintf(screen.detail, sizeof screen.detail, "自己猜科室容易走错");
        snprintf(screen.footer, sizeof screen.footer, "中键看科室位置  BOOT返回");
    }

    void DeptList() {
        dept_cursor_ = 0;
        page_ = Page::kDeptList;
        cursor_ = 0;
        ++view_;
        message_[0] = 0;
        ESP_LOGI(TAG, "dept: list opened hospital=%s entries=%d", RouteHospitalKey(),
                 DeptCountForKey(RouteHospitalKey()));
    }
    void PublishDeptList(epaper::Screen& screen) {
        const char* key = RouteHospitalKey();
        const int count = DeptCountForKey(key);
        snprintf(screen.title, sizeof screen.title, "%s科室", key && *key ? key : "该院");
        if (!count) {
            snprintf(screen.rows[0], sizeof screen.rows[0], "该院科室信息未收录");
            snprintf(screen.detail, sizeof screen.detail, "以现场导医信息为准");
            snprintf(screen.footer, sizeof screen.footer, "BOOT返回");
            return;
        }
        if (dept_cursor_ >= count) dept_cursor_ = count - 1;
        const int start = (dept_cursor_ / kDeptRowsPerPage) * kDeptRowsPerPage;
        int row = 0;
        for (int i = start; i < count && row < kDeptRowsPerPage; ++i, ++row) {
            const DeptEntry* dept = DeptAt(key, i);
            if (dept) snprintf(screen.rows[row], sizeof screen.rows[row], "%d. %s", i + 1, dept->name);
        }
        screen.selected = dept_cursor_ - start;
        snprintf(screen.detail, sizeof screen.detail, "第 %d-%d 项，共 %d 项", start + 1, start + row, count);
        snprintf(screen.footer, sizeof screen.footer, "上下键选择  中键查看  BOOT返回");
    }
    void PublishDept(epaper::Screen& screen) {
        const char* key = RouteHospitalKey();
        const DeptEntry* dept = DeptAt(key, dept_selected_);
        if (!dept) {
            snprintf(screen.title, sizeof screen.title, "科室信息");
            snprintf(screen.rows[0], sizeof screen.rows[0], "该院科室信息未收录");
            snprintf(screen.footer, sizeof screen.footer, "BOOT返回");
            return;
        }
        snprintf(screen.title, sizeof screen.title, "%s", dept->name);
        screen.row_scale = 2;
        for (int i = 0; i < 5; ++i)
            if (dept->lines[i]) snprintf(screen.rows[i], sizeof screen.rows[i], "%s", dept->lines[i]);
        snprintf(screen.detail, sizeof screen.detail, "%s", dept->hours);
        snprintf(screen.footer, sizeof screen.footer, "以现场为准  BOOT返回");
    }

    // 通话期间只做“版本探针”，不显示卡片；用户挂断时若探针发现新版本，
    // 直接进入路线页。这样不需要长按菜单和短按确认，也不会在语音播报中切页。
    void StartAutoRoute() {
        auto_card_ = RouteCard{};
        auto_baseline_version_ = 0;
        auto_baseline_ready_ = false;
        auto_route_pending_ = false;
        auto_next_probe_us_ = 0;
    }
    void ObserveAutoRoute() {
        if (!auto_baseline_ready_) {
            auto_baseline_version_ = auto_card_.state == RouteCard::State::kOk ? auto_card_.version : 0;
            auto_baseline_ready_ = true;
            ESP_LOGI(TAG, "auto route baseline version=%u", static_cast<unsigned>(auto_baseline_version_));
            return;
        }
        if (auto_card_.version && auto_card_.version != auto_baseline_version_) {
            auto_route_pending_ = true;
            ESP_LOGI(TAG, "auto route new version=%u (baseline=%u)",
                     static_cast<unsigned>(auto_card_.version), static_cast<unsigned>(auto_baseline_version_));
        }
    }
    void PollAutoRoute() {
        if (!call_ || page_ != Page::kHome || auto_route_pending_) return;
        if (auto_card_.state == RouteCard::State::kOk) {
            ObserveAutoRoute();
            auto_card_.state = RouteCard::State::kIdle;
            if (auto_route_pending_) return;
        } else if (auto_card_.state == RouteCard::State::kFailed) {
            // HTTP 成功但服务端明确没有卡（code=3）可以建立 0 基线；
            // 网络失败不建立基线，避免把下一次恢复误当成新路线。
            if (auto_card_.error == ESP_OK && !auto_baseline_ready_) {
                ObserveAutoRoute();
            }
            auto_card_.state = RouteCard::State::kIdle;
        } else if (auto_card_.state == RouteCard::State::kLoading) {
            return;
        }

        const int64_t now = esp_timer_get_time();
        if (now < auto_next_probe_us_) return;
        auto_card_.state = RouteCard::State::kLoading;
        if (RouteStart(&auto_card_, session_[0] ? session_ : nullptr)) {
            auto_next_probe_us_ = now + 2000000;  // 2 秒一次，足够快但不会刷爆接口
        } else {
            auto_card_.state = RouteCard::State::kIdle;
            auto_next_probe_us_ = now + 1000000;
        }
    }
    int RoutePages() const {
        if (route_view_.count <= 0) return 1;
        return (route_view_.count + kRouteRowsPerPage - 1) / kRouteRowsPerPage;
    }
    void PublishRoute(epaper::Screen& screen) {        snprintf(screen.title, sizeof screen.title, "%s", route_view_.title);
        screen.row_scale = 2;
        screen.provisional = route_view_.state == RouteCard::State::kLoading;
        // 失败时 source 里放的是可直接显示的原因（见 route_card.h），绝不拿假路线凑数。
        if (route_view_.state == RouteCard::State::kFailed) {
            snprintf(screen.detail, sizeof screen.detail, "%s",
                     route_view_.source[0] ? route_view_.source : "网络不好，请再试");
            snprintf(screen.footer, sizeof screen.footer, "中键重试  BOOT返回");
            return;
        }
        if (route_view_.state == RouteCard::State::kLoading) {
            snprintf(screen.detail, sizeof screen.detail, "正在查询公交，请稍候");
            snprintf(screen.footer, sizeof screen.footer, "BOOT返回");
            return;
        }
        const int first = route_page_ * kRouteRowsPerPage;
        for (int i = 0; i < kRouteRowsPerPage; ++i) {
            const int index = first + i;
            if (index >= route_view_.count) break;
            snprintf(screen.rows[i], sizeof screen.rows[i], "%s", route_view_.lines[index]);
        }
        snprintf(screen.detail, sizeof screen.detail, "%s", route_view_.source);
        const int pages = RoutePages();
        // 页脚 28 px 一行只有 440 px 可用（约 15 个汉字），提示语按实测宽度裁剪：
        // 多页时保留"翻页 + 返回"，重查退到第二优先——它只在联网失败时才是主操作。
        if (pages > 1)
            snprintf(screen.footer, sizeof screen.footer, "%d/%d 页  上下键翻页  BOOT返回",
                     route_page_ + 1, pages);
        else
            snprintf(screen.footer, sizeof screen.footer, "中键刷新  长按科室  BOOT返回");
    }
    void Call() {
        if (call_) {
            EndCall();
            if (page_ != Page::kRoute) Message("长按中键，再选去医院");
            return;
        }
        if (!SyncAgent(true)) return;
        if (!session_[0]) { Sessions(); return; }
        if (!voice_) { Message("该智能体不支持语音"); return; }
        if (!audio_.Ready()) { Message("音频不可用，请重启"); return; }
        const agent_state_t state = agent_link_state();
        if (state != AGENT_STATE_CONNECTED && state != AGENT_STATE_READY) { Message("正在等待平台连接"); return; }
        ui_.Answer("");
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
        const esp_err_t request = agent_link_request_realtime();
#else
        const esp_err_t request = agent_link_request_realtime_session(agent_, session_);
#endif
        if (!Error(request)) {
            call_ = true; active_ = false; requested_ = esp_timer_get_time();
            // 每次通话独立累积：上一轮推荐的科室不能串到这一轮。
            spoken_[0] = 0;
            StartAutoRoute();
            Message("接通中，中键挂断");
        }
    }
    void Choose() {
        if (page_ == Page::kHome) { Call(); return; }
        if (page_ == Page::kMenu) {
            if (cursor_ == 0) RealRoute();
#if !CONFIG_AGENT_LINK_REALTIME_DIRECT
            else if (cursor_ == 1) { page_ = Page::kSettings; cursor_ = 0; ++view_; }
#endif
        } else if (page_ == Page::kDeptHint) {
            // 提示页只是"别自己猜"，老人自己知道科名时仍可进列表查位置。
            DeptList();
        } else if (page_ == Page::kDeptList) {
            if (DeptCountForKey(RouteHospitalKey()) > 0) {
                dept_selected_ = dept_cursor_;
                page_ = Page::kDeptDetail;
                ++view_;
                ESP_LOGI(TAG, "dept: open %d", dept_selected_);
            }
        } else if (page_ == Page::kDeptDetail) {
            // 详情页只认 BOOT 返回，中键不重复进入。
        } else if (page_ == Page::kSettings) {
#if !CONFIG_AGENT_LINK_REALTIME_DIRECT
            if (cursor_ == 0) SampleRoute();
            else if (cursor_ == 1) Sessions();
            else if (cursor_ == 2) NewSession();
            else if (cursor_ == 3) { page_ = Page::kReset; cursor_ = 0; ++view_; }
#else
            Home();
#endif
        } else if (page_ == Page::kReset) {
            if (cursor_ == 1) {
                EndCall();
                if (!Error(agent_link_forget())) {
                    session_[0] = session_name_[0] = 0; agent_ = saved_agent_ = 0;
                    Save(); Home(); Message("请先在控制台解绑，WiFi 会保留");
                }
            } else Home();
        } else {
            if (!SyncAgent(true) || page_ != Page::kSessions) return;
            if (cursor_ < count_) {
                snprintf(session_, sizeof session_, "%s", sessions_[cursor_].id);
                snprintf(session_name_, sizeof session_name_, "%s", sessions_[cursor_].name);
                Save(); Home(); Message("已选择会话，中键通话");
            } else {
                size_t row = cursor_ - count_;
                if (more_ && row-- == 0) { Sessions(offset_ + count_); return; }
                if (offset_ && row-- == 0) { Sessions(offset_ >= 3 ? offset_ - 3 : 0); return; }
                Home();
            }
        }
    }
    size_t Rows() const {
        if (page_ == Page::kMenu) {
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            return 1;
#else
            return 2;
#endif
        }
        if (page_ == Page::kSettings) return 4;
        if (page_ == Page::kReset) return 2;
        if (page_ == Page::kDeptList) {
            const int count = DeptCountForKey(RouteHospitalKey());
            return count > 0 ? static_cast<size_t>(count) : 1;
        }
        if (page_ == Page::kDeptDetail) return 1;
        if (page_ == Page::kDeptHint) return 1;
        return count_ + (more_ ? 1 : 0) + (offset_ ? 1 : 0) + 1;
    }
    void Publish() {
        epaper::Screen screen{}; screen.view = view_;
        if (page_ == Page::kRoute) { PublishRoute(screen); ui_.Publish(screen); return; }
        if (page_ == Page::kDeptList) { PublishDeptList(screen); ui_.Publish(screen); return; }
        if (page_ == Page::kDeptDetail) { PublishDept(screen); ui_.Publish(screen); return; }
        if (page_ == Page::kDeptHint) { PublishDeptHint(screen); ui_.Publish(screen); return; }
        if (CLOUD_PRODUCT_ID <= 0) {
            snprintf(screen.title, sizeof screen.title, "未配置平台产品 ID");
            snprintf(screen.detail, sizeof screen.detail, "请在 products.json 注册本机型后重编");
        } else {
            snprintf(screen.title, sizeof screen.title, "%s", page_ == Page::kMenu ? "就医通" :
                     page_ == Page::kSettings ? "设备设置" : page_ == Page::kSessions ? "选择会话" :
                     page_ == Page::kReset ? "重置激活？" :
                     active_ ? "请说话" : call_ ? "正在接通" : "就医通");
            if (page_ == Page::kReset) snprintf(screen.detail, sizeof screen.detail, "WiFi 保留，先在控制台解绑");
            else if (message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", message_);
            else if (page_ == Page::kHome) snprintf(screen.detail, sizeof screen.detail, "按中键开始，说哪里不舒服");
            else snprintf(screen.detail, sizeof screen.detail, "%s·%s", agent_name_[0] ? agent_name_ : "控制台未绑定",
                          session_name_[0] ? session_name_ : session_[0] ? "已选会话" : "未选会话");
        }
        if (page_ != Page::kHome) {
            screen.selected = static_cast<int>(cursor_);
            snprintf(screen.footer, sizeof screen.footer, "上下键选择  中键确认  BOOT返回");
            if (page_ == Page::kMenu) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "去医院（查看路线）");
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
                snprintf(screen.footer, sizeof screen.footer, "中键查看路线  BOOT返回");
#else
                snprintf(screen.rows[1], sizeof screen.rows[1], "设备设置");
            } else if (page_ == Page::kSettings) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "样例路线卡（离线自检）");
                snprintf(screen.rows[1], sizeof screen.rows[1], "选择已有会话");
                snprintf(screen.rows[2], sizeof screen.rows[2], "新建会话");
                snprintf(screen.rows[3], sizeof screen.rows[3], "重置平台激活");
#endif
            } else if (page_ == Page::kReset) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "取消");
                snprintf(screen.rows[1], sizeof screen.rows[1], "确认重置激活");
            } else {
                size_t row = 0;
                for (; row < count_; ++row)
                    snprintf(screen.rows[row], sizeof screen.rows[row], "%u. %s", static_cast<unsigned>(offset_ + row + 1),
                             sessions_[row].name[0] ? sessions_[row].name : "会话");
                if (more_) snprintf(screen.rows[row++], sizeof screen.rows[0], "下一页");
                if (offset_) snprintf(screen.rows[row++], sizeof screen.rows[0], "上一页");
                snprintf(screen.rows[row], sizeof screen.rows[row], "返回");
                // 列表页不画 detail，所以"没有会话"只能写进页脚，否则这屏只剩一个"返回"，
                // 老人不知道是没有会话还是加载失败。
                if (!count_) snprintf(screen.footer, sizeof screen.footer, "还没有会话　长按中键新建");
            }
        } else {
            // 页脚 28 px 一行只有 440 px。实测"中键说话　长按菜单　上下键音量 60%"要 484 px，
            // 尾部会被折到屏外，所以缩成 394 px 的写法：保留音量数值，去掉"上下键"三字。
#if CONFIG_AGENT_LINK_REALTIME_DIRECT
            snprintf(screen.footer, sizeof screen.footer, "中键说话　长按路线　音量%d%%", volume_);
#else
            snprintf(screen.footer, sizeof screen.footer, "中键说话　长按菜单　音量%d%%", volume_);
#endif
        }
        ui_.Publish(screen);
    }
    void ControlLoop() {
        int64_t poll = 0, volume_at = 0;
        bool volume_dirty = false;
        while (true) {
            Input input;
            if (xQueueReceive(events_, &input, pdMS_TO_TICKS(50)) == pdTRUE) {
                // Ending a call and changing volume remain usable while e-paper is refreshing.
                if (input.action == Action::kBack) {
                    // BOOT 逐级返回。通话中必须随时能挂断；页面导航仍等当前刷屏完成，防止连跳。
                    const bool stale = !call_ && !epaper::Current(input.view, view_);
                    ESP_LOGI(TAG, "key: BOOT page=%d call=%d stale=%d", static_cast<int>(page_),
                             call_ ? 1 : 0, stale ? 1 : 0);
                    if (stale) {
                        // 丢弃：这一次按键属于上一屏，不处理
                    } else if (page_ == Page::kDeptDetail) {
                        page_ = Page::kDeptList; ++view_;
                    } else if (page_ == Page::kDeptList) {
                        page_ = Page::kRoute; ++view_;
                    } else if (page_ == Page::kDeptHint) {
                        page_ = Page::kRoute; ++view_;
                    } else if (page_ == Page::kSettings) {
                        page_ = Page::kMenu; cursor_ = 0; ++view_;
                    } else if (page_ == Page::kSessions || page_ == Page::kReset) {
                        page_ = Page::kSettings; cursor_ = 0; ++view_;
                    } else {
                        const bool was_calling = call_;
                        EndCall();
                        // BOOT 的语义始终是“回到首页”。EndCall 可能因为自动切页
                        // 把 page_ 改成 kRoute，这里必须再 Home 一次，否则交通页
                        // 会吞掉 BOOT，用户看起来像按键失效。
                        Home();
                        if (was_calling) Message("已挂断");
                        else message_[0] = 0;
                    }
                } else if (page_ == Page::kHome && (input.action == Action::kUp || input.action == Action::kDown)) {
                    int next = epaper::Volume(volume_, input.action == Action::kUp ? 1 : -1);
                    if (hardware_.AudioReady() && !Error(hardware_.codec.SetVolume(next))) {
                        volume_ = next; volume_dirty = true; volume_at = esp_timer_get_time();
                    }
                } else if (page_ == Page::kHome && call_ && input.action == Action::kCenter) {
                    EndCall();
                    if (page_ != Page::kRoute) Message("长按中键，再选去医院");
                } else if (epaper::Current(input.view, view_)) {
                    message_[0] = 0; ++view_;
                    if (input.action == Action::kMenu) {
                        if (page_ == Page::kRoute) {
                            DeptForRoute();
                        } else if (page_ == Page::kDeptList || page_ == Page::kDeptDetail) {
                            // 科室页不重复叠菜单，BOOT 才是返回键。
                        } else {
                            EndCall();
                            if (page_ != Page::kRoute) {
                                page_ = Page::kMenu;
                                cursor_ = 0;
                            }
                        }
                    } else if (page_ == Page::kRoute) {
                        if (input.action == Action::kCenter) {
                            // 失败重试、成功重查走同一条路：RouteStart 自带重入保护，请求中就忽略按键。
                            if (route_view_.state == RouteCard::State::kLoading) {
                                ESP_LOGI(TAG, "route: still loading, ignore center");
                            } else {
                                ESP_LOGI(TAG, "route: requery");
                                RealRoute();
                            }
                        } else {
                            // 翻页：Up 上一页、Down 下一页，到头回绕。刷新期间按键被门控丢弃，防误触连跳。
                            const int pages = RoutePages();
                            if (pages > 1 && (input.action == Action::kUp || input.action == Action::kDown)) {
                                route_page_ = input.action == Action::kUp ? (route_page_ + pages - 1) % pages
                                                                          : (route_page_ + 1) % pages;
                                ++view_;
                                ESP_LOGI(TAG, "route: page %d/%d", route_page_ + 1, pages);
                            }
                        }
                    } else if (input.action == Action::kCenter) {
                        Choose();
                    } else if (page_ == Page::kDeptList) {
                        const int count = DeptCountForKey(RouteHospitalKey());
                        if (count > 0 && (input.action == Action::kUp || input.action == Action::kDown)) {
                            dept_cursor_ = input.action == Action::kUp ? (dept_cursor_ + count - 1) % count
                                                                       : (dept_cursor_ + 1) % count;
                            cursor_ = dept_cursor_;
                            ++view_;
                        }
                    } else if (page_ == Page::kDeptDetail) {
                        // 详情页只认 BOOT 返回。
                    } else if (page_ != Page::kHome) {
                        const size_t rows = Rows();
                        cursor_ = input.action == Action::kUp ? (cursor_ + rows - 1) % rows : (cursor_ + 1) % rows;
                    }
                }
            }
            const int64_t now = esp_timer_get_time();
            if (now >= poll) {
                (void)SyncAgent();
                int percent = -1; bool charging = false;
                if (hardware_.Battery(&percent, &charging) == ESP_OK) {
                    battery_.store(percent); charging_.store(charging);
                }
                poll = now + 1000000;
            }
            if (call_) {
                const agent_state_t state = agent_link_state();
                if (state == AGENT_STATE_READY) {
                    if (!active_) { active_ = true; Message("请说话，中键挂断"); }
                    audio_.SetContinuous(true);
                } else if (active_ || state == AGENT_STATE_DISCONNECTED || now - requested_ >= 30000000) {
                    EndCall(); Message("连接已断，中键重试");
                }
            }
            if (volume_dirty && now - volume_at >= 1000000) { Save(); volume_dirty = false; }
            PollAutoRoute();
            Publish();
        }
    }
    void Keys() {
        epaper::Key keys[4];
        bool down[4]{};
        uint32_t pressed_view[4]{};
        const gpio_num_t pins[] = {KEY_UP, KEY_DOWN, KEY_CENTER, KEY_BOOT};
        while (true) {
            const int64_t now = esp_timer_get_time();
            for (int i = 0; i < 4; ++i) {
                const bool pressed = !gpio_get_level(pins[i]);
                if (pressed && !down[i]) pressed_view[i] = ui_.Displayed();
                down[i] = pressed;
                const epaper::Press press = keys[i].Poll(pressed, now);
                if (press == epaper::Press::kNone) continue;
                Action action = i == 0 ? Action::kUp : i == 1 ? Action::kDown : i == 3 ? Action::kBack :
                                press == epaper::Press::kHold ? Action::kMenu : Action::kCenter;
                Input input{action, pressed_view[i]};
                if (xQueueSend(events_, &input, 0) != pdTRUE) ESP_LOGW(TAG, "Input busy");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    static void ControlEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->ControlLoop(); }
    static void KeyEntry(void* context) { static_cast<WaveshareEpaperWifi*>(context)->Keys(); }

    EpaperHardware hardware_;
    EpaperWifiAudio audio_;
    epaper::Ui ui_;
    QueueHandle_t events_ = nullptr;
    bool screen_ok_ = false, hardware_ok_ = false;
    std::atomic<int> battery_{-1};
    std::atomic<bool> charging_{false};
    int volume_ = AUDIO_DEFAULT_VOLUME;
    int32_t agent_ = 0, saved_agent_ = 0;
    char session_[80]{}, session_name_[96]{}, agent_name_[80]{}, message_[192]{};
    agent_link_platform_conversation_t sessions_[3]{};
    size_t cursor_ = 0, count_ = 0, offset_ = 0;
    bool more_ = false, voice_ = false, call_ = false, active_ = false;
    Page page_ = Page::kHome;
    uint32_t view_ = 1;
    int64_t requested_ = 0;
    RouteCard route_view_{};
    RouteCard auto_card_{};
    uint32_t auto_baseline_version_ = 0;
    bool auto_baseline_ready_ = false;
    bool auto_route_pending_ = false;
    int64_t auto_next_probe_us_ = 0;
    int route_page_ = 0;
    int dept_cursor_ = 0;
    int dept_selected_ = 0;
    // 本轮语音回复的文本。科室楼层本来就在固件的静态表里，不需要再从云端取；
    // 缺的只有“这次该看哪个科”，而这句话 Agent 已经在语音里说过，本地认出来即可。
    char spoken_[900] = {};
};

DECLARE_APPLICATION(WaveshareEpaperWifi);

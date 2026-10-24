#include "application.h"
#include "config.h"
#include "epaper_hardware.h"
#include "epaper_wifi_audio.h"
#include "epaper_wifi_ui.h"
#include "epaper_controls.h"
#include "agent_link_platform.h"
#include "route_card.h"
#include "dept_table.h"

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
constexpr int kDeptRowsPerPage = 10;   // 科室名 4–5 字，40 px 一屏 10 条（与 ui.cc 的 kListRows 对齐）
enum class Page { kHome, kMenu, kSessions, kReset, kRoute, kDeptList, kDeptDetail };
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
        call_ = active_ = false; requested_ = 0;
        audio_.Stop(); (void)agent_link_end_realtime();
        // 上一轮对话的回复属于"这次通话"，不是首页的一部分。不清掉的话，
        // 按 BOOT 退出通话后首页会同时写着"请说一声你好"和上一轮的整段回答，
        // 老人会以为那是当前要说的话。Ui 那边同时会在回复消失时强制全刷，擦掉残影。
        ui_.Answer("");
    }
    bool SyncAgent(bool explain = false) {
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
        RouteStart(&route_view_);
        ESP_LOGI(TAG, "route: request started");
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
            snprintf(screen.footer, sizeof screen.footer, "中键重查  BOOT返回");
    }
    // 院内科室：先列表选科室，再出大字卡。数据是开机就在闪存里的静态表，离线可查。
    void DeptList() {
        EndCall();
        dept_cursor_ = 0;
        page_ = Page::kDeptList; ++view_;
        message_[0] = 0;
        ESP_LOGI(TAG, "dept: list opened, %d entries", kDepartmentCount);
    }
    void PublishDeptList(epaper::Screen& screen) {
        const int start = (dept_cursor_ / kDeptRowsPerPage) * kDeptRowsPerPage;
        int row = 0;
        for (int i = 0; i < kDeptRowsPerPage && start + i < kDepartmentCount; ++i, ++row)
            snprintf(screen.rows[row], sizeof screen.rows[row], "%d. %s", start + i + 1,
                     kDepartments[start + i].name);
        screen.selected = dept_cursor_ - start;
        snprintf(screen.title, sizeof screen.title, "中山医院科室");
        snprintf(screen.detail, sizeof screen.detail, "第 %d-%d 项，共 %d 项", start + 1, start + row,
                 kDepartmentCount);
        snprintf(screen.footer, sizeof screen.footer, "上下键选择  中键查看  BOOT返回");
    }
    void PublishDept(epaper::Screen& screen) {
        if (dept_selected_ < 0 || dept_selected_ >= kDepartmentCount) return;
        const DeptEntry& dept = kDepartments[dept_selected_];
        snprintf(screen.title, sizeof screen.title, "%s", dept.name);
        screen.row_scale = 2;
        for (int i = 0; i < 5; ++i)
            if (dept.lines[i]) snprintf(screen.rows[i], sizeof screen.rows[i], "%s", dept.lines[i]);
        snprintf(screen.detail, sizeof screen.detail, "%s", dept.hours);
        snprintf(screen.footer, sizeof screen.footer, "静态表·以现场为准·BOOT返回");
    }
    void Call() {
        if (call_) { EndCall(); Message("已挂断"); return; }
        if (!SyncAgent(true)) return;
        if (!session_[0]) { Sessions(); return; }
        if (!voice_) { Message("该智能体不支持语音"); return; }
        if (!audio_.Ready()) { Message("音频不可用，请重启"); return; }
        const agent_state_t state = agent_link_state();
        if (state != AGENT_STATE_CONNECTED && state != AGENT_STATE_READY) { Message("正在等待平台连接"); return; }
        ui_.Answer("");
        if (!Error(agent_link_request_realtime_session(agent_, session_))) {
            call_ = true; active_ = false; requested_ = esp_timer_get_time();
            Message("接通中，中键挂断");
        }
    }
    void Choose() {
        if (page_ == Page::kHome) { Call(); return; }
        if (page_ == Page::kMenu) {
            if (cursor_ == 0) RealRoute();
            else if (cursor_ == 1) DeptList();
            else if (cursor_ == 2) SampleRoute();
            else if (cursor_ == 3) Sessions();
            else if (cursor_ == 4) NewSession();
            else if (cursor_ == 5) { page_ = Page::kReset; cursor_ = 0; ++view_; }
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
        if (page_ == Page::kMenu) return 6;
        if (page_ == Page::kReset) return 2;
        return count_ + (more_ ? 1 : 0) + (offset_ ? 1 : 0) + 1;
    }
    void Publish() {
        epaper::Screen screen{}; screen.view = view_;
        if (page_ == Page::kRoute) { PublishRoute(screen); ui_.Publish(screen); return; }
        if (page_ == Page::kDeptList) { PublishDeptList(screen); ui_.Publish(screen); return; }
        if (page_ == Page::kDeptDetail) { PublishDept(screen); ui_.Publish(screen); return; }
        if (CLOUD_PRODUCT_ID <= 0) {
            snprintf(screen.title, sizeof screen.title, "未配置平台产品 ID");
            snprintf(screen.detail, sizeof screen.detail, "请在 products.json 注册本机型后重编");
        } else {
            snprintf(screen.title, sizeof screen.title, "%s", page_ == Page::kMenu ? "菜单" :
                     page_ == Page::kSessions ? "选择会话" : page_ == Page::kReset ? "重置激活？" :
                     active_ ? "请说话" : call_ ? "正在接通" : "就医通");
            if (page_ == Page::kReset) snprintf(screen.detail, sizeof screen.detail, "WiFi 保留，先在控制台解绑");
            else if (message_[0]) snprintf(screen.detail, sizeof screen.detail, "%s", message_);
            else if (page_ == Page::kHome) snprintf(screen.detail, sizeof screen.detail, "请说一声你好");
            else snprintf(screen.detail, sizeof screen.detail, "%s·%s", agent_name_[0] ? agent_name_ : "控制台未绑定",
                          session_name_[0] ? session_name_ : session_[0] ? "已选会话" : "未选会话");
        }
        if (page_ != Page::kHome) {
            screen.selected = static_cast<int>(cursor_);
            snprintf(screen.footer, sizeof screen.footer, "上下键选择  中键确认  BOOT返回");
            if (page_ == Page::kMenu) {
                snprintf(screen.rows[0], sizeof screen.rows[0], "去医院（查坐车路线）");
                snprintf(screen.rows[1], sizeof screen.rows[1], "院内科室位置");
                snprintf(screen.rows[2], sizeof screen.rows[2], "样例路线卡（离线）");
                snprintf(screen.rows[3], sizeof screen.rows[3], "选择已有会话");
                snprintf(screen.rows[4], sizeof screen.rows[4], "新建会话");
                snprintf(screen.rows[5], sizeof screen.rows[5], "重置平台激活");
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
            snprintf(screen.footer, sizeof screen.footer, "中键说话　长按菜单　音量%d%%", volume_);
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
                    // BOOT 逐级返回：科室详情 → 科室列表 → 主页。只有回到主页才结束通话。
                    // 通话中必须随时能挂断，所以通话态不设门控；纯页面导航则和其它键一样等屏幕刷完，
                    // 否则屏还在刷（2.45 s）时再按一次会连跳两级，看起来像"没逐级返回"。
                    const bool stale = !call_ && !epaper::Current(input.view, view_);
                    ESP_LOGI(TAG, "key: BOOT page=%d call=%d stale=%d", static_cast<int>(page_),
                             call_ ? 1 : 0, stale ? 1 : 0);
                    if (stale) {
                        // 丢弃：这一次按键属于上一屏，不处理
                    } else if (page_ == Page::kDeptDetail) {
                        page_ = Page::kDeptList; ++view_;
                    } else {
                        // 只有"确实在通话"时才报挂断。原来的写法无条件写一句 Call ended，
                        // 于是从菜单/科室列表按 BOOT 回首页也会弹这行——用户根本没打过电话，
                        // 看起来就像屏上凭空冒出对话窗口的内部状态。
                        const bool was_calling = call_;
                        EndCall(); Home();
                        if (was_calling) Message("已挂断");
                        else message_[0] = 0;   // 回首页就清提示，露出"请说一声你好"
                    }
                } else if (page_ == Page::kHome && (input.action == Action::kUp || input.action == Action::kDown)) {
                    int next = epaper::Volume(volume_, input.action == Action::kUp ? 1 : -1);
                    if (hardware_.AudioReady() && !Error(hardware_.codec.SetVolume(next))) {
                        volume_ = next; volume_dirty = true; volume_at = esp_timer_get_time();
                    }
                } else if (page_ == Page::kHome && call_ && input.action == Action::kCenter) {
                    EndCall(); Message("已挂断");
                } else if (epaper::Current(input.view, view_)) {
                    message_[0] = 0; ++view_;
                    if (input.action == Action::kMenu) {
                        EndCall(); page_ = Page::kMenu; cursor_ = 0;
                    } else if (page_ == Page::kDeptList) {
                        if (input.action == Action::kCenter) {
                            dept_selected_ = dept_cursor_;
                            page_ = Page::kDeptDetail; ++view_;
                            ESP_LOGI(TAG, "dept: open %d '%s'", dept_selected_,
                                     kDepartments[dept_selected_].name);
                        } else if (input.action == Action::kUp) {
                            dept_cursor_ = (dept_cursor_ + kDepartmentCount - 1) % kDepartmentCount;
                        } else if (input.action == Action::kDown) {
                            dept_cursor_ = (dept_cursor_ + 1) % kDepartmentCount;
                        }
                    } else if (page_ == Page::kDeptDetail) {
                        // 只认 BOOT 返回，其它键忽略，避免在详情页误翻页。
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
                    } else if (input.action == Action::kCenter) Choose();
                    else if (page_ != Page::kHome) {
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
    int route_page_ = 0;
    int dept_cursor_ = 0, dept_selected_ = 0;
};

DECLARE_APPLICATION(WaveshareEpaperWifi);

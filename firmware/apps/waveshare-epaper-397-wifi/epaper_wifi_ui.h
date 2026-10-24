#pragma once
#include <atomic>
#include <mutex>
#include "agent_link.h"
#include "epaper_hardware.h"
#include "lvgl.h"

namespace epaper {
struct Screen {
    uint32_t view = 1;
    char title[96] = "就医通";
    char detail[320] = "请说一声你好";
    char rows[6][112]{};
    int selected = -1;
    // 1 = 现有列表页（28 px 汉字，6 行）；2 = 路线卡大字页（56 px 汉字，5 行）。
    // 大字页重新排版整屏，标题与前四行放大，供老人一米外辨认。
    int row_scale = 1;
    // 临时状态（如"正在查路线"）。结果通常 0.7 秒内就到，这种页先压一会儿再画，
    // 否则会白占一个 2.5 秒的刷屏周期，把真正要看的路线卡往后推。
    bool provisional = false;
    // 竖屏原型：逻辑坐标 480×800，物理 RAM 仍是 800×480，靠 Pixel() 转置。
    // 只用于单独一页试验，其它页面不受影响。
    bool portrait = false;
    char footer[112] = "中键说话　长按菜单";
};
class Ui {
public:
    esp_err_t Start(EpaperHardware* hardware);
    void Status(const agent_link_status_t& value);
    void Answer(const char* text);
    void EndAnswer();
    void Publish(const Screen& value);
    uint32_t Displayed() const { return displayed_.load(); }
private:
    static void Entry(void* context) { static_cast<Ui*>(context)->Loop(); }
    void Loop();
    void Text(int x, int y, int max_y, const char* text, int scale = 1);
    // 单行裁切：放不下时截断并画省略号，绝不换行。路线卡行距固定，换行会压到下一行。
    void TextClipped(int x, int y, int max_y, const char* text, int scale);
    void Pixel(int x, int y);
    // 当前逻辑宽度：竖屏 480，横屏 800。Text/TextClipped 的换行与裁切都按它算。
    int Width() const { return portrait_ ? 480 : EPD_WIDTH; }
    EpaperHardware* hardware_ = nullptr;
    uint8_t* frame_ = nullptr;
    lv_font_t* cjk_font_ = nullptr;  // Owned by the display worker for its lifetime.
    std::mutex lock_;
    agent_link_status_t status_{};
    Screen screen_;
    char answer_[1024]{}, painted_answer_[1024]{};
    int64_t answer_at_ = 0;
    bool answer_done_ = false;
    int painted_scale_ = 1;  // 排版模式变化时强制全刷，避免大字页留下上一页残影
    int64_t provisional_since_ = 0;
    bool portrait_ = false;  // 本次绘制是否走竖屏转置
    std::atomic<uint32_t> displayed_{0};
};
}

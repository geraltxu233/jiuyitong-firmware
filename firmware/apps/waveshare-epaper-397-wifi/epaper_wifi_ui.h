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
    // 竖屏 480×800 的行容量：40 px 行 + 56 px 行距，从 y=180 到 y=724 能放 10 行。
    char rows[12][112]{};
    int selected = -1;
    // 1 = 列表页（40 px 汉字，一屏 10 行）；2 = 路线卡大字页（35 px 正文，一屏 9–11 行）。
    // 大字页重新排版整屏：标题 56 px，正文按宽度折行，供老人一米外辨认。
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
    // face/glyph_px 默认走 28 px 小字档，所有既有调用点行为不变。
    // 列表页与路线卡正文显式传 body_font_ 与 CJK_BODY_FONT_PX，拿到原生大字号。
    void Text(int x, int y, int max_y, const char* text, int scale = 1,
              const lv_font_t* face = nullptr, int glyph_px = CJK_FONT_PX);
    // 单行裁切：放不下时截断并画省略号，绝不换行。路线卡行距固定，换行会压到下一行。
    void TextClipped(int x, int y, int max_y, const char* text, int scale,
                     const lv_font_t* face = nullptr, int glyph_px = CJK_FONT_PX);
    // 按像素宽度折行的正文版：一条逻辑行折成若干物理行，各占一个行槽，续行缩进。
    // 返回用掉的槽数。槽用尽时返回剩余槽数，调用方据此提示"还有内容"。
    int TextWrapped(int x, int slot, int max_slot, const char* text,
                    const lv_font_t* face, int glyph_px);
    // 同一套折行逻辑的定位版：起点 y、行距、下边界都外部给定，不绑正文字段。
    // 首页提示语用它——那里没有行槽概念，只有"这一块能放几行"。
    // indent_px 为续行缩进，0 表示顶格（短提示语缩进反而显得参差）。
    int TextWrappedAt(int x, int y, int pitch, int max_y, const char* text,
                      const lv_font_t* face, int glyph_px, int indent_px);
    void Pixel(int x, int y);
    // 逻辑宽度固定为竖屏的 480；物理帧仍是 800×480，由 Pixel() 转置。
    int Width() const { return 480; }
    EpaperHardware* hardware_ = nullptr;
    uint8_t* frame_ = nullptr;
    lv_font_t* cjk_font_ = nullptr;       // 28 px：标题行、来源行、页脚、状态页
    lv_font_t* body_font_ = nullptr;      // 35 px：路线卡/科室卡正文
    lv_font_t* list_font_ = nullptr;      // 40 px：菜单 / 会话 / 科室列表项、首页提示语
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

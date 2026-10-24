#include "epaper_wifi_ui.h"
#include "cjk_font.h"
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace epaper {
namespace {
// 竖屏逻辑尺寸。物理 RAM 仍是 800×480，Pixel() 负责转置。
constexpr int kMargin = 20;
constexpr int kLogicalW = 480;
constexpr int kLogicalH = 800;
constexpr int kRuleTop = 160;    // 标题与正文之间的分隔线
constexpr int kBodyTop = 180;    // 正文首行基线顶
constexpr int kBodyPitch = 52;   // 35 px 行 + 17 px 间隙
constexpr int kBodyRows = 11;    // 180 + 10*52 = 700，末行底 735
constexpr int kNoticeY = 740;    // 来源行
constexpr int kFooterY = 766;    // 按键提示
// 列表页（菜单 / 会话 / 科室）用 40 px 正文章节：行距 56，一屏 10 行。
// 10 行是 480×800 竖屏在 40 px 下的上限：180 + 9*56 = 684，末行底 724，再往下就压到页脚。
constexpr int kListPitch = 56;
constexpr int kListRows = 10;
// 路线卡正文字高，必须与加载 body_font_ 时用的 px 一致，否则裁切判据会差一个字高。
constexpr int kBodyGlyphPx = CJK_BODY_FONT_PX;
// 列表项字高，同理必须与 list_font_ 的 px 一致。
constexpr int kListGlyphPx = CJK_LIST_FONT_PX;

// 这行里有没有 CJK。阈值与 Text() 里 prefer_cjk 保持一致（0x2000 起）。
// 用途只有一个：判断要不要按中文字体的基线对齐混排。
bool HasCjk(const char* text) {
    while (*text) {
        uint32_t cp = static_cast<unsigned char>(*text++);
        if (cp >= 0xc2 && cp <= 0xf4) {
            int more = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= more == 1 ? 0x1f : more == 2 ? 0x0f : 0x07;
            for (int i = 0; i < more; ++i) {
                const uint8_t c = static_cast<uint8_t>(*text);
                if ((c & 0xc0) != 0x80) { cp = '?'; break; }
                cp = (cp << 6) | (c & 0x3f); ++text;
            }
        }
        if (cp >= 0x2000) return true;
    }
    return false;
}
}

esp_err_t Ui::Start(EpaperHardware* hardware) {
    hardware_ = hardware;
    if (!hardware_->DisplayReady()) return ESP_ERR_INVALID_STATE;
    frame_ = static_cast<uint8_t*>(heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT / 8, MALLOC_CAP_SPIRAM));
    if (!frame_) return ESP_ERR_NO_MEM;
    if (xTaskCreate(Entry, "ep397_screen", 16384, this, 2, nullptr) != pdPASS) {
        free(frame_); frame_ = nullptr; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void Ui::Status(const agent_link_status_t& value) { std::lock_guard<std::mutex> guard(lock_); status_ = value; }
void Ui::Answer(const char* text) {
    if (!text) return;
    std::lock_guard<std::mutex> guard(lock_);
    if (strcmp(answer_, text) == 0) return;
    snprintf(answer_, sizeof answer_, "%s", text); answer_at_ = esp_timer_get_time(); answer_done_ = false;
}
void Ui::EndAnswer() { std::lock_guard<std::mutex> guard(lock_); answer_done_ = true; }
void Ui::Publish(const Screen& value) { std::lock_guard<std::mutex> guard(lock_); screen_ = value; }
void Ui::Pixel(int x, int y) {
    // 整机改为竖屏：逻辑 480×800 → 物理 800×480。
    // 对应"把设备顺时针转 90° 看"：逻辑纵轴 y 对应物理横轴 px，逻辑横轴 x 从物理底部往上。
    // 若实机是倒的/镜像，只改这两行（另一组是 px = 799 - y, py = x）。
    const int px = y, py = EPD_HEIGHT - 1 - x;
    if (px >= 0 && px < EPD_WIDTH && py >= 0 && py < EPD_HEIGHT)
        frame_[py * (EPD_WIDTH / 8) + px / 8] &= ~(0x80 >> (px % 8));
}
void Ui::Text(int x, int y, int max_y, const char* text, int scale,
              const lv_font_t* face, int glyph_px) {
    const int left = x;
    const lv_font_t* const primary = face ? face : cjk_font_;
    // 一行里中英文必须坐在同一条基线上。ASCII 现在也优先走同一份 CJK 字库，字体一致，
    // ascent 天然相同；只有字库缺字退回 Montserrat 时，才需要按汉字字体的 ascent 对齐。
    const int ref_ascent = (primary && HasCjk(text)) ? primary->line_height - primary->base_line : -1;
    while (*text && y + (glyph_px + 2) * scale <= max_y) {
        uint32_t cp = static_cast<unsigned char>(*text++);
        if (cp >= 0xc2 && cp <= 0xf4) {
            int more = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= more == 1 ? 0x1f : more == 2 ? 0x0f : 0x07;
            for (int i = 0; i < more; ++i) {
                const uint8_t c = static_cast<uint8_t>(*text);
                if ((c & 0xc0) != 0x80) { cp = '?'; break; }
                cp = (cp << 6) | (c & 0x3f); ++text;
            }
        } else if (cp >= 0x80) cp = '?';
        if (cp == '\n') { x = left; y += (glyph_px + 4) * scale; continue; }
        // ASCII 也优先走 CJK 字库：数字/英文与汉字同字号同基线。字库子集含 0x20–0x7E，
        // 本来就有拉丁字形；不这样做，40 px 汉字会配 20 px 数字，混排一眼就歪。
        const lv_font_t* candidates[] = {
            primary,
            &lv_font_montserrat_20,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = primary ? primary : &lv_font_montserrat_20;
        lv_font_glyph_dsc_t glyph{};
        bool found = false;
        for (const lv_font_t* candidate : candidates) {
            if (!candidate) continue;
            glyph = {};
            if (lv_font_get_glyph_dsc(candidate, &glyph, cp, 0) && !glyph.is_placeholder) {
                font = candidate; found = true; break;
            }
        }
        if (!found) {
            glyph = {};
            (void)lv_font_get_glyph_dsc(font, &glyph, '?', 0);
        }
        const int zoom = font == &lv_font_simsun_14_cjk ? 2 * scale : scale;
        if (x + glyph.adv_w * zoom >= Width() - kMargin) { x = left; y += (glyph_px + 4) * scale; }
        if (y + (glyph_px + 2) * scale > max_y) break;
        if (glyph.box_w && glyph.box_h && glyph.box_w <= 64 && glyph.box_h <= 64) {
            alignas(16) uint8_t pixels[4096]{};
            lv_draw_buf_t buffer{};
            const uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
            if (lv_draw_buf_init(&buffer, glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8,
                                 stride, pixels, sizeof pixels) == LV_RESULT_OK) {
                // TinyTTF returns its cached draw buffer; bitmap fonts use the supplied buffer.
                const auto* bitmap = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&glyph, &buffer));
                if (bitmap && bitmap->data) {
                    const int ascent = ref_ascent > 0 ? ref_ascent : font->line_height - font->base_line;
                    const int top = ascent - glyph.box_h - glyph.ofs_y;
                    for (unsigned row = 0; row < glyph.box_h; ++row)
                        for (unsigned col = 0; col < glyph.box_w; ++col)
                            if (bitmap->data[row * bitmap->header.stride + col] >= 96)
                                for (int dy = 0; dy < zoom; ++dy)
                                    for (int dx = 0; dx < zoom; ++dx)
                                        Pixel(x + (glyph.ofs_x + static_cast<int>(col)) * zoom + dx, y + (top + static_cast<int>(row)) * zoom + dy);
                }
                lv_font_glyph_release_draw_data(&glyph);
            }
        }
        x += glyph.adv_w * zoom;
    }
}
// 路线卡专用：一行就是一行，超宽截断并画省略号。
// 不复用 Text() 的原因：Text() 是换行语义（y += 32*scale），本页行距只有 70 px，
// 换行后的第二行会压在下一行字上。字体选择顺序与 Text() 保持一致，改一处必须同步改另一处。
void Ui::TextClipped(int x, int y, int max_y, const char* text, int scale,
                     const lv_font_t* face, int glyph_px) {
    if (y + (glyph_px + 2) * scale > max_y) return;
    const char* const whole = text;
    const lv_font_t* const primary = face ? face : cjk_font_;
    const int limit = Width() - kMargin;
    const int reserve = glyph_px * scale + 8;  // 给省略号留的宽度
    // 与 Text() 同一条规则：含汉字的行按汉字字体 ascent 对齐，避免数字英文被抬成上标。
    const int ref_ascent = (primary && HasCjk(text)) ? primary->line_height - primary->base_line : -1;

    // 返回推进宽度；放不下（或版面已到 max_y）返回 -1，一个像素都不画。
    auto draw = [&](uint32_t cp, int px, int slack) -> int {
        // 与 Text() 保持一致：ASCII 也优先走 CJK 字库，字号与基线统一。
        const lv_font_t* candidates[] = {
            primary,
            &lv_font_montserrat_20,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = primary ? primary : &lv_font_montserrat_20;
        lv_font_glyph_dsc_t glyph{};
        bool found = false;
        for (const lv_font_t* candidate : candidates) {
            if (!candidate) continue;
            glyph = {};
            if (lv_font_get_glyph_dsc(candidate, &glyph, cp, 0) && !glyph.is_placeholder) {
                font = candidate; found = true; break;
            }
        }
        if (!found) {
            glyph = {};
            (void)lv_font_get_glyph_dsc(font, &glyph, '?', 0);
        }
        const int zoom = font == &lv_font_simsun_14_cjk ? 2 * scale : scale;
        const int advance = glyph.adv_w * zoom;
        if (px + advance + slack > limit) return -1;
        if (glyph.box_w && glyph.box_h && glyph.box_w <= 64 && glyph.box_h <= 64) {
            alignas(16) uint8_t pixels[4096]{};
            lv_draw_buf_t buffer{};
            const uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
            if (lv_draw_buf_init(&buffer, glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8,
                                 stride, pixels, sizeof pixels) == LV_RESULT_OK) {
                const auto* bitmap = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&glyph, &buffer));
                if (bitmap && bitmap->data) {
                    const int ascent = ref_ascent > 0 ? ref_ascent : font->line_height - font->base_line;
                    const int top = ascent - glyph.box_h - glyph.ofs_y;
                    for (unsigned row = 0; row < glyph.box_h; ++row)
                        for (unsigned col = 0; col < glyph.box_w; ++col)
                            if (bitmap->data[row * bitmap->header.stride + col] >= 96)
                                for (int dy = 0; dy < zoom; ++dy)
                                    for (int dx = 0; dx < zoom; ++dx)
                                        Pixel(px + (glyph.ofs_x + static_cast<int>(col)) * zoom + dx,
                                              y + (top + static_cast<int>(row)) * zoom + dy);
                }
                lv_font_glyph_release_draw_data(&glyph);
            }
        }
        return advance;
    };

    while (*text) {
        uint32_t cp = static_cast<unsigned char>(*text++);
        if (cp >= 0xc2 && cp <= 0xf4) {
            int more = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= more == 1 ? 0x1f : more == 2 ? 0x0f : 0x07;
            for (int i = 0; i < more; ++i) {
                const uint8_t c = static_cast<uint8_t>(*text);
                if ((c & 0xc0) != 0x80) { cp = '?'; break; }
                cp = (cp << 6) | (c & 0x3f); ++text;
            }
        } else if (cp >= 0x80) cp = '?';
        if (cp == '\n') break;
        // reserve 只在"后面还有字"时才扣。原来的写法每个字都预留省略号宽度，
        // 28 px 时余量足看不出问题；40 px 下 440 px 只够 10 个字，再扣掉 48 px
        // 就只剩 9 个字，10 字行会被无谓地截成 9 字加省略号。
        const int advance = draw(cp, x, *text ? reserve : 0);
        if (advance < 0) {
            (void)draw(0x2026, x, 0);  // U+2026 省略号
            // 联调用：屏上只有一个 …，看不出是哪一行写长了。同一条长行只报一次。
            static char reported[112];
            if (strcmp(reported, whole) != 0) {
                snprintf(reported, sizeof reported, "%s", whole);
                ESP_LOGW("WS397.ui", "line clipped (scale %d): %s", scale, whole);
            }
            return;
        }
        x += advance;
    }
}
// 折行版正文。与 TextClipped 只差一条：放不下时换到下一行，而不是画省略号。
// 三处字形选择顺序必须一致（Text / TextClipped / 这里），改一处要同步改另外两处。
int Ui::TextWrappedAt(int x, int y, int pitch, int max_y, const char* text,
                      const lv_font_t* face, int glyph_px, int indent_px) {
    const lv_font_t* const primary = face ? face : cjk_font_;
    const int limit = Width() - kMargin;
    const int ref_ascent = (primary && HasCjk(text)) ? primary->line_height - primary->base_line : -1;

    // 量宽并绘制。放不下返回 -1，一个像素都不落。
    // allow_over：处在数字串中间时放行，宁可溢出一点也不能把「150 米」断成「15」「0 米」。
    auto draw = [&](uint32_t cp, int px, int py, bool allow_over) -> int {
        const lv_font_t* candidates[] = {
            primary,
            &lv_font_montserrat_20,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = primary ? primary : &lv_font_montserrat_20;
        lv_font_glyph_dsc_t glyph{};
        bool found = false;
        for (const lv_font_t* candidate : candidates) {
            if (!candidate) continue;
            glyph = {};
            if (lv_font_get_glyph_dsc(candidate, &glyph, cp, 0) && !glyph.is_placeholder) {
                font = candidate; found = true; break;
            }
        }
        if (!found) {
            glyph = {};
            (void)lv_font_get_glyph_dsc(font, &glyph, '?', 0);
        }
        const int zoom = font == &lv_font_simsun_14_cjk ? 2 : 1;
        const int advance = glyph.adv_w * zoom;
        if (px + advance > limit && !allow_over) return -1;
        if (glyph.box_w && glyph.box_h && glyph.box_w <= 64 && glyph.box_h <= 64) {
            alignas(16) uint8_t pixels[4096]{};
            lv_draw_buf_t buffer{};
            const uint32_t stride = lv_draw_buf_width_to_stride(glyph.box_w, LV_COLOR_FORMAT_A8);
            if (lv_draw_buf_init(&buffer, glyph.box_w, glyph.box_h, LV_COLOR_FORMAT_A8,
                                 stride, pixels, sizeof pixels) == LV_RESULT_OK) {
                const auto* bitmap = static_cast<const lv_draw_buf_t*>(lv_font_get_glyph_bitmap(&glyph, &buffer));
                if (bitmap && bitmap->data) {
                    const int ascent = ref_ascent > 0 ? ref_ascent : font->line_height - font->base_line;
                    const int top = ascent - glyph.box_h - glyph.ofs_y;
                    for (unsigned row = 0; row < glyph.box_h; ++row)
                        for (unsigned col = 0; col < glyph.box_w; ++col)
                            if (bitmap->data[row * bitmap->header.stride + col] >= 96)
                                for (int dy = 0; dy < zoom; ++dy)
                                    for (int dx = 0; dx < zoom; ++dx)
                                        Pixel(px + (glyph.ofs_x + static_cast<int>(col)) * zoom + dx,
                                              py + (top + static_cast<int>(row)) * zoom + dy);
                }
                lv_font_glyph_release_draw_data(&glyph);
            }
        }
        return advance;
    };

    int line = 0;
    int cur_x = x;
    bool prev_digit = false;
    while (*text) {
        uint32_t cp = static_cast<unsigned char>(*text++);
        if (cp >= 0xc2 && cp <= 0xf4) {
            int more = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            cp &= more == 1 ? 0x1f : more == 2 ? 0x0f : 0x07;
            for (int i = 0; i < more; ++i) {
                const uint8_t c = static_cast<uint8_t>(*text);
                if ((c & 0xc0) != 0x80) { cp = '?'; break; }
                cp = (cp << 6) | (c & 0x3f); ++text;
            }
        } else if (cp >= 0x80) cp = '?';
        if (cp == '\n') { prev_digit = false; continue; }

        const bool is_digit = (cp >= '0' && cp <= '9');
        int advance = draw(cp, cur_x, y + line * pitch, prev_digit && is_digit);
        if (advance < 0) {
            // 本行放不下：换下一行，续行按 indent_px 缩进后重试。
            // 下一行会越过 max_y 就收手——剩下的字宁可不画，也不能把版面撑破。
            if (y + (line + 1) * pitch + glyph_px > max_y) break;
            ++line;
            cur_x = x + indent_px;
            advance = draw(cp, cur_x, y + line * pitch, false);
            if (advance < 0) continue;  // 缩进后还放不下（字比一行还宽），跳过这个字
        }
        cur_x += advance;
        prev_digit = is_digit;
    }
    return line + 1;
}
// 正文字段版：槽位换算成坐标后交给 TextWrappedAt。返回用掉的槽数。
int Ui::TextWrapped(int x, int slot, int max_slot, const char* text,
                    const lv_font_t* face, int glyph_px) {
    // 续行缩进一个汉字：能看出这是上一步的延续，又不至于把尾行推到屏幕中间。
    const int used = TextWrappedAt(x, kBodyTop + slot * kBodyPitch, kBodyPitch,
                                   kBodyTop + (max_slot - 1) * kBodyPitch + glyph_px,
                                   text, face, glyph_px, glyph_px);
    return used > 0 ? used : 1;
}
void Ui::Loop() {
    lv_init();  // This profile uses LVGL fonts only; this worker owns every LVGL call.
    cjk_font_ = LoadCjkFont(CJK_FONT_PARTITION, CJK_FONT_PX, CJK_FONT_CACHE_GLYPHS);
    if (!cjk_font_) ESP_LOGW("WS397.ui", "Chinese font unavailable; using the built-in subset. Flash app and font together.");
    // 第二个实例读同一份 mmap 的 TTF，只是按 35 px 重新栅格化。加载失败就退回 28 px 字库，
    // 正文会小回去但不会画不出来——宁可字小，也不能白屏。
    body_font_ = LoadCjkFont(CJK_FONT_PARTITION, CJK_BODY_FONT_PX, CJK_BODY_CACHE_GLYPHS);
    if (!body_font_) { ESP_LOGW("WS397.ui", "body font %d px unavailable; body falls back to 28 px", CJK_BODY_FONT_PX); body_font_ = cjk_font_; }
    // 第三个实例给列表页（40 px）。同样一份 TTF、只换栅格尺寸，flash 里不会多存一个字。
    list_font_ = LoadCjkFont(CJK_FONT_PARTITION, CJK_LIST_FONT_PX, CJK_LIST_CACHE_GLYPHS);
    if (!list_font_) { ESP_LOGW("WS397.ui", "list font %d px unavailable; lists fall back to body", CJK_LIST_FONT_PX); list_font_ = body_font_; }
    // 混排基线要对齐的就是这两个 ascent 之差，打出来便于核对实际偏移量。
    ESP_LOGI("WS397.ui", "font metrics: cjk lh=%d base=%d ascent=%d | body lh=%d base=%d | list lh=%d base=%d | latin lh=%d base=%d ascent=%d",
             cjk_font_ ? cjk_font_->line_height : -1, cjk_font_ ? cjk_font_->base_line : -1,
             cjk_font_ ? cjk_font_->line_height - cjk_font_->base_line : -1,
             body_font_ ? body_font_->line_height : -1, body_font_ ? body_font_->base_line : -1,
             list_font_ ? list_font_->line_height : -1, list_font_ ? list_font_->base_line : -1,
             lv_font_montserrat_20.line_height, lv_font_montserrat_20.base_line,
             lv_font_montserrat_20.line_height - lv_font_montserrat_20.base_line);
    // 这些缓冲原来全在任务栈上。Screen 涨到 12 行（约 2 KB）后，栈上再放 content/previous
    // 两个 4 KB 会吃满 16 KB 栈；本任务独占一个 Ui 实例，改成静态没有并发问题。
    static Screen screen;
    static agent_link_status_t status;
    static char answer[sizeof answer_], previous[4096];
    int64_t completed = -10000000, retry = 0;
    unsigned refreshes = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> guard(lock_);
            screen = screen_; status = status_;
            if (!answer_[0] || answer_done_ || now - answer_at_ >= 2000000)
                snprintf(answer, sizeof answer, "%s", answer_);
        }
        // 状态栏只在等激活码时出现；平时屏上不该有机型、在线状态这些东西。
        // 同一个 show_status 既控制绘制，也控制重绘判据——否则连上/断开这类
        // 内部状态变化会白白触发一次 2 秒的整屏刷新。
        const bool show_status = status.code[0] != '\0';
        char status_line[sizeof status.hint];
        if (show_status) snprintf(status_line, sizeof status_line, "激活码：%s", status.code);
        else status_line[0] = 0;
        static char content[4096];
        snprintf(content, sizeof content, "%d%d|%s|%s|%s|%s|%s|%d|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s",
                 screen.portrait ? 1 : 0, screen.row_scale, show_status ? status.title : "", status_line,
                 screen.title, screen.detail, screen.footer,
                 screen.selected, screen.rows[0], screen.rows[1], screen.rows[2], screen.rows[3],
                 screen.rows[4], screen.rows[5], screen.rows[6], screen.rows[7], screen.rows[8],
                 screen.rows[9], screen.rows[10], screen.rows[11]);
        // Streamed answer updates settle before refresh and never block microphone or network workers.
        const bool changed = strcmp(content, previous) != 0 || (!screen.rows[0][0] && strcmp(answer, painted_answer_) != 0);
        // 临时页（"正在查路线"）先压 700 ms：实测局域网 680 ms 就能拿到结果，
        // 抢着画这一页反而要多等一个 2.5 s 的刷屏周期。慢网时它只是晚 0.7 s 出现，仍会显示。
        if (screen.provisional) { if (!provisional_since_) provisional_since_ = now; }
        else provisional_since_ = 0;
        const bool holding = provisional_since_ && now - provisional_since_ < 700000;
        // 大字页要的是"尽快出结果"；2 s 节流是为流式语音文本设的，不该套在路上。
        const bool urgent = screen.row_scale >= 2;
        if (changed && !holding && now >= retry && (urgent || now - completed >= 2000000)) {
            portrait_ = true;
            memset(frame_, 0xff, EPD_WIDTH * EPD_HEIGHT / 8);
            if (screen.row_scale >= 2) {
                // 大字卡（路线 / 科室）：标题 56 px 给"去哪"，正文 35 px 给"怎么走"。
                // 标题用 Text()（可换行）：竖屏 440 px 一行只放 7 个汉字，
                // "去复旦大学附属中山医院"这种 11 字标题必须折成两行，不能截断——截了就读不出是哪家医院。
                Text(kMargin, 24, 150, screen.title, 2);
                for (int x = kMargin; x < Width() - kMargin; ++x) Pixel(x, kRuleTop);
                // 正文 35 px，按宽度折行：一条逻辑行可以占多个行槽，续行缩进一字。
                // 槽用满时剩下的逻辑行留在下一页，绝不为了塞进去而截断或压行。
                int slot = 0;
                for (int i = 0; i < kBodyRows && slot < kBodyRows; ++i) {
                    if (!screen.rows[i][0]) break;
                    slot += TextWrapped(kMargin, slot, kBodyRows, screen.rows[i], body_font_, kBodyGlyphPx);
                }
                if (screen.detail[0]) TextClipped(kMargin, kNoticeY, kNoticeY + 30, screen.detail, 1);
                Text(kMargin, kFooterY, kLogicalH, screen.footer);
            } else if (show_status) {
                // 配网/激活阶段：6 位激活码必须露出来，状态区在这一阶段是功能性的。
                Text(kMargin, 16, 50, "就医通");
                TextClipped(kMargin, 56, 110, status.title, 2);
                TextClipped(kMargin, 120, 200, status_line, 2);
                for (int x = kMargin; x < Width() - kMargin; ++x) Pixel(x, 214);
                TextClipped(kMargin, 230, 296, screen.title, 1);
                TextClipped(kMargin, 300, kNoticeY - 10, screen.detail, 1);
                if (screen.rows[0][0]) {
                    for (int i = 0; i < kBodyRows; ++i) {
                        const int y = 380 + i * kBodyPitch;
                        if (!screen.rows[i][0]) break;
                        if (screen.selected == i) Text(kMargin - 2, y, y + 40, ">");
                        Text(kMargin + 28, y, y + 40, screen.rows[i]);
                    }
                } else Text(kMargin, 380, kNoticeY - 10, answer);
                Text(kMargin, kFooterY, kLogicalH, screen.footer);
            } else if (screen.rows[0][0]) {
                // 列表页（菜单 / 会话 / 科室列表）：正文 40 px，一屏 10 行。
                TextClipped(kMargin, 40, 168, screen.title, 2);
                for (int x = kMargin; x < Width() - kMargin; ++x) Pixel(x, kRuleTop);
                for (int i = 0; i < kListRows; ++i) {
                    const int y = kBodyTop + i * kListPitch;
                    if (!screen.rows[i][0]) break;
                    if (screen.selected == i)
                        Text(kMargin - 2, y, y + kListPitch, ">", 1, list_font_, kListGlyphPx);
                    // 列表一行只能占一行：超宽就截断加省略号。折行会叠到下一项的头上，
                    // 那比"名字看不全"更糟——老人会读成另一条。
                    TextClipped(kMargin + 26, y, y + kListPitch - 4, screen.rows[i], 1,
                                list_font_, kListGlyphPx);
                }
                Text(kMargin, kFooterY, kLogicalH, screen.footer);
            } else {
                // 首页：一块屏只讲一件事——叫什么、现在该做什么。
                TextClipped(kMargin, 120, 270, screen.title, 2);
                // 提示语与列表页同为 40 px，老人不用在两套字号之间来回适应。
                // 300/356/412 三行，超出就停笔：提示语都是本固件自己写的短句，
                // 宁可少写一行，也不能让它压到下面的回复文字上。
                TextWrappedAt(kMargin, 300, kListPitch, 470, screen.detail,
                              list_font_, kListGlyphPx, 0);
                if (answer[0]) Text(kMargin, 485, kNoticeY - 10, answer);
                Text(kMargin, kFooterY, kLogicalH, screen.footer);
            }
            // 排版模式切换（列表 ↔ 大字）必须全刷，否则上一页的字会透过来。
            // 大字页每次干脆都全刷：56 px 的字一旦残影，一眼就能看出来，不值得省这一次。
            // 回复文字被清空（挂断 / 退出通话）也要全刷：那一整段字在屏上占了三五行，
            // 半刷新只按新帧重画，旧的那块底色会留下一层灰影，看起来像"没擦干净"。
            const bool answer_gone = painted_answer_[0] && !answer[0];
            const bool full = refreshes % 16 == 0 || answer_gone || screen.row_scale != painted_scale_ ||
                              screen.row_scale >= 2 || screen.portrait;
            const int64_t paint_at = esp_timer_get_time();
            const esp_err_t error = hardware_->Display(frame_, full);
            completed = esp_timer_get_time();
            const long paint_ms = static_cast<long>((completed - paint_at) / 1000);
            if (error == ESP_OK) {
                ++refreshes; snprintf(previous, sizeof previous, "%s", content);
                snprintf(painted_answer_, sizeof painted_answer_, "%s", answer);
                painted_scale_ = screen.row_scale;
                displayed_.store(screen.view);
                ESP_LOGI("WS397.ui", "refresh ok: scale=%d full=%d %ldms first='%s'",
                         screen.row_scale, full ? 1 : 0, paint_ms, screen.rows[0][0] ? screen.rows[0] : screen.title);
            } else {
                ESP_LOGW("WS397.ui", "Refresh failed after %ldms: %s", paint_ms, esp_err_to_name(error));
                retry = completed + 30000000;
            }
        } else if (!changed && now >= retry) displayed_.store(screen.view);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
}

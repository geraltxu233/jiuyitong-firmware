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
    if (portrait_) {
        // 逻辑 480×800 → 物理 800×480。对应"把设备顺时针转 90° 看"：
        // 逻辑纵轴 ly 对应物理横轴 px，逻辑横轴 lx 从物理底部往上。
        // 若实机看到的是倒的/镜像，只需改这两行（另一组是 px=799-ly, py=lx）。
        const int px = y, py = 479 - x;
        if (px >= 0 && px < EPD_WIDTH && py >= 0 && py < EPD_HEIGHT)
            frame_[py * (EPD_WIDTH / 8) + px / 8] &= ~(0x80 >> (px % 8));
        return;
    }
    if (x >= 0 && x < EPD_WIDTH && y >= 0 && y < EPD_HEIGHT)
        frame_[y * (EPD_WIDTH / 8) + x / 8] &= ~(0x80 >> (x % 8));
}
void Ui::Text(int x, int y, int max_y, const char* text, int scale) {
    const int left = x;
    // 一行里中英文必须坐在同一条基线上。LVGL 的定位用的是"该字形所属字体"的行高，
    // 汉字字体行高 32、Montserrat 20 只有 24，混排时数字英文会被抬高约 7 px 变成上标。
    // 含汉字的行统一按汉字字体的 ascent 定位，纯英文行保持原来的行为不动。
    const int ref_ascent = (cjk_font_ && HasCjk(text)) ? cjk_font_->line_height - cjk_font_->base_line : -1;
    while (*text && y + 30 * scale <= max_y) {
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
        if (cp == '\n') { x = left; y += 32 * scale; continue; }
        // Include general punctuation (curly quotes, dashes, ellipsis), not just Hanzi.
        const bool prefer_cjk = cp >= 0x2000 && cjk_font_;
        const lv_font_t* candidates[] = {
            prefer_cjk ? cjk_font_ : &lv_font_montserrat_20,
            prefer_cjk ? &lv_font_montserrat_20 : cjk_font_,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = &lv_font_montserrat_20;
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
        if (x + glyph.adv_w * zoom >= Width() - 20) { x = left; y += 32 * scale; }
        if (y + 30 * scale > max_y) break;
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
void Ui::TextClipped(int x, int y, int max_y, const char* text, int scale) {
    if (y + 30 * scale > max_y) return;
    const char* const whole = text;
    const int limit = Width() - 20;
    const int reserve = 28 * scale + 8;  // 给省略号留的宽度
    // 与 Text() 同一条规则：含汉字的行按汉字字体 ascent 对齐，避免数字英文被抬成上标。
    const int ref_ascent = (cjk_font_ && HasCjk(text)) ? cjk_font_->line_height - cjk_font_->base_line : -1;

    // 返回推进宽度；放不下（或版面已到 max_y）返回 -1，一个像素都不画。
    auto draw = [&](uint32_t cp, int px, int slack) -> int {
        const bool prefer_cjk = cp >= 0x2000 && cjk_font_;
        const lv_font_t* candidates[] = {
            prefer_cjk ? cjk_font_ : &lv_font_montserrat_20,
            prefer_cjk ? &lv_font_montserrat_20 : cjk_font_,
            &lv_font_simsun_14_cjk,
        };
        const lv_font_t* font = &lv_font_montserrat_20;
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
        const int advance = draw(cp, x, reserve);
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
void Ui::Loop() {
    lv_init();  // This profile uses LVGL fonts only; this worker owns every LVGL call.
    cjk_font_ = LoadCjkFont(CJK_FONT_PARTITION, CJK_FONT_PX, CJK_FONT_CACHE_GLYPHS);
    if (!cjk_font_) ESP_LOGW("WS397.ui", "Chinese font unavailable; using the built-in subset. Flash app and font together.");
    // 混排基线要对齐的就是这两个 ascent 之差，打出来便于核对实际偏移量。
    ESP_LOGI("WS397.ui", "font metrics: cjk lh=%d base=%d ascent=%d | latin lh=%d base=%d ascent=%d",
             cjk_font_ ? cjk_font_->line_height : -1, cjk_font_ ? cjk_font_->base_line : -1,
             cjk_font_ ? cjk_font_->line_height - cjk_font_->base_line : -1,
             lv_font_montserrat_20.line_height, lv_font_montserrat_20.base_line,
             lv_font_montserrat_20.line_height - lv_font_montserrat_20.base_line);
    Screen screen{};
    agent_link_status_t status{};
    char answer[sizeof answer_]{}, previous[2048]{};
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
        if (show_status) snprintf(status_line, sizeof status_line, "Activation code: %s", status.code);
        else status_line[0] = 0;
        char content[2048];
        snprintf(content, sizeof content, "%d%d|%s|%s|%s|%s|%s|%d|%s|%s|%s|%s|%s|%s",
                 screen.portrait ? 1 : 0, screen.row_scale, show_status ? status.title : "", status_line,
                 screen.title, screen.detail, screen.footer,
                 screen.selected, screen.rows[0], screen.rows[1], screen.rows[2], screen.rows[3],
                 screen.rows[4], screen.rows[5]);
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
            portrait_ = screen.portrait;   // 必须在本屏所有 Pixel() 之前设定
            memset(frame_, 0xff, EPD_WIDTH * EPD_HEIGHT / 8);
            if (screen.portrait) {
                // 竖屏原型：逻辑 480×800。正文用的是 Text()（换行语义），不是 TextClipped（截断语义）——
                // 竖屏更窄，靠换行和更多行数容纳同样内容，正是这次要验证的事。
                Text(20, 14, 46, "竖屏原型 480x800", 1);
                for (int x = 20; x < Width() - 20; ++x) Pixel(x, 56);
                Text(20, 66, 132, screen.title, 2);          // 56 px，每行 7 个汉字
                Text(20, 138, 438, screen.rows[0], 2);       // 56 px 自动换行
                for (int x = 20; x < Width() - 20; ++x) Pixel(x, 452);
                Text(20, 464, 764, screen.detail, 1);        // 28 px 自动换行，每行约 15 字
                Text(20, 770, 800, screen.footer, 1);
            } else if (screen.row_scale >= 2) {
                // 大字路线卡：标题、5 行正文、来源、按键提示。没有状态栏——一块屏只讲一件事。
                TextClipped(20, 6, 74, screen.title, 2);
                for (int x = 20; x < EPD_WIDTH - 20; ++x) Pixel(x, 78);
                for (int i = 0; i < 5; ++i) {
                    if (!screen.rows[i][0]) break;
                    TextClipped(20, 84 + i * 66, 146 + i * 66, screen.rows[i], 2);
                }
                if (screen.detail[0]) TextClipped(20, 410, 442, screen.detail, 1);
                Text(20, 444, 480, screen.footer);
            } else if (show_status) {
                // 配网/激活阶段：6 位激活码必须露出来，状态区在这一阶段是功能性的。
                Text(20, 12, 60, "就医通");
                Text(20, 52, 85, status.title);
                Text(20, 86, 154, status_line);
                for (int x = 20; x < EPD_WIDTH - 20; ++x) Pixel(x, 157);
                Text(20, 166, 200, screen.title);
                Text(20, 202, 270, screen.detail);
                if (screen.rows[0][0]) {
                    for (int i = 0; i < 6; ++i) {
                        if (screen.selected == i) Text(20, 276 + i * 28, 446, ">");
                        Text(50, 276 + i * 28, 446, screen.rows[i]);
                    }
                } else Text(20, 276, 438, answer);
                Text(20, 446, 480, screen.footer);
            } else if (screen.rows[0][0]) {
                // 列表页（菜单 / 会话 / 重置）：标题一行，六行列表，行距放大方便点选。
                TextClipped(20, 14, 66, screen.title, 2);
                for (int x = 20; x < EPD_WIDTH - 20; ++x) Pixel(x, 74);
                for (int i = 0; i < 6; ++i) {
                    const int y = 84 + i * 46;
                    if (screen.selected == i) Text(18, y, y + 44, ">");
                    Text(48, y, y + 44, screen.rows[i]);
                }
                Text(20, 446, 480, screen.footer);
            } else {
                // 首页：一块屏只讲一件事——叫什么、现在该做什么。
                TextClipped(20, 70, 150, screen.title, 2);
                TextClipped(20, 200, 280, screen.detail, 2);
                if (answer[0]) Text(20, 300, 420, answer);
                Text(20, 446, 480, screen.footer);
            }
            // 排版模式切换（列表 ↔ 大字）必须全刷，否则上一页的字会透过来。
            // 大字页每次干脆都全刷：56 px 的字一旦残影，一眼就能看出来，不值得省这一次。
            const bool full = refreshes % 16 == 0 || screen.row_scale != painted_scale_ ||
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

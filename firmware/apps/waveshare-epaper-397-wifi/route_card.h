#pragma once

#include <cstdint>
#include "esp_err.h"

// 路线卡：导航后端返回的最小字段集合。
// 显示文字由后端精简，固件只负责排版上屏，不做语义加工。
//
// 响应契约（GET，见 INTERFACE 文档 I3）：
//   {
//     "code": 0,
//     "title": "去XX医院",
//     "lines": ["出小区东门向东走200米到XX站", "乘58路，开往XX方向", "坐6站，XX站下车", "下车向西走200米到医院"],
//     "source": "腾讯公交 10:32"
//   }
//   code 非 0 时可用 "message" 给出一句可直接显示的中文原因。
//   每行不超过 12 个汉字；超长会被固件截断而不是换行（换行会与下一行重叠）。

// 演示环境的后端地址。改地址只需改这里并重新编译烧录。
//
// 首选云端路线卡接口：把完整 URL 写成 printf 风格模板，唯一参数是当前会话 ID，
// 例如：
//   "https://care.example.com/api/route-card?session_id=%s"
// 会话 ID 由平台生成，固件只透传，不把地图 Key 或平台 Key 放进设备。
// 模板为空时回退到下面的旧版固定起终点接口，保证局域网演示不受影响。
#ifndef WS_ROUTE_SESSION_ENDPOINT
#define WS_ROUTE_SESSION_ENDPOINT "https://keleshiyizhimao.com/api/route-card?session_id=%s"
#endif
#ifndef WS_ROUTE_SESSION_FALLBACKS
#define WS_ROUTE_SESSION_FALLBACKS ""
#endif

// 仅用于烧录前的公网 TLS 实机探针。默认关闭；开启时，真实路线入口会依次
// 请求百度 robots.txt 与 B 站公开导航 API，并将结果写到串口和屏幕，不会解析为路线卡。
// 探针结束后必须恢复为 0，避免上线构建误把路线请求改成测试请求。
#ifndef WS_ROUTE_HTTPS_PROBE
#define WS_ROUTE_HTTPS_PROBE 0
#endif

// 旧版固定起终点接口：仅用于云端会话接口尚未配置或没有选择会话时的演示。
// 手机热点每次重启都会换网段（实测 192.168.82.x → 192.168.219.x），而 PC 在热点里
// 通常是第一个 DHCP 客户端、尾号稳定为 .100。所以主地址之外再留几个候选，
// 连不上就依次换，避免为了一个网段改动专门重烧一次。
#ifndef WS_ROUTE_ENDPOINT
#define WS_ROUTE_ENDPOINT "http://192.168.219.100:8000/api/transit?from=demo&to=hospital"
#endif
#ifndef WS_ROUTE_FALLBACKS
#define WS_ROUTE_FALLBACKS \
    "http://192.168.82.100:8000/api/transit?from=demo&to=hospital", \
    "http://192.168.1.100:8000/api/transit?from=demo&to=hospital"
#endif

struct RouteCard {
    enum class State { kIdle, kLoading, kOk, kFailed };
    static constexpr int kMaxLines = 12;

    State state = State::kIdle;
    char title[64] = {};
    char lines[kMaxLines][112] = {};
    int count = 0;
    char source[80] = {};      // 数据来源与查询时间；失败时放可直接显示的原因
    esp_err_t error = ESP_OK;  // 失败时的底层错误码，仅用于日志
    int64_t updated_us = 0;
    // 卡片版本：优先用服务端 cards.card_id；旧服务端没有该字段时由整段 JSON
    // 做 FNV-1a 指纹。只用于判断“这是不是一张新卡”，不参与路线语义。
    uint32_t version = 0;
    int card_id = 0;
    char built_at[24] = {};
};

// 单次 GET，阻塞到拿到结果或超时（最长约 8 秒），从不抛异常、不重试。
// session_id 非空且配置了 WS_ROUTE_SESSION_ENDPOINT 时，优先按会话取最新路线卡；
// 否则使用旧版固定起终点演示接口。由 route 任务独占调用，不要在其他任务里直接调。
void RouteFetch(RouteCard* card, const char* session_id = nullptr);

// 非阻塞触发：起一个一次性任务（栈 8 KB）执行 RouteFetch，完成后 card->state 变
// kOk 或 kFailed。调用方在等待期间轮询 state，不要在此之后再改动同一 card。
// 同一时刻只允许一个请求在跑，session_id 会被复制，不依赖调用方生命周期。
// 返回 false 表示已有请求在跑、任务创建失败或参数无效，调用方应稍后重试。
bool RouteStart(RouteCard* card, const char* session_id = nullptr);

// 当前构建优先使用的后端地址，启动时打一条日志，避免"改了没生效"的怀疑。
const char* RouteEndpoint();
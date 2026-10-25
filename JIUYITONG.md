# 就医通固件分支

本仓是 [Deotaland/Agent_link](https://github.com/Deotaland/Agent_link) 的派生分支，服务一个具体项目：**给老人做一块只会说话、只会显示一行行大字路线卡的墨水屏导诊牌**。

硬件是微雪 ESP32-S3 3.97 寸墨水屏（`waveshare-epaper-397-wifi`），单键操作，无触屏。

上游 SDK 的 BLE 配网、OTA、音频流、平台接入全部保留，本分支只改应用层与两处传输层行为。

## 相对上游改了什么

| 文件 | 改动 | 为什么 |
| --- | --- | --- |
| `apps/waveshare-epaper-397-wifi/epaper_wifi.cc` | 重写界面层：竖屏、三档字号、单键状态机、路线卡分页、科室详情页 | 老人看不清小字，一屏必须只讲一件事 |
| `apps/waveshare-epaper-397-wifi/route_card.cc/.h` | 新增 `card_id` / `built_at` 字段，兼容旧响应；通话中静默轮询版本 | 避免"屏幕还停在上一张卡" |
| `apps/waveshare-epaper-397-wifi/dept_table.cc/.h` | 科室表扩到四家医院 26 个科室，含楼层、候诊区、门诊时间 | 交通页之后要能说清"到楼里怎么走" |
| `components/agent_link/src/transports/wifi/cloud/cloud_realtime.cc` | ① 平台会话统一 `endpointing.mode=server`；② 回复期间暂停上行、播放排空后恢复；③ 写超时 1s→3s、上行批次 8→4 包；④ 原会话有界重连 3 次 | 见下节 |
| `main/CMakeLists.txt` | 增加 `mbedtls` 依赖 | HTTPS 校验证书链需要 |

### 语音掉线的根因

首版固件在老人说完一段较长的症状描述后频繁掉线，串口证据是：

```
audio WSS send failed after 1004/1009 ms
```

排查过程排除了两个错误假设：不是 180 秒空闲超时（掉线发生在 1 秒级），也不是模型复述卡住（下行音频是连续的）。

真实原因是**上行背压**：服务端生成长语音时下行占用带宽，麦克风仍按约 44 KB/s 连续上行，TCP 发送缓冲被顶满，1 秒写超时直接把 WSS 拆了。

修复分三层：

1. `response.started` 时暂停上行并清空积压，本地播放队列排空后 250 ms 恢复，另有 90 秒兜底恢复；
2. 上行批次从 8 包降到 4 包，写超时放宽到 3 秒，降低单次写阻塞概率；
3. 非主动挂断的断开，在同一会话内按 1s / 2.5s / 4s 重试 3 次，避免瞬时抖动直接结束通话。

### 断句交给服务端

上游直连会话默认 `client PTT`，但这块板子的音频任务是连续通话，从不发送 `input_audio.activity/end`。结果是麦克风与转写都正常，平台却一直等不到本轮结束。

本分支把平台会话与直连会话统一改为 `endpointing.mode=server`；外部 grant 路径仍保留客户端 activity 协议。

## 交互收敛过程

早期版本要求老人「长按中键进菜单 → 短按选中 → 再长按看科室」，实测老人会反复按错。现在的规则：

| 场景 | 行为 |
| --- | --- |
| 按中键 | 开始通话 |
| 说完停顿 | 服务端自动断句，无需按键 |
| 再按中键 | 挂断；若云端已有新卡，自动切到路线页 |
| 路线页按中键 | 重查 |
| 路线页长按 | 直接进本次推荐的科室详情，不再让老人上下键挑 |
| 无唯一推荐科室 | 显示"请到一楼服务台问"，**不列科室** |

最后一条是踩坑后加的：语音里 Agent 说"您去一楼服务台问一下"，屏幕却把全院科室楼层铺满，语音和屏幕直接矛盾。宁可不给信息，也不给错信息。

## 构建

ESP-IDF v5.5.x：

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
```

烧录只写应用分区，不动 bootloader、分区表与字库，NVS 中的配网信息与设备绑定会保留：

```bash
python flash_local.py COM6
```

`flash_local.py` 会读 `build/flasher_args.json`，分块写入并逐块校验，失败自动重试，最后做整镜像校验。比一次性 `idf.py flash` 更适合 USB 供电不稳的长线。

## 配置

`apps/waveshare-epaper-397-wifi/route_card.h`：

```c
#define WS_ROUTE_SESSION_ENDPOINT "https://<你的域名>/api/route-card?session_id=%s"
#define WS_ROUTE_ENDPOINT         "http://192.168.x.100:8000/api/transit?..."   // 仅局域网调试
```

`WS_ROUTE_ENDPOINT` 及候选地址是局域网联调用的明文 HTTP 入口，正式链路只走上面的 HTTPS。板端 TLS 握手需要约 32 KB 连续内存，通话期间不够用，所以局域网调试必须绕开 TLS。

**本仓不含任何密钥。** 激活码、设备绑定、Wi-Fi 凭据都存在板子 NVS 里，不进代码。

## 已知限制

- 语音稳定性修复只经过单次长回复复测，样本量不足以宣称问题已彻底关闭；
- 医院与科室表硬编码四家（中山、仁济、市六、肿瘤），未接官方号源；
- 墨水屏全刷约 2 秒，交互刻意限制成"一屏一件事"；
- 未做长时间浸泡测试（断电、断网、弱信号）。

## 许可

继承上游仓库的许可条款。上游自带的 Muse 固件、`AGENTS.md`、`CLAUDE.md` 等文件与本项目无关，保持原样未改。

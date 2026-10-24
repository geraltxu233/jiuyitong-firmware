#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "sdkconfig.h"
#include "platform_config.h"
#include "platform_product.h"

// Waveshare ESP32-S3-ePaper-3.97, N16R8. Verified against the official IDF BSP.
#define EPAPER_MODEL "WS-EPAPER-397"
#define CLOUD_BASE_URL DEOTALAND_PLATFORM_BASE_URL
#define CLOUD_PRODUCT_ID AGENT_LINK_PRODUCT_ID
#define CLOUD_CHIP_TYPE "esp32-s3"
#define SYS_I2C_PORT I2C_NUM_0
#define SYS_I2C_SDA GPIO_NUM_41
#define SYS_I2C_SCL GPIO_NUM_42
#define PMIC_ADDR 0x34
#define ES8311_ADDR 0x18
#define AUDIO_I2S_MCLK GPIO_NUM_13
#define AUDIO_I2S_BCLK GPIO_NUM_14
#define AUDIO_I2S_WS GPIO_NUM_47
#define AUDIO_I2S_DIN GPIO_NUM_21
#define AUDIO_I2S_DOUT GPIO_NUM_48
#define AUDIO_PA GPIO_NUM_39
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_DEFAULT_VOLUME 60
#define AUDIO_PLAY_BUF_BYTES (256 * 1024)
#define EPD_SPI_HOST SPI3_HOST
#define EPD_SCLK GPIO_NUM_11
#define EPD_MOSI GPIO_NUM_12
#define EPD_CS GPIO_NUM_10
#define EPD_DC GPIO_NUM_9
#define EPD_RST GPIO_NUM_46
#define EPD_BUSY GPIO_NUM_3
#define CJK_FONT_PARTITION "anim_pack"
// 小字档：标题按 scale=2 放大到 56 px，来源行与页脚直接用 28 px。
#define CJK_FONT_PX 28
// 28 px 一屏有 100 多个不同汉字，32 个槽会反复淘汰重栅格化；提到 128 只是 PSRAM，不吃 flash。
#define CJK_FONT_CACHE_GLYPHS 128
// 正文档：路线卡正文原来 28 px 太小（老人一米外看不清），单独再挂一个实例。
// 字号是 Noto Sans SC 矢量现场栅格化，不是位图放大，任意 px 都清晰。
// 35 px 是按实拍文案定的：480 宽减左右边距剩 440 px，实测最长行 427 px 一行放得下。
// 40 px 时「乘58路，开往人民广场方向」这类 12 字行要折 4 条，续行只剩 1–2 个字，整屏碎掉。
#define CJK_BODY_FONT_PX 35
#define CJK_BODY_CACHE_GLYPHS 128
// 列表档（菜单 / 会话 / 科室）：条目本身短，最长一条「去医院（查坐车路线）」实测 400 px，
// 440 px 一行放得下，所以能比路线正文再大一档到 40 px。
// 与正文档分开两个实例，是因为两类页面要的东西相反：列表要"够大"，路线要"一行装得下"。
// 列表一屏最多 10 行 × 8 字，96 个字形槽够用；再大只是吃 PSRAM，不吃 flash。
#define CJK_LIST_FONT_PX 40
#define CJK_LIST_CACHE_GLYPHS 96
#define EPD_WIDTH 800
#define EPD_HEIGHT 480
#define KEY_UP GPIO_NUM_4
#define KEY_CENTER GPIO_NUM_5
#define KEY_DOWN GPIO_NUM_6
#define KEY_BOOT GPIO_NUM_0

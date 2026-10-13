// TFT_eSPI 在 ESP32-C3 上的 SPI 寄存器基址缺陷修正。
//
// 症状：固件启动即 Guru Meditation (Store access fault)，MTVAL = 0x10，
//       崩溃点 TFT_eSPI::begin_tft_write() 的 SET_BUS_WRITE_MODE 一行。
//
// 成因链：
//   1. IDF 的 soc/soc.h 定义
//          #define REG_SPI_BASE(i)  (((i)==2) ? (DR_REG_SPI2_BASE) : (0))
//      参数须为「外设序号 2」。
//   2. TFT_eSPI 的 Processors/TFT_eSPI_ESP32_C3.h 定义
//          #define SPI_PORT SPI2_HOST
//      而 SPI2_HOST 在 hal/spi_types.h 中是枚举值 1（SPI2_HOST=1）。
//   3. 于是 Processors/TFT_eSPI_ESP32_C3.c 里的文件级全局量
//          volatile uint32_t* _spi_user = (volatile uint32_t*)(SPI_USER_REG(SPI_PORT));
//      展开为 REG_SPI_BASE(1) + 0x10 = 0 + 0x10 = 0x10。
//   4. 首次写该寄存器即触发 Store access fault。
//
// TFT_eSPI 自身有兜底 `#ifndef REG_SPI_BASE`，但 IDF 已定义过它，故兜底不生效。
//
// 修正方式：本文件经编译期 -include 强制预包含到每个编译单元（含库自身的
// 编译单元）。先包含 soc.h 锁定其 include guard，再强制把基址指向 C3 上
// 唯一的 GPSPI。由于 guard 已锁，后续 soc.h 不会再次定义该宏。
#pragma once

#include <soc/soc.h>

#undef REG_SPI_BASE
#define REG_SPI_BASE(i) (DR_REG_SPI2_BASE)

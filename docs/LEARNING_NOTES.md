# ESP32-S3-RLCD-4.2 学习笔记

> 整理时间：2026-07-29 ｜ 来源：Waveshare「相关资料」页
> https://docs.waveshare.net/ESP32-S3-RLCD-4.2/Resources-And-Documents
> 配合 `BOARD_REFERENCE.md` 使用（本文补充其未覆盖/易错的点）。

---

## 1. 本次新增下载的资料

页面上的「相关资料」清单已几乎齐备，本次补齐缺失的**官方示例程序压缩包**：

| 资料 | 状态 | 位置 |
|------|------|------|
| ESP32-S3-RLCD-4.2 示例程序（ZIP） | ✅ 本次下载 | `waveshare-demo/ESP32-S3-RLCD-4.2-Demo.zip`（168MB）→ 解压 527MB |
| 原理图 / ST7305 / ES8311 / PCF85063 / SHTC3 数据手册 | ✅ 已在 `datasheets/` | — |
| 官方示例仓库（GitHub clone） | ✅ 已在 `examples/` | `examples/ESP32-S3-RLCD-4.2/` |
| 结构尺寸 3D 文件（.rar） | ⚠️ 未下载（需 Windows/UnRAR，暂跳过） | — |

**压缩包与 clone 的差异**：压缩包顶层是 `01_Arduino/ 02_ESP-IDF/ 03_XiaoZhi/ 04_Firmware/`，
而 clone 是 `01_Arduino_Libraries/ 02_Example/ 03_Firmware/`。**代码内容等价**，只是打包布局不同
（压缩包把 Arduino 库、ESP-IDF、小智 AI、固件各自平铺；clone 把库与示例分开放）。两份都在本地，
以 clone 的 `02_Example/` 为日常查阅入口更顺手。

---

## 1.5 压缩包版 vs clone 版：第三方库版本差异

用脚本扫描了两棵树的 `library.properties` / `idf_component.yml` / 版本头文件，结果如下。

**① 两边共有、版本完全一致（无冲突）：**

| 库 / 依赖 | 版本 | 备注 |
|-----------|------|------|
| lvgl (Arduino) v8 | **8.4.0** | 两边相同 |
| lvgl (Arduino) v9 | **9.3.0** | 两边相同 |
| SensorLib | **0.3.1** | Arduino 版 + ESP-IDF `ExternLib` 版均 0.3.1 |
| esp_codec_dev (`ExternLib`) | **1.3.5** | 两边相同（audio 组件） |
| ESP-IDF LVGL v8 例程依赖 | `lvgl/lvgl ^8.4.0` | `08_LVGL_V8_Test/main/idf_component.yml` 明示 |

**② clone 独有、压缩包缺失的部分：**

| 内容 | clone 位置 | 压缩包 |
|------|-----------|--------|
| Arduino U8g2 库 | `01_Arduino_Libraries/U8g2` = **2.36.18** | ❌ 完全没有（压缩包 `01_Arduino/libraries` 仅 SensorLib/lvgl8/lvgl9） |
| Arduino 示例 `10_U8G2_Test` | 有 | ❌ 压缩包 Arduino 示例只有 01–09 |
| ESP-IDF 示例 `11_U8G2_Test` | 有 | ❌ 压缩包 ESP-IDF 示例只有 01–10 |
| U8g2 组件（ESP-IDF） | `11_U8G2_Test/components/u8g2` | ❌ 缺失 |

**结论**：压缩包（wiki 资源页，ZIP 内文件日期 2026-01-20）比 GitHub clone（2026-07-29 拉取）**整体更旧**；
两边重叠的库版本**零冲突**，clone 额外多了 U8G2 的 Arduino(10)+ESP-IDF(11) 例程与 U8g2 库（含 ESP-IDF 组件）。
日常开发直接用 clone 的 `02_Example/` 即可；压缩包的意义是 wiki「一键下载」的官方整体打包。

---

## 2. ST7305 显示驱动（最难、最值得学的一节）

源码：`02_Example/Arduino/10_U8G2_Test/ST7305_U8g2.cpp`（U8g2 设备驱动），ESP-IDF 版在
`02_Example/ESP-IDF/11_U8G2_Test/`，逻辑相同。

### 2.1 SPI 与时序参数
- SPI 时钟 **24 MHz**，SPI_MODE0，MSB first，走 **HSPI**。
- 引脚（已在 `user_config.h` 交叉确认）：

  | 信号 | GPIO |
  |------|------|
  | SCK  | 11 |
  | MOSI | 12 |
  | DC   | 5  |
  | CS   | 40 |
  | RST  | 41 |
  | **TE**（撕裂同步） | **6** ← `BOARD_REFERENCE.md` 之前漏记，已补 |

### 2.2 分辨率与旋转
- 面板**原生 300×400（竖屏）**。U8g2 tile = 宽 38（=300/8）、高 50（=400/8）。
- 横屏使用时逻辑分辨率翻转成 400×300，靠 MADCTL(0x36) 的 MV 位实现（见下）。

### 2.3 初始化序列（fullInit 关键寄存器）
完整序列在 `ST7305_U8g2.cpp:220-275`，挑重点：

| 寄存器 | 写入值 | 含义 |
|--------|--------|------|
| 0xD6 | `17 02` | 接口/数据格式配置 |
| 0xC0–0xC5 | 重复 `69/19/4B` 等 | VCOM、源驱动、灰阶 gamma 阶梯（4 级灰度的电压梯度） |
| **0x11** | — | SLPOUT 退出睡眠，**必须 delay 120ms** |
| 0x36 | `48` | **MADCTL**（数据手册证实，见下）：位序 `MY MX MV 0 DO GS 0 0`；0x48 = MY=0/MX=1/MV=0/DO=1 → 仅水平镜像+数据序，**未做行列交换(MV=0)** |
| 0x3A | `11` | **DTFORM**(Data Format Select，非 COLMOD)：D0=BPS 选 SPI 输入打包(0=4写/TYPE1,1=3写/TYPE2)，D4=XDE 上下交换开关；0x11→BPS=1(TYPE2)&XDE=1。2bpp 是面板固有，不由本寄存器选 |
| **0x21** | — | INVON 显示反相：**反射式面板必需**，使帧缓冲 0≈白（反射）、1≈黑 |
| 0x2A | `12 2A` | 列地址窗口 起点 0x12 / 终点 0x2A |
| 0x2B | `00 C7` | 行地址窗口 起点 0x00 / 终点 0xC7(=199) |
| 0x29 | — | DISPON 开显示 |

> 注意：行地址窗口终点仅 0xC7(199)，但物理 400 行。这是 ST7305 在 2bpp 模式下
> 的「压缩寻址」——每 1 个地址行承载 2 个物理扫描行，配合下面的交错打包使用。

#### 2.3.1 数据手册实证与对初稿的修正（来源：ST7305_Datasheet.pdf）

读数据手册后，对 §2.3 初稿的两点解释做了**修正**（详见 `2026-07-29` 工作记录）：

1. **0x36 不是「横屏旋转开关」**。
   数据手册 p55/57 明确：`36h` = **MADCTL (Memory Data Access Control)**，数据字节位序为
   `1 MY MX MV 0 DO GS 0 0`（D7..D0）。`0x48` = `0100 1000` → **MY=0, MX=1, MV=0, DO=1, GS=0**。
   - MV(D5)=0 ⇒ **没有**行列交换，所以 0x48 并不产生 90° 硬件旋转。
   - 它的作用是 **MX=1（列地址递减=水平镜像）+ DO=1（数据序）**，用于补偿面板 FPC 走线方向，
     使后续软件旋转后的画面不左右翻转。
   - **横屏(400×300)是 U8g2 的软件旋转回调（如 `U8G2_R1`）完成的**，与 MADCTL 的 MV 无关。

2. **0x3A 不是「选 2bpp 像素格式」的寄存器**。
   数据手册 p55/57：`3Ah` = **DTFORM (Data Format Select)**，不是 ST77xx 那种 COLMOD。
   位定义（数据手册 bit description 原文）：
   - **D0 = BPS**：Bytes-Per-Pixel / 写次数选择。`0` = 4 次写操作打包 24-bit 数据(TYPE1)；`1` = 3 次写(TYPE2)。
   - **D4 = XDE**：Data Up/Down Switch。`0`=开（配合 MY=1），`1`=关。
   - `0x11` = `0001 0001` ⇒ **D4=1(XDE 关)、D0=1(BPS=TYPE2)**。
   - 2bpp / 4 灰度是 **ST7305 反射式面板的固有特性**（由 gamma/电压与面板结构决定），**不由 0x3A 选择**。
   - 顺带：Waveshare 选 BPS=1(TYPE2，每次 8 位写) 正好匹配驱动里「1 字节=4 像素(2bpp)」的打包方式；
     数据手册自带的参考初始化(p52)写的是 `0x10`(BPS=0/TYPE1)，是另一种打包，二者皆可用。

> 备注：§2.3 里「行地址窗口仅 0xC7(199)、每地址行承载 2 物理行」属**推断**，本次未从数据手册逐字核实，
> 保留为待验证项（见 §5 待办）。

### 2.4 像素打包逻辑（真正难懂的部分，U8X8_MSG_DISPLAY_DRAW_TILE）
ST7305 不是普通的「1 字节=1 像素或 8 像素」，而是 **2bpp + 跨 4 个子行交错** 的私有扫描序。
代码核心（`ST7305_U8g2.cpp:156-177`）：

```
st_lut[4][4]:
  {0x00,0x80,0x40,0xC0}   // 像素 bit0-1
  {0x00,0x20,0x10,0x30}   // 像素 bit2
  {0x00,0x08,0x04,0x0C}   // 像素 bit4
  {0x00,0x02,0x01,0x03}   // 像素 bit6
```

绘制一个 8×8 tile 时：
1. 按列地址分块，每 12 列一组，地址基址偏移 0x12；列地址命令 `0x2A` 发的是
   `{0x3C - addr_end, 0x3C - addr_start}` —— **注意 0x3C 反相**，这是 ST7305 专属列寻址。
2. 对 4 个「子行」(sr=0..3，shift = sr*2) 各生成一行输出字节：
   - 连续 4 个源像素（2bpp，每像素占 2 bit）交错进 **1 个输出字节** ——
     4 个像素的 bit 经 `st_lut[0..3]` 移位后 `OR` 合并。
   - 即 **1 字节 = 4 像素**，且这 4 个像素分布在 4 条不同子行上（交错扫描序）。
3. 最后用 `0x2C`(RAMWR) 一次性写入 `send_cnt * 4` 字节（4 子行 × 每行 send_cnt 字节）。

**结论**：要自己写驱动，最稳妥是**直接复用官方 `ST7305_U8g2.cpp` 的 `st_lut` 与列地址反相公式**，
不要凭直觉推导字节序，这块极易写错而满屏乱码。

---

## 3. 音频链路（codec_board 组件）

源码：`02_Example/Arduino/07_Audio_Test/src/ExternLib/codec_board/`

- **ES8311**（DAC，驱动扬声器）：I2S 输出，带 `pa_pin`（功放使能）。
  - ⚠️ `pa_pin` 必须拉高（BOARD_REFERENCE 记为 GPIO46）扬声器才出声。
- **ES7210**（ADC，双麦克风采集）：I2S 输入。
- **TCA9554**（I2C IO 扩展器，地址 **0x40**，源码实证）：`lcd_init.c` 用它经 I2C 配置/设置 IO 电平，
  把部分 GPIO（显示或功放控制）从 ESP32 直连改为 I2C 扩展，节省引脚。
- ES8311/ES7210 的 I2C 地址走 esp_codec_dev 默认值（未在本文件夹硬编码，典型 0x18 / 0x40–0x41）。
- I2S 引脚（BOARD_REFERENCE）：BCLK=9、DOUT(喇叭)=8、DIN(麦)=10、MCLK=16、LRCLK=45。

> 移植提醒：音视频例程强依赖 `ExternLib/codec_board` 组件（ES8311+ES7210+TCA9554），
> 单独抽例程时必须带上它，否则编不过或静音。

---

## 4. 上手速查（详见 BOARD_REFERENCE.md）
- 烧出厂固件 / ESP-IDF / Arduino / ESPHome 四种方式，BOARD_REFERENCE §6 已写全。
- 三个硬坑：**PSRAM 必开**、**USB 下载加 `--before usb_reset`**、**扬声器 GPIO46 功放使能拉高**。

---

## 5. 待办 / 可继续深挖
- [x] 对比 `01_Arduino/`(压缩包) 与 `02_Example/Arduino/`(clone) 的库版本差异 —— 见下方「库版本差异」一节。
- [x] 读 `ST7305_Datasheet.pdf` 核对 0x36/0x3A 的 bit 定义 —— 见 §2.3.1，并修正了初稿两处误解。
- [ ] 下载并查看结构尺寸 3D 文件（.rar，需 UnRAR）。
- [ ] 从数据手册核实 0x2B 行窗口仅 0xC7(199) 的「2 物理行/地址行」推断（目前未逐字证实）。
- [ ] 对比压缩包与 clone 的 ESP-IDF LVGL(^8.4.0) 例程是否还有源码级差异。

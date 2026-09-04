#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include "lvgl_bsp.h"

static lv_disp_draw_buf_t disp_buf; 		// contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      		// contains callback functions
static SemaphoreHandle_t lvgl_mux = NULL;

static const char *TAG = "LvglPort";

/* LVGL 任务栈容量。tileview 切页会触发全量布局递归（lv_obj_update_layout），
 * 栈不足会向下踩内部 RAM 堆，把 lv_disp_t 的 top_layer/act_scr 改坏，
 * 随后刷新时解引用坏指针 -> LoadProhibited 崩溃重启。
 * 历史：8KB -> 16KB(8-13) -> 32KB(8-31)；9-4 起用 [stack] 水位日志实测校验。 */
#define LVGL_TASK_STACK_BYTES  (32 * 1024)

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

extern "C" void *lv_mem_alloc_psram(size_t size)
{
    if (size == 0) return NULL;
    /* 优先 PSRAM（8MB，容量不再是瓶颈）；PSRAM 耗尽时回退内部 RAM，
     * 绝不返回 NULL —— LVGL 拿到 NULL 会造出半初始化对象，后续解引用必崩。 */
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) return p;
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

extern "C" void lv_mem_free_psram(void *ptr)
{
    if (ptr) heap_caps_free(ptr);   // heap_caps_free 自动识别所属堆
}

extern "C" void *lv_mem_realloc_psram(void *ptr, size_t size)
{
    if (!ptr) return lv_mem_alloc_psram(size);
    if (size == 0) { lv_mem_free_psram(ptr); return NULL; }

    /* 优先在 PSRAM 内原地扩展/搬迁 */
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) return p;

    /* PSRAM 不足：搬回内部 RAM。heap_caps_realloc 失败时原块仍然有效，
     * 用 heap_caps_get_allocated_size 取原大小安全拷贝。 */
    size_t old = heap_caps_get_allocated_size(ptr);
    void *n = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (!n) return NULL;            // 让 LVGL 自行处理分配失败，原块保持不变
    memcpy(n, ptr, size < old ? size : old);
    heap_caps_free(ptr);
    return n;
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	static void *last_top = (void *)-1;
  	static void *last_act = (void *)-1;
  	/* 8-31：内存水位周期上报（20s）。切页重启的真凶是堆耗尽，必须能观测。 */
  	static uint32_t last_mem_ms = 0;
  	static uint32_t stack_low = UINT32_MAX;   /* 9-4：栈历史最低水位（剩余字节） */
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1))
  	  	{
  	  	  	/* 8-13 诊断：监视 disp 指针突变——top_layer/act_scr 被清零
  	  	  	 * 是 LoadProhibited 崩溃的直接前兆，记录清零时刻对齐行为日志 */
  	  	  	lv_disp_t *d = lv_disp_get_default();
  	  	  	if (d) {
  	  	  	  	if ((void*)d->top_layer != last_top) {
  	  	  	  	  	Serial.printf("[lvgl] top_layer %p -> %p\n", last_top, (void*)d->top_layer);
  	  	  	  	  	last_top = (void*)d->top_layer;
  	  	  	  	}
  	  	  	  	if ((void*)d->act_scr != last_act) {
  	  	  	  	  	Serial.printf("[lvgl] act_scr %p -> %p\n", last_act, (void*)d->act_scr);
  	  	  	  	  	last_act = (void*)d->act_scr;
  	  	  	  	}
  	  	  	}
  	  	  	task_delay_ms = lv_timer_handler();

  	  	  	/* 9-4：栈水位监测。切页时 tileview 触发全量布局递归，
  	  	  	 * lv_obj_update_layout 栈峰值一旦触底就会向下踩内部 RAM 堆
  	  	  	 * （表现为随后 lv_obj_update_layout(NULL) LoadProhibited 崩溃）。
  	  	  	 * 这里记录历史最低水位，用来判断当前栈容量是否真正够用。 */
  	  	  	{
  	  	  	  	uint32_t hwm = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
  	  	  	  	if (hwm < stack_low) {
  	  	  	  	  	stack_low = hwm;
  	  	  	  	  	Serial.printf("[stack] LVGL task new low: %u B left (of %u)\n",
  	  	  	  	  	  	  	(unsigned)stack_low, (unsigned)(LVGL_TASK_STACK_BYTES));
  	  	  	  	}
  	  	  	}

  	  	  	/* 8-31：内存水位上报——内部 RAM / PSRAM 空闲量与最大连续块。
  	  	  	 * 密集切页重启的根因是堆耗尽，这里给出可观测证据。 */
  	  	  	{
  	  	  	  	uint32_t now = millis();
  	  	  	  	if (now - last_mem_ms > 20000) {
  	  	  	  	  	last_mem_ms = now;
  	  	  	  	  	Serial.printf("[mem] internal free=%u largest=%u | psram free=%u largest=%u | stack_low=%u\n",
  	  	  	  	  	  	  	(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
  	  	  	  	  	  	  	(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
  	  	  	  	  	  	  	(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
  	  	  	  	  	  	  	(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
  	  	  	  	  	  	  	(unsigned)stack_low);
  	  	  	  	}
  	  	  	}
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  	}
}


void Lvgl_PortInit(int width, int height, DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_color_t *buffer1 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer1);
	lv_color_t *buffer2 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer2);

    lv_disp_draw_buf_init(&disp_buf, buffer1, buffer2, width * height);
    ESP_LOGI(TAG, "Register display driver to LVGL");

    lv_disp_drv_init(&disp_drv);
  	disp_drv.hor_res = width;
  	disp_drv.ver_res = height;
  	disp_drv.flush_cb = flush_cb;
	disp_drv.full_refresh = 1;
  	disp_drv.draw_buf = &disp_buf;
  	lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
  	lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

    /* 8-13 审核修复：栈 8KB -> 16KB。tileview 全量布局递归（4 页对象树全量计算）
     * 时 lv_obj_update_layout 栈峰值可超 8KB；溢出会向下踩内部 RAM 堆区，
     * 把 lv_disp_t.top_layer/act_scr 清零 -> 下个刷新 tick lv_obj_update_layout(NULL)
     * LoadProhibited 崩溃（密集切页时频繁全量布局 -> 高频触发）。
     * 8-31 修复：16KB 仍不够。首页新增法语卡(200组)/农历/行情卡后 tileview 布局
     * 递归加深，WiFi 键盘 Ctrl+1/2/3 快速连按时 lv_obj_update_layout 栈峰值再破 16KB
     * -> 依旧踩内部 RAM 堆（实测 lv_mem_test 堆完整性失败 + LoadProhibited 崩溃）。
     * 再提到 32KB（内部 RAM 当前空闲 ~180KB，任务栈仅多占 16KB，安全）。 */
    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", LVGL_TASK_STACK_BYTES, NULL, 5, NULL, 0);
}

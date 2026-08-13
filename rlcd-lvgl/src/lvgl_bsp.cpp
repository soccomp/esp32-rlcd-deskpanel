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

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	static void *last_top = (void *)-1;
  	static void *last_act = (void *)-1;
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
     * LoadProhibited 崩溃（密集切页时频繁全量布局 -> 高频触发）。 */
    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", 16 * 1024, NULL, 5, NULL, 0);
}

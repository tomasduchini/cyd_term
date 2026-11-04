#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "term.h"
#include "boot.h"
#include "console.h"
#include "frame.h"
#include "pages.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>
#include "net.h"
#include "backlight.h"
#include "page_mon.h"
#include "pcstat.h"
#include "freertos/task.h"
#include "settings.h"

#define LCD_HOST     SPI2_HOST
#define PIN_SCLK     14
#define PIN_MOSI     13
#define PIN_MISO     12
#define PIN_CS       15
#define PIN_DC       2
#define PIN_BL       21
#define LCD_H_RES    320
#define LCD_V_RES    240
#define LCD_PCLK_HZ  (40 * 1000 * 1000)
#define STRIP_H      40

#define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

static const char *TAG = "term";

void app_main(void) {
	backlight_init(PIN_BL);
    settings_init();  // NVS y ajustes guardados: hacen falta antes de prender la pantalla

	spi_bus_config_t bus = { .sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI,
			.miso_io_num = PIN_MISO, .quadwp_io_num = -1, .quadhd_io_num = -1,
			.max_transfer_sz = LCD_H_RES * STRIP_H * sizeof(uint16_t), };
	ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

	esp_lcd_panel_io_handle_t io;
	esp_lcd_panel_io_spi_config_t io_cfg = { .dc_gpio_num = PIN_DC,
			.cs_gpio_num = PIN_CS, .pclk_hz = LCD_PCLK_HZ, .lcd_cmd_bits = 8,
			.lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10,
			.on_color_trans_done = term_on_trans_done, };
	ESP_ERROR_CHECK(
			esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t) LCD_HOST,
					&io_cfg, &io));

	esp_lcd_panel_handle_t panel;
	esp_lcd_panel_dev_config_t panel_cfg = { .reset_gpio_num = -1,
			.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16, };
	ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &panel_cfg, &panel));
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
	ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));        // landscape
	ESP_ERROR_CHECK (esp_lcd_panel_mirror( panel, true, true)); // ajustar según test
	ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
	term_init(panel);
	term_flush();            // limpia la pantalla antes de prender el backlight
    backlight_set(settings_get()->brightness);

	boot_init();
	bool booting = true;
	time_t last_clock = 0;
	TickType_t last = xTaskGetTickCount();
	int64_t acc_us = 0;
	int frames = 0;
	while (1) {
		int64_t t0 = esp_timer_get_time();
		if (booting) {
			if (boot_step()) {
				booting = false;
				con_init();
				con_type(CON_C_HI "nodo en linea\n" CON_C_DIM "esperando datos...\n" CON_C_TEXT);
				pages_init();
				pcstat_start();
				net_start();
			}
		} else {
            if (!pages_busy()) con_step();  // la consola espera durante la transicion
			pages_step();
			time_t now = time(NULL);
			if (now != last_clock) {
				last_clock = now;
				struct tm tm;
				localtime_r(&now, &tm);
				char hhmmss[9];
				if (tm.tm_year + 1900 >= 2025) {
					strftime(hhmmss, sizeof hhmmss, "%H:%M:%S", &tm);
				} else {
					strcpy(hhmmss, "--:--:--");
				}
				frame_status(hhmmss);
			}
		}
		term_flush();
		acc_us += esp_timer_get_time() - t0;
		if (++frames == 100) {
			ESP_LOGD(TAG, "frame medio: %lld us", acc_us / frames);
			mon_set_frame_us((int) (acc_us / frames));
			acc_us = 0;
			frames = 0;
		}
		xTaskDelayUntil(&last, pdMS_TO_TICKS(30));
	}
}

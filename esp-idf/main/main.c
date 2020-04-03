#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <sys/unistd.h>
#include <sys/stat.h>

//#include <esp_wifi.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>

//#include <esp_http_server.h>
#include "esp_camera.h"

//#include <rom/ets_sys.h>

#include "esp_intr_alloc.h"
#include "esp_attr.h"
#include "driver/timer.h"

#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

#define MOUNT_POINT "/sdcard"

#define SPI_DMA_CHAN    1

#define SERIAL_PIN (GPIO_NUM_14)
#define PTT_PIN (GPIO_NUM_15)

static const char *TAG = "foo";
volatile uint8_t* rgb_buf;
volatile uint16_t rgb_width;
volatile uint16_t rgb_height;

volatile uint8_t serial_buffer[64];

volatile uint32_t filenr=0;

static camera_config_t camera_config =
{
  .pin_pwdn = -1,
  .pin_reset = CONFIG_RESET,
  .pin_xclk = CONFIG_XCLK,
  .pin_sscb_sda = CONFIG_SDA,
  .pin_sscb_scl = CONFIG_SCL,

  .pin_d7 = CONFIG_D7,
  .pin_d6 = CONFIG_D6,
  .pin_d5 = CONFIG_D5,
  .pin_d4 = CONFIG_D4,
  .pin_d3 = CONFIG_D3,
  .pin_d2 = CONFIG_D2,
  .pin_d1 = CONFIG_D1,
  .pin_d0 = CONFIG_D0,
  .pin_vsync = CONFIG_VSYNC,
  .pin_href = CONFIG_HREF,
  .pin_pclk = CONFIG_PCLK,

  //XCLK 20MHz or 10MHz
  .xclk_freq_hz = CONFIG_XCLK_FREQ,
  .ledc_timer = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,

  .pixel_format = PIXFORMAT_JPEG, //YUV422,GRAYSCALE,RGB565,JPEG
  //.frame_size = FRAMESIZE_VGA,   //QQVGA-UXGA Do not use sizes above QVGA when not JPEG
  .frame_size = FRAMESIZE_UXGA,   //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

  .jpeg_quality = 2, //0-63 lower number means higher quality
  .fb_count = 1       //if more than one, i2s runs in continuous mode. Use only with JPEG
};

static esp_err_t init_camera()
{
  //initialize the camera
  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Camera Init Failed");
    return err;
  }

  return ESP_OK;
}

static intr_handle_t s_timer_handle;

void app_main()
{
  ESP_ERROR_CHECK(nvs_flash_init());
  init_camera();
  
  ledc_timer_config_t timer_conf;
  //timer_conf.bit_num = LEDC_TIMER_15_BIT;
  timer_conf.bit_num = 8; //1-10: numbers, 11-15: enums
  timer_conf.freq_hz = 15625;
  timer_conf.speed_mode = LEDC_HIGH_SPEED_MODE;
  timer_conf.timer_num = LEDC_TIMER_3;
  ledc_timer_config(&timer_conf);

  ledc_channel_config_t ledc_conf;
  ledc_conf.channel = LEDC_CHANNEL_1;
  ledc_conf.duty = 50;
  ledc_conf.gpio_num = 2;
  ledc_conf.intr_type = LEDC_INTR_DISABLE;
  ledc_conf.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_conf.timer_sel = LEDC_TIMER_3;
  ledc_channel_config(&ledc_conf);

  vTaskDelay(1000/portTICK_PERIOD_MS);

  //while(1)
  //{
  //	camera_fb_t *fb = NULL;
  //	fb = esp_camera_fb_get();
  //	//rgb_buf = (uint8_t*)malloc(fb->width*fb->height*3*sizeof(uint8_t));
  //	//rgb_width = fb->width;
  //	//rgb_height = fb->height;
  //	//fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
  //	esp_camera_fb_return(fb);vTaskDelay(90*1000/portTICK_PERIOD_MS);
  //  //ESP_LOGE(TAG, "DELAY vege");
  //}


  esp_err_t ret;
  // Options for mounting the filesystem.
  // If format_if_mount_failed is set to true, SD card will be partitioned and
  // formatted in case when mounting fails.
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 5,
      .allocation_unit_size = 16 * 1024
  };
  sdmmc_card_t* card;
  const char mount_point[] = MOUNT_POINT;
  ESP_LOGI(TAG, "Initializing SD card");

  // Use settings defined above to initialize SD card and mount FAT filesystem.
  // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
  // Please check its source code and implement error recovery when developing
  // production applications.
  
	ESP_LOGI(TAG, "Using SDMMC peripheral");
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();

  // This initializes the slot without card detect (CD) and write protect (WP) signals.
  // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  // To use 1-line SD mode, uncomment the following line:
  // slot_config.width = 1;

  // GPIOs 15, 2, 4, 12, 13 should have external 10k pull-ups.
  // Internal pull-ups are not sufficient. However, enabling internal pull-ups
  // does make a difference some boards, so we do that here.
  gpio_set_pull_mode(15, GPIO_PULLUP_ONLY);   // CMD, needed in 4- and 1- line modes
  gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);    // D0, needed in 4- and 1-line modes
  gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);    // D1, needed in 4-line mode only
  gpio_set_pull_mode(12, GPIO_PULLUP_ONLY);   // D2, needed in 4-line mode only
  gpio_set_pull_mode(13, GPIO_PULLUP_ONLY);   // D3, needed in 4- and 1-line modes

  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK)
	{
    return;
  }

  sdmmc_card_print_info(stdout, card);

	char fname[32];
	struct stat st;
	while(1)
	{
		snprintf(fname, 64, "%s/p%07d.jpg", MOUNT_POINT, filenr);
    if (stat(fname, &st) != 0)
		{
  		ESP_LOGI(TAG, "Counter %d", filenr);
			break;
		}
		filenr++;
	}
  
	ESP_LOGI(TAG, "Final counter %d", filenr);
	
	while(1)
	{
		snprintf(fname, 64, "%s/p%07d.jpg", MOUNT_POINT, filenr);
  	ESP_LOGI(TAG, "Fname %s", fname);
  	FILE* f = fopen(fname, "wb");
  	if (f == NULL)
		{
  		ESP_LOGI(TAG, "Cannot open file.");
  	  return;
  	}

  	camera_fb_t *fb = NULL;
  	fb = esp_camera_fb_get();
  	//rgb_buf = (uint8_t*)malloc(fb->width*fb->height*3*sizeof(uint8_t));
  	//rgb_width = fb->width;
  	//rgb_height = fb->height;
  	//fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
		fwrite(fb->buf, 1, fb->len, f);
  	esp_camera_fb_return(fb);
		vTaskDelay(5*1000/portTICK_PERIOD_MS);
  	//ESP_LOGE(TAG, "DELAY vege");
		
  	//fprintf(f, "Hello %s!\n", card->cid.name);
  	fclose(f);
  	ESP_LOGI(TAG, "File written");
		filenr++;
	}

  esp_vfs_fat_sdmmc_unmount(mount_point, card);
  ESP_LOGI(TAG, "Card unmounted");
	
	while(1);
}

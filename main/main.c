#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "esp_http_client.h"
#include "connect_wifi.h"
#include "bme280.h"
#include "driver/i2c.h"

static const char *TAG = "HTTP_CLIENT";
char api_key[] = "replace_with_your_api_key";

#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22

#define TAG_BME280 "BME280"

#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

double temp;
double press;
double hum;

void i2c_master_init()
{
	i2c_config_t i2c_config = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = SDA_PIN,
		.scl_io_num = SCL_PIN,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 1000000};
	i2c_param_config(I2C_NUM_0, &i2c_config);
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}

s8 BME280_I2C_bus_write(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
	s32 iError = BME280_INIT_VALUE;

	esp_err_t espRc;
	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);

	i2c_master_write_byte(cmd, reg_addr, true);
	i2c_master_write(cmd, reg_data, cnt, true);
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
	if (espRc == ESP_OK)
	{
		iError = SUCCESS;
	}
	else
	{
		iError = FAIL;
	}
	i2c_cmd_link_delete(cmd);

	return (s8)iError;
}

s8 BME280_I2C_bus_read(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
{
	s32 iError = BME280_INIT_VALUE;
	esp_err_t espRc;

	i2c_cmd_handle_t cmd = i2c_cmd_link_create();

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
	i2c_master_write_byte(cmd, reg_addr, true);

	i2c_master_start(cmd);
	i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);

	if (cnt > 1)
	{
		i2c_master_read(cmd, reg_data, cnt - 1, I2C_MASTER_ACK);
	}
	i2c_master_read_byte(cmd, reg_data + cnt - 1, I2C_MASTER_NACK);
	i2c_master_stop(cmd);

	espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10 / portTICK_PERIOD_MS);
	if (espRc == ESP_OK)
	{
		iError = SUCCESS;
	}
	else
	{
		iError = FAIL;
	}

	i2c_cmd_link_delete(cmd);

	return (s8)iError;
}

void BME280_delay_msek(u32 msek)
{
	vTaskDelay(msek / portTICK_PERIOD_MS);
}

void bme280_get_readings(void *ignore)
{
	struct bme280_t bme280 = {
		.bus_write = BME280_I2C_bus_write,
		.bus_read = BME280_I2C_bus_read,
		.dev_addr = BME280_I2C_ADDRESS1,
		.delay_msec = BME280_delay_msek};

	s32 com_rslt;
	s32 v_uncomp_pressure_s32;
	s32 v_uncomp_temperature_s32;
	s32 v_uncomp_humidity_s32;

	com_rslt = bme280_init(&bme280);

	com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
	com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_2X);
	com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_1X);

	com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1_MS);
	com_rslt += bme280_set_filter(BME280_FILTER_COEFF_16);

	com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);
	if (com_rslt == SUCCESS)
	{
		while (true)
		{
			vTaskDelay(4000 / portTICK_PERIOD_MS);

			com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
				&v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);

			if (com_rslt == SUCCESS)
			{
				ESP_LOGI(TAG_BME280, "%.2f degC / %.3f hPa / %.3f %%",
						 bme280_compensate_temperature_double(v_uncomp_temperature_s32),
						 bme280_compensate_pressure_double(v_uncomp_pressure_s32) / 100, // Pa -> hPa
						 bme280_compensate_humidity_double(v_uncomp_humidity_s32));
				temp = bme280_compensate_temperature_double(v_uncomp_temperature_s32);
				press = bme280_compensate_pressure_double(v_uncomp_pressure_s32) / 100;
				hum = bme280_compensate_humidity_double(v_uncomp_humidity_s32);
			}
			else
			{
				ESP_LOGE(TAG_BME280, "measure error. code: %d", com_rslt);
			}
		}
	}
	else
	{
		ESP_LOGE(TAG_BME280, "init or setting error. code: %d", com_rslt);
	}

	vTaskDelete(NULL);
}

void send_data_to_thingspeak(void *pvParameters)
{

	char thingspeak_url[] = "https://api.thingspeak.com";
	char data[] = "/update?api_key=%s&field1=%.2f&field2=%.2f&field3=%.2f";
	char post_data[200];
	esp_err_t err;

	esp_http_client_config_t config = {
		.url = thingspeak_url,
		.method = HTTP_METHOD_GET,
	};
	esp_http_client_handle_t client = esp_http_client_init(&config);
	esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
	while (1)
	{
		vTaskDelay(5000 / portTICK_RATE_MS);
		strcpy(post_data, "");
		snprintf(post_data, sizeof(post_data), data, api_key, temp, press, hum);
		ESP_LOGI(TAG, "post = %s", post_data);
		esp_http_client_set_post_field(client, post_data, strlen(post_data));

		err = esp_http_client_perform(client);

		if (err == ESP_OK)
		{
			int status_code = esp_http_client_get_status_code(client);
			if (status_code == 200)
			{
				ESP_LOGI(TAG, "Message sent Successfully");
			}
			else
			{
				ESP_LOGI(TAG, "Message sent Failed");				
				goto exit;
			}
		}
		else
		{
			ESP_LOGI(TAG, "Message sent Failed");
			goto exit;
		}
	}
exit:
	esp_http_client_cleanup(client);
	vTaskDelete(NULL);
}

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	i2c_master_init();
	connect_wifi();
	if (wifi_connect_status)
	{
		xTaskCreate(&bme280_get_readings, "bme280_get_readings", 2048, NULL, 5, NULL);
		xTaskCreate(&send_data_to_thingspeak, "send_data_to_thingspeak", 8192, NULL, 6, NULL);
	}
}

/*
WiCAN
*/

static const char *DEBUG_TAG = "WiCAN";

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

// WiFi
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include <lwip/netdb.h>
#include "wifi_manager.h"
#include "mdns.h"

// CAN
#include "driver/gpio.h"
#include "driver/twai.h"

// SD
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

// SD Card CS pin is GPIO2; SD.begin(2) SD Card MISO = GPIO19, MOSI = GPIO23
#define PIN_NUM_MISO  19
#define PIN_NUM_MOSI  23
#define PIN_NUM_CLK   18
#define PIN_NUM_CS    2

#include "nmea2000.h"

QueueHandle_t can_queue;
QueueHandle_t logger_queue;
volatile bool clear_log;

bool have_valid_ip = false;
uint8_t green_led_status = 0;

// Logged parameters
typedef struct logger_params_t
{
	float engine1_speed;
	float engine2_speed;
	float engine3_speed;
	float engine4_speed;
	uint8_t engine1_load_percent;
	uint8_t engine2_load_percent;
	uint8_t engine3_load_percent;
	uint8_t engine4_load_percent;
	uint8_t engine1_torque_percent;
	uint8_t engine2_torque_percent;
	uint8_t engine3_torque_percent;
	uint8_t engine4_torque_percent;
	float speed_over_ground;
	uint32_t gps_date;
	float gps_time;
	float lat;
	float lon;
	float depth;
	uint8_t temperature_instance;
	uint8_t temperature_source;
	float temperature;
	uint32_t total_distance;
} logger_params_t;



/////////////////////////////////////////////////////////////////
// CAN Functions
/////////////////////////////////////////////////////////////////

void task_CAN(void *pvParameters)
{	
	twai_message_t message;
	logger_params_t logged;
	uint32_t pgn;
	
	uint32_t gnss_pos_data_id = 0;
	uint8_t gnss_pos_data_buf[52];
	uint8_t gnss_pos_data_len = 0;
	
	uint32_t engine_data_id = 0;
	uint8_t engine_data_buf[52];
	uint8_t engine_data_len = 0;
	
	uint32_t distance_log_id = 0;
	uint8_t distance_log_buf[15];
	uint8_t distance_log_len = 0;
	
	logged.engine1_speed = 0;
	logged.engine2_speed = 0;
	logged.engine3_speed = 0;
	logged.engine4_speed = 0;
	logged.engine1_load_percent = 0;
	logged.engine2_load_percent = 0;
	logged.engine3_load_percent = 0;
	logged.engine4_load_percent = 0;
	logged.engine1_torque_percent = 0;
	logged.engine2_torque_percent = 0;
	logged.engine3_torque_percent = 0;
	logged.engine4_torque_percent = 0;
	logged.speed_over_ground = 0;
	logged.gps_date = 0;
	logged.gps_time = 0;
	logged.lat = 0;
	logged.lon = 0;
	logged.depth = 0;
	logged.temperature_instance = 0;
	logged.temperature_source = 0;
	logged.temperature = 0;
	logged.total_distance = 0;
	
    while (1)
	{
        //Wait for message to be received
		if (twai_receive(&message, pdMS_TO_TICKS(1000)) != ESP_OK) 
		{
			ESP_LOGE(DEBUG_TAG, "Failed to receive message\n");
			continue;
		}
		
		ESP_LOGI(DEBUG_TAG, "Receive message\n");
		
		if (!(message.rtr) || true)
		{
			// Put the packet in the queue for the TCP thread
			if (can_queue != NULL)
			{
				//ESP_LOGI(DEBUG_TAG, "Receive message\n");
				if (uxQueueSpacesAvailable(can_queue) > 5)
					xQueueSend(can_queue, &message, pdMS_TO_TICKS(1));
			}
			
			pgn = (message.identifier >> 8) & 0x3FFFF;
			//printf("pgn: %x\n", pgn);
			
			// Engine Parameters Rapid Update 0x19F200FE
			if (pgn == 0x1F200)
			{
				struct nmea2000_engine_parameters_rapid_update_t engine;
				
				nmea2000_engine_parameters_rapid_update_unpack(&engine, message.data, NMEA2000_ENGINE_PARAMETERS_RAPID_UPDATE_LENGTH);

				if (engine.engine_instance == 0)
					logged.engine1_speed = nmea2000_engine_parameters_rapid_update_engine_speed_decode(engine.engine_speed);
				else if (engine.engine_instance == 1)
					logged.engine2_speed = nmea2000_engine_parameters_rapid_update_engine_speed_decode(engine.engine_speed);
				else if (engine.engine_instance == 2)
					logged.engine3_speed = nmea2000_engine_parameters_rapid_update_engine_speed_decode(engine.engine_speed);
				else if (engine.engine_instance == 3)
					logged.engine4_speed = nmea2000_engine_parameters_rapid_update_engine_speed_decode(engine.engine_speed);
				else
					continue; // only support 4 engines
				
				xQueueOverwrite(logger_queue, &logged);
			}
			
			// EngineParametersDynamic 0x19F201FE
			if (pgn == 0x1F201)
			{
				bool done = false;
				
				// Make sure we are reassembling the packet from the same sender
				if (engine_data_id != 0 && engine_data_id != message.identifier)
					continue;
				
				uint8_t seq = message.data[0] & 0x0F;
				
				if (seq == 0)
				{
					engine_data_id = message.identifier;
					engine_data_len = message.data[1];
					
					engine_data_buf[0] = message.data[2];
					engine_data_buf[1] = message.data[3];
					engine_data_buf[2] = message.data[4];
					engine_data_buf[3] = message.data[5];
					engine_data_buf[4] = message.data[6];
					engine_data_buf[5] = message.data[7];
				}
				else if (engine_data_len > 0)
				{
					uint8_t offset = 6 + ((seq - 1) * 7);
					
					int i;
					for (i=0; i<6; i++)
					{
						engine_data_buf[offset] = message.data[i+1];
						offset++;
						if (offset > engine_data_len)
						{
							done = true;
							engine_data_id = 0;
							break;
						}
					}
				}
				
				if (done)
				{
					struct nmea2000_engine_parameters_dynamic_t engine;
					
					nmea2000_engine_parameters_dynamic_unpack(&engine, engine_data_buf, NMEA2000_ENGINE_PARAMETERS_DYNAMIC_LENGTH);
					
					if (engine.engine_instance == 0)
					{
						logged.engine1_load_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_load);
						logged.engine1_torque_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_torque);
					}
					else if (engine.engine_instance == 1)
					{
						logged.engine2_load_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_load);
						logged.engine2_torque_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_torque);
					}
					else if (engine.engine_instance == 2)
					{
						logged.engine3_load_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_load);
						logged.engine3_torque_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_torque);
					}
					else if (engine.engine_instance == 3)
					{
						logged.engine4_load_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_load);
						logged.engine4_torque_percent = nmea2000_engine_parameters_dynamic_percent_engine_load_decode(engine.percent_engine_torque);
					}
					else
						continue; // only 4 engines
					
					xQueueOverwrite(logger_queue, &logged);
				}
			}
			
			//COGSSOGRapidUpdate 0x09F802
			if (pgn == 0x1F802)
			{
				struct nmea2000_cogsog_rapid_update_t cog_sog;
				
				nmea2000_cogsog_rapid_update_unpack(&cog_sog, message.data, NMEA2000_COGSOG_RAPID_UPDATE_LENGTH);

				logged.speed_over_ground = nmea2000_cogsog_rapid_update_speed_over_ground_decode(cog_sog.speed_over_ground);
				
				xQueueOverwrite(logger_queue, &logged);
			}
			
			//WaterDepth 0x19F50BFE
			if (pgn == 0x1F50B)
			{
				struct nmea2000_water_depth_t water_depth;
				
				nmea2000_water_depth_unpack(&water_depth, message.data, NMEA2000_WATER_DEPTH_LENGTH);

				float max_depth_m = nmea2000_water_depth_maximum_depth_range_decode(water_depth.maximum_depth_range);
				float offset_m = nmea2000_water_depth_offset_decode(water_depth.offset);
				float depth_at_transducer_m = nmea2000_water_depth_water_depth_transducer_decode(water_depth.water_depth_transducer);
				
				depth_at_transducer_m += offset_m;
				
				if (water_depth.maximum_depth_range < 253)
				{
					if (max_depth_m < depth_at_transducer_m)
						depth_at_transducer_m = max_depth_m;
				}
				
				logged.depth = depth_at_transducer_m;
				
				xQueueOverwrite(logger_queue, &logged);
			}
			
			// TemperatureExtendedRange 0x19FD0CFE
			if (pgn == 0x1FD0C)
			{
				struct nmea2000_temperature_extended_range_t temp;
				
				nmea2000_temperature_extended_range_unpack(&temp, message.data, NMEA2000_TEMPERATURE_EXTENDED_RANGE_LENGTH);

				logged.temperature_instance = temp.temperature_instance;
				logged.temperature_source = temp.temperature_source;
				logged.temperature = nmea2000_temperature_extended_range_actual_temperature_decode(temp.actual_temperature)-273;
				
				xQueueOverwrite(logger_queue, &logged);
			}
			
			
			//GNSSPositionData 0x0DF805
			if (pgn == 0x1F805)
			{
				bool done = false;
				
				// Make sure we are reassembling the packet from the same sender
				if (gnss_pos_data_id != 0 && gnss_pos_data_id != message.identifier)
					continue;
				
				uint8_t seq = message.data[0] & 0x0F;
				
				if (seq == 0)
				{
					gnss_pos_data_id = message.identifier;
					gnss_pos_data_len = message.data[1];
					
					gnss_pos_data_buf[0] = message.data[2];
					gnss_pos_data_buf[1] = message.data[3];
					gnss_pos_data_buf[2] = message.data[4];
					gnss_pos_data_buf[3] = message.data[5];
					gnss_pos_data_buf[4] = message.data[6];
					gnss_pos_data_buf[5] = message.data[7];
				}
				else if (gnss_pos_data_len > 0)
				{
					uint8_t offset = 6 + ((seq - 1) * 7);
					
					int i;
					for (i=0; i<6; i++)
					{
						gnss_pos_data_buf[offset] = message.data[i+1];
						offset++;
						if (offset > gnss_pos_data_len)
						{
							done = true;
							gnss_pos_data_id = 0;
							break;
						}
					}
				}
				
				if (done)
				{
					struct nmea2000_gnss_position_data_t gnss;
					
					nmea2000_gnss_position_data_unpack(&gnss, gnss_pos_data_buf, NMEA2000_GNSS_POSITION_DATA_LENGTH);

					logged.gps_date = gnss.position_date;
					logged.gps_time = nmea2000_gnss_position_data_position_time_decode(gnss.position_time);
					logged.lat = nmea2000_gnss_position_data_latitude_decode(gnss.latitude);
					logged.lon = nmea2000_gnss_position_data_longitude_decode(gnss.longitude);
					
					xQueueOverwrite(logger_queue, &logged);
				}
			}
			
			//DistanceLog 0x19F513FE
			if (pgn == 0x1F513)
			{
				bool done = false;
				
				// Make sure we are reassembling the packet from the same sender
				if (distance_log_id != 0 && distance_log_id != message.identifier)
					continue;
				
				uint8_t seq = message.data[0] & 0x0F;
				
				if (seq == 0)
				{
					distance_log_id = message.identifier;
					distance_log_len = message.data[1];
					
					distance_log_buf[0] = message.data[2];
					distance_log_buf[1] = message.data[3];
					distance_log_buf[2] = message.data[4];
					distance_log_buf[3] = message.data[5];
					distance_log_buf[4] = message.data[6];
					distance_log_buf[5] = message.data[7];
				}
				else if (distance_log_len > 0)
				{
					uint8_t offset = 6 + ((seq - 1) * 7);
					
					int i;
					for (i=0; i<6; i++)
					{
						distance_log_buf[offset] = message.data[i+1];
						offset++;
						if (offset > distance_log_len)
						{
							done = true;
							distance_log_id = 0;
							break;
						}
					}
				}
				
				if (done)
				{
					struct nmea2000_distance_log_t dist;
					
					nmea2000_distance_log_unpack(&dist, distance_log_buf, NMEA2000_DISTANCE_LOG_LENGTH);

					logged.total_distance = dist.total_cumulative_distance;
					
					xQueueOverwrite(logger_queue, &logged);
				}
			}
		}
		//vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

/////////////////////////////////////////////////////////////////
// TCPIP Functions
/////////////////////////////////////////////////////////////////

void cb_connection_ok(void *pvParameter)
{
	ESP_LOGI(DEBUG_TAG, "Setting hostname");
	
    esp_err_t err = mdns_init();
    if (err) 
        printf("MDNS Init failed: %d\n", err);
	else
	{
		mdns_hostname_set("photon");
		mdns_instance_name_set("Photon Logger");
	}
	
	have_valid_ip = true;
}

static void tcp_server_task(void *pvParameters)
{
    char addr_str[128];
    int addr_family;
    int ip_protocol;
	
	struct sockaddr_in dest_addr;
	dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(8080);
	addr_family = AF_INET;
	ip_protocol = IPPROTO_IP;
	inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

	while (!have_valid_ip)
	{
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}
	
	int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
	if (listen_sock < 0) 
	{
		ESP_LOGE(DEBUG_TAG, "Unable to create socket: errno %d", errno);
		return;
	}
	ESP_LOGI(DEBUG_TAG, "Socket created");

	int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) 
	{
		ESP_LOGE(DEBUG_TAG, "Socket unable to bind: errno %d", errno);
		return;
	}
	ESP_LOGI(DEBUG_TAG, "Socket bound, port 8080");

	err = listen(listen_sock, 1);
	if (err != 0) 
	{
		ESP_LOGE(DEBUG_TAG, "Error occurred during listen: errno %d", errno);
		return;
	}
	ESP_LOGI(DEBUG_TAG, "Socket listening");
	
	while (1)
	{
		struct sockaddr_in source_addr;
		uint addr_len = sizeof(source_addr);
		int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
		if (sock < 0)
		{
			ESP_LOGE(DEBUG_TAG, "Unable to accept connection: errno %d", errno);
			break;
		}
		ESP_LOGI(DEBUG_TAG, "Socket accepted");
		
		inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
		
		twai_message_t message;
		char buffer[19];
		uint32_t msecs;
		while (1)
		{
			if (can_queue != NULL && xQueueReceive(can_queue, &message, portMAX_DELAY))
			{
				msecs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
				buffer[0] = 0xAA;
				buffer[1] = msecs;
				buffer[2] = msecs >> 8;
				buffer[3] = msecs >> 16;
				buffer[4] = msecs >> 24;
				buffer[5] = message.data_length_code;
				buffer[6] = message.identifier;
				buffer[7] = message.identifier >> 8;
				buffer[8] = message.identifier >> 16;
				buffer[9] = message.identifier >> 24;
				for (int i=0; i < message.data_length_code; i++)
					buffer[10+i] = message.data[i];
				buffer[10+message.data_length_code] = 0xBB;
				
				int err = send(sock, buffer, 11+message.data_length_code, 0);
				if (err < 0) 
				{
					ESP_LOGE(DEBUG_TAG, "Error occurred during sending: errno %d", errno);
					break;
				}
			}
			vTaskDelay(5 / portTICK_PERIOD_MS);
		}

		if (sock != -1) 
		{
			ESP_LOGE(DEBUG_TAG, "Shutting down socket and restarting...");
			shutdown(sock, 0);
			close(sock);
		}
	}

	vTaskDelete(NULL);
}

/////////////////////////////////////////////////////////////////
// Logger Functions
/////////////////////////////////////////////////////////////////

void logger_task(void *pvParameters)
{
	logger_params_t logged;
	esp_err_t ret;
	const char *file_hello = "/sdcard/log.csv";
	FILE *f = NULL;
	
	ESP_LOGI(DEBUG_TAG, "Starting logger thread");

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024
	};
	sdmmc_card_t *card;
	const char mount_point[] = "/sdcard";
	ESP_LOGI(DEBUG_TAG, "Initializing SD card");
	
	
	/*
	ESP_LOGI(DEBUG_TAG, "Using SPI peripheral");

	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	spi_bus_config_t bus_cfg = {
		.mosi_io_num = PIN_NUM_MOSI,
		.miso_io_num = PIN_NUM_MISO,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
	if (ret != ESP_OK) 
	{
		ESP_LOGE(DEBUG_TAG, "Failed to initialize bus.");
		while(1) {vTaskDelay(500 / portTICK_PERIOD_MS);}
	}

	// This initializes the slot without card detect (CD) and write protect (WP) signals.
	// Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = PIN_NUM_CS;
	slot_config.host_id = host.slot;
	*/
	
	
	
	
	
	ESP_LOGI(DEBUG_TAG, "Using SDMMC peripheral");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 40MHz for SDMMC)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
	#define SOC_SDMMC_USE_GPIO_MATRIX true
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
	
	
	
	
	

	ESP_LOGI(DEBUG_TAG, "Mounting filesystem");
	ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK) 
	{
		if (ret == ESP_FAIL) 
		{
			ESP_LOGE(DEBUG_TAG, "Failed to mount filesystem.");
		} 
		else 
		{
			ESP_LOGE(DEBUG_TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
		}
		while(1) {vTaskDelay(500 / portTICK_PERIOD_MS);}
	}
	ESP_LOGI(DEBUG_TAG, "Filesystem mounted");

	// Card has been initialized, print its properties
	sdmmc_card_print_info(stdout, card);
		
	while (1)
	{
		// Open the file
		ESP_LOGI(DEBUG_TAG, "Opening file %s", file_hello);
		f = fopen(file_hello, "a");
		if (f == NULL) 
		{
			ESP_LOGE(DEBUG_TAG, "Failed to open file for writing");
			while(1) {vTaskDelay(500 / portTICK_PERIOD_MS);}
		}
	
		while (1)
		{
			if (logger_queue != NULL && xQueueReceive(logger_queue, &logged, pdMS_TO_TICKS(10)))
			{
				// Get latest data
			}
			
			// date, time, lat, lon, speed, rpm
			if (f != NULL)
			{
				fprintf(f, "%i,%f,%f,%f,%f,%ul,%f,%f,%f,%f,%i,%i,%i,%i,%i,%i,%i,%i,%f,%i:%i:%f\n", 
					logged.gps_date, 
					logged.gps_time, 
					logged.lat, 
					logged.lon, 
					logged.speed_over_ground, 
					logged.total_distance,
					logged.engine1_speed,
					logged.engine2_speed,
					logged.engine3_speed,
					logged.engine4_speed,
					logged.engine1_load_percent,
					logged.engine2_load_percent,
					logged.engine3_load_percent,
					logged.engine4_load_percent,
					logged.engine1_torque_percent,
					logged.engine2_torque_percent,
					logged.engine3_torque_percent,
					logged.engine4_torque_percent,
					logged.depth,
					logged.temperature_instance,
					logged.temperature_source,
					logged.temperature
					);
				
				
				fflush(f);
				fsync(fileno(f));
			}
			
			if (clear_log)
			{
				fclose(f);
				f = fopen(file_hello, "w");
				fflush(f);
				fsync(fileno(f));
				fclose(f);
				
				clear_log = false;
				
				break;
			}
			
			green_led_status = 1 - green_led_status;
			gpio_set_level(GPIO_NUM_16, green_led_status);
			
			vTaskDelay(500 / portTICK_PERIOD_MS);
		}
	}
}

/////////////////////////////////////////////////////////////////
// Main
/////////////////////////////////////////////////////////////////

void app_main()
{
	esp_err_t ret;
	
	can_queue = xQueueCreate(10, sizeof(twai_message_t));
	logger_queue = xQueueCreate(1, sizeof(logger_params_t));
	
	// Setup wifi manager to manage saved SSIDs
	//wifi_manager_start();
	//wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);
	
    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
	{
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
	
	// Start CAN
    ESP_LOGI(DEBUG_TAG, "Starting CAN");
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_21, GPIO_NUM_22, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

	//Install TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) 
	{
        ESP_LOGI(DEBUG_TAG, "CAN driver installed\n");
    } 
	else 
	{
        ESP_LOGE(DEBUG_TAG, "Failed to install CAN driver\n");
    }

    //Start TWAI driver
    if (twai_start() == ESP_OK) 
	{
        ESP_LOGI(DEBUG_TAG, "CAN driver started\n");
    } 
	else 
	{
        ESP_LOGE(DEBUG_TAG, "Failed to start CAN driver\n");
    }
	
	
	gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
	gpio_set_level(GPIO_NUM_16, green_led_status);
	
	//SD Card must be ejected while programming. 
	
	xTaskCreate(&task_CAN, "CAN", 4096, NULL, 5, NULL);
	xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
	xTaskCreate(&logger_task, "logger", 4096, NULL, 5, NULL);
}

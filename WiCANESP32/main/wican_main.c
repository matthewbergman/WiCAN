/*
WiCAN
*/

static const char *TAG = "WiCAN";

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

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
#include "driver/can.h"

QueueHandle_t can_queue;
bool have_valid_ip = false;

/////////////////////////////////////////////////////////////////
// CAN Functions
/////////////////////////////////////////////////////////////////

void task_CAN(void *pvParameters)
{	
	can_message_t message;
    while (1)
	{
        //Wait for message to be received
		if (can_receive(&message, pdMS_TO_TICKS(10000)) != ESP_OK) 
		{
			ESP_LOGE(TAG, "Failed to receive message\n");
			continue;
		}
		
		if (!(message.flags & CAN_MSG_FLAG_RTR)) 
		{
			if (can_queue != NULL)
				xQueueOverwrite(can_queue, &message);
		}
		vTaskDelay(5 / portTICK_PERIOD_MS);
    }
}

void cb_connection_ok(void *pvParameter)
{
	ESP_LOGI(TAG, "Setting hostname");
	
	//initialize mDNS service
    esp_err_t err = mdns_init();
    if (err) 
	{
        printf("MDNS Init failed: %d\n", err);
    }
	else
	{
		mdns_hostname_set("wican");
		mdns_instance_name_set("WiCAN Adapter");
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
		ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
		return;
	}
	ESP_LOGI(TAG, "Socket created");

	int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) 
	{
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
		return;
	}
	ESP_LOGI(TAG, "Socket bound, port 8080");

	err = listen(listen_sock, 1);
	if (err != 0) 
	{
		ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
		return;
	}
	ESP_LOGI(TAG, "Socket listening");
	
	while (1)
	{
		struct sockaddr_in source_addr;
		uint addr_len = sizeof(source_addr);
		int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
		if (sock < 0)
		{
			ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
			break;
		}
		ESP_LOGI(TAG, "Socket accepted");
		
		inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
		
		can_message_t message;
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
					ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
					break;
				}
			}
			vTaskDelay(5 / portTICK_PERIOD_MS);
		}

		if (sock != -1) 
		{
			ESP_LOGE(TAG, "Shutting down socket and restarting...");
			shutdown(sock, 0);
			close(sock);
		}
	}

	vTaskDelete(NULL);
}

void app_main()
{
	// Queue length must be 1 for the overwrite function
	can_queue = xQueueCreate(1, sizeof(can_message_t));
	
	// Setup wifi manager to manage saved SSIDs
	wifi_manager_start();
	wifi_manager_set_callback(EVENT_STA_GOT_IP, &cb_connection_ok);
	
	// Start CAN
    ESP_LOGI(TAG, "Starting CAN");
    can_general_config_t g_config = CAN_GENERAL_CONFIG_DEFAULT(GPIO_NUM_5, GPIO_NUM_4, CAN_MODE_NORMAL);
    can_timing_config_t t_config = CAN_TIMING_CONFIG_500KBITS();
    can_filter_config_t f_config = CAN_FILTER_CONFIG_ACCEPT_ALL();

    //Install CAN driver
    if (can_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
	{
        ESP_LOGE(TAG, "Failed to install CAN driver\n");
        return;
    }

    //Start CAN driver
    if (can_start() != ESP_OK) 
	{
        ESP_LOGE(TAG, "Failed to start CAN driver\n");
        return;
    }
	
	xTaskCreate(&task_CAN, "CAN", 2048, NULL, 5, NULL);
	xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}

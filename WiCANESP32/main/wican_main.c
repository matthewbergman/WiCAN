/*
WiCAN
*/

static const char *DEBUG_TAG = "WiCAN";

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

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
#include "driver/can.h"

// BLE
#include "ble_defs.h"

QueueHandle_t can_queue;

#define BLE_BUF_SIZE 32
unsigned char ble_buf[BLE_BUF_SIZE];
void parse_ble_packet();
uint32_t old_can_id = 0;

bool have_valid_ip = false;

/////////////////////////////////////////////////////////////////
// BLE Functions
/////////////////////////////////////////////////////////////////
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) 
	{
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~ADV_CONFIG_FLAG);
            if (adv_config_done == 0)
                esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            if (adv_config_done == 0)
                esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            /* advertising start complete event to indicate advertising start successfully or failed */
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
                ESP_LOGE(DEBUG_TAG, "advertising start failed");
            else
                ESP_LOGI(DEBUG_TAG, "advertising start successfully");
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
                ESP_LOGE(DEBUG_TAG, "Advertising stop failed");
            else
                ESP_LOGI(DEBUG_TAG, "Stop adv successfully\n");
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(DEBUG_TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
            break;
        default:
            break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
	esp_err_t set_dev_name_ret;
	
    switch (event) 
	{
        case ESP_GATTS_REG_EVT:
            set_dev_name_ret = esp_ble_gap_set_device_name(ESP_DEVICE_NAME);
            if (set_dev_name_ret)
                ESP_LOGE(DEBUG_TAG, "set device name failed, error code = %x", set_dev_name_ret);
            esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
            if (ret)
                ESP_LOGE(DEBUG_TAG, "config adv data failed, error code = %x", ret);
            adv_config_done |= ADV_CONFIG_FLAG;
            //config scan response data
            ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
            if (ret)
                ESP_LOGE(DEBUG_TAG, "config scan response data failed, error code = %x", ret);
            adv_config_done |= SCAN_RSP_CONFIG_FLAG;
            esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, HRS_IDX_NB, SVC_INST_ID);
            if (create_attr_ret)
                ESP_LOGE(DEBUG_TAG, "create attr table failed, error code = %x", create_attr_ret);
       	    break;
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(DEBUG_TAG, "ESP_GATTS_READ_EVT");
       	    break;
        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) 
			{
                // the data length of gattc write  must be less than CHAR_VAL_LEN_MAX.
                ESP_LOGI(DEBUG_TAG, "GATT_WRITE_EVT, handle = %d, value len = %d", param->write.handle, param->write.len);
                esp_log_buffer_hex(DEBUG_TAG, param->write.value, param->write.len);
				
                if (ble_handle_table[IDX_CHAR_CFG_CAN] == param->write.handle) 
				{
					if (param->write.len == 2) 
					{
						uint16_t descr_value = param->write.value[1]<<8 | param->write.value[0];
						char_can_notify_enabled = false;
						if (descr_value == 0x0001)
						{
							char_can_notify_enabled = true;
							ESP_LOGI(DEBUG_TAG, "bms notify enable");
							
							uint8_t indicate_data[15];
							for (int i = 0; i < sizeof(indicate_data); ++i)
								indicate_data[i] = 0;
							//the size of indicate_data[] need less than MTU size
							esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, ble_handle_table[IDX_CHAR_VAL_CAN],
												sizeof(indicate_data), indicate_data, false);
						}
						else if (descr_value == 0x0002)
						{
							ESP_LOGI(DEBUG_TAG, "bms indicate enable");
							
							uint8_t indicate_data[15];
							for (int i = 0; i < sizeof(indicate_data); ++i)
								indicate_data[i] = 0;
							//the size of indicate_data[] need less than MTU size
							esp_ble_gatts_send_indicate(gatts_if, param->write.conn_id, ble_handle_table[IDX_CHAR_VAL_CAN],
												sizeof(indicate_data), indicate_data, true);
						}
						else if (descr_value == 0x0000)
						{
							ESP_LOGI(DEBUG_TAG, "bms notify/indicate disable ");
						}
						else
						{
							ESP_LOGE(DEBUG_TAG, "bms unknown descr value");
							esp_log_buffer_hex(DEBUG_TAG, param->write.value, param->write.len);
						}
					} 
					else 
					{
						
					}
				}
				if (ble_handle_table[IDX_CHAR_VAL_CAN] == param->write.handle) 
				{
					// regular data, parse it as a command from the client app
					printf("BLE packet from the APP, pass to CAN bus\n");
					esp_log_buffer_hex(DEBUG_TAG, param->write.value, param->write.len);
					
					if (param->write.len > 5)
					{
						memset(ble_buf, 0, BLE_BUF_SIZE);
						for (int i=0; i<param->write.len; i++)
							ble_buf[i] = param->write.value[i];
							
						parse_ble_packet();
					}
				}
				else 
				{
					printf("CHAR_CAN %d\n", ble_handle_table[IDX_CHAR_CAN]);
					printf("CHAR_VAL_CAN %d\n", ble_handle_table[IDX_CHAR_VAL_CAN]);
					printf("CHAR_CFG_CAN %d\n", ble_handle_table[IDX_CHAR_CFG_CAN]);
					
					printf("Unknown write.handle\n");
				}
				
                /* send response when param->write.need_rsp is true*/
                if (param->write.need_rsp)
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
            }
			else
                printf("BLE PREPARE WRITES NOT SUPPORTED!\n");
      	    break;
        case ESP_GATTS_EXEC_WRITE_EVT: 
            // the length of gattc prapare write data must be less than CHAR_VAL_LEN_MAX. 
            ESP_LOGI(DEBUG_TAG, "ESP_GATTS_EXEC_WRITE_EVT NOT SUPPORTED");
            break;
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(DEBUG_TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
            break;
        case ESP_GATTS_CONF_EVT:
			// WHY are we getting these?
            //ESP_LOGI(DEBUG_TAG, "ESP_GATTS_CONF_EVT, status = %d, attr_handle %d", param->conf.status, param->conf.handle);
            break;
        case ESP_GATTS_START_EVT:
            ESP_LOGI(DEBUG_TAG, "SERVICE_START_EVT, status %d, service_handle %d", param->start.status, param->start.service_handle);
            break;
        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(DEBUG_TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
            esp_log_buffer_hex(DEBUG_TAG, param->connect.remote_bda, 6);
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            /* For the iOS system, please refer to Apple official documents about the BLE connection parameters restrictions. */
            conn_params.latency = 0;
            conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
            conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
            conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
            //start sent the update connection parameters to the peer device.
            esp_ble_gap_update_conn_params(&conn_params);
			
			spp_conn_id = param->connect.conn_id;
    	    spp_gatts_if = gatts_if;
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(DEBUG_TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
            esp_ble_gap_start_advertising(&adv_params);
			char_can_notify_enabled = false;
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK)
                ESP_LOGE(DEBUG_TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            else if (param->add_attr_tab.num_handle != HRS_IDX_NB)
			{
                ESP_LOGE(DEBUG_TAG, "create attribute table abnormally, num_handle (%d) \
                        doesn't equal to HRS_IDX_NB(%d)", param->add_attr_tab.num_handle, HRS_IDX_NB);
            }
            else 
			{
                ESP_LOGI(DEBUG_TAG, "create attribute table successfully, the number handle = %d\n",param->add_attr_tab.num_handle);
                memcpy(ble_handle_table, param->add_attr_tab.handles, sizeof(ble_handle_table));
                esp_ble_gatts_start_service(ble_handle_table[IDX_SVC]);
            }
            break;
        case ESP_GATTS_STOP_EVT:
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        case ESP_GATTS_UNREG_EVT:
        case ESP_GATTS_DELETE_EVT:
        default:
            break;
    }
}

void parse_ble_packet()
{
	uint32_t id;
	uint8_t data_len;
	can_message_t message;
	
	if (ble_buf[0] != 0xAA)
		return;
		
	data_len = ble_buf[5];
	
	if (data_len > 8)
	{
		ESP_LOGI(DEBUG_TAG, "Bad length: %d", data_len);
		return;
	}
	
	if (ble_buf[10 + data_len] != 0xBB)
	{
		ESP_LOGI(DEBUG_TAG, "Bad check byte: %d", ble_buf[10 + data_len]);
		return;
	}
	
	id = ble_buf[6];
	id |= (uint32_t)ble_buf[7] << 8;
	id |= (uint32_t)ble_buf[8] << 16;
	id |= (uint32_t)ble_buf[9] << 24;
	
	
	message.identifier = id;
	if (id > 0x7FF)
		message.flags = CAN_MSG_FLAG_EXTD;	// TODO: this should be explicit
		
	message.data_length_code = data_len;
	for (int i = 0; i < data_len; i++) {
		message.data[i] = ble_buf[10+i];
	}

	//Queue message for transmission
	if (can_transmit(&message, pdMS_TO_TICKS(100)) == ESP_OK) {
		printf("Message queued for transmission\n");
	} else {
		printf("Failed to queue message for transmission\n");
	}
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) 
	{
        if (param->reg.status == ESP_GATT_OK) 
		{
            ble_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
        } 
		else 
		{
            ESP_LOGE(DEBUG_TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }
	
	int idx;
	for (idx = 0; idx < PROFILE_NUM; idx++) 
	{
		/* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
		if (gatts_if == ESP_GATT_IF_NONE || gatts_if == ble_profile_tab[idx].gatts_if) 
		{
			if (ble_profile_tab[idx].gatts_cb) 
			{
				ble_profile_tab[idx].gatts_cb(event, gatts_if, param);
			}
		}
	}
}

void task_BLE(void *pvParameters)
{	
	can_message_t message;
	char buffer[19];
	uint32_t msecs;
	
	while (1)
	{
		if (!char_can_notify_enabled)
		{
			vTaskDelay(1000 / portTICK_PERIOD_MS);
			continue;
		}
		
		if (can_queue != NULL && xQueueReceive(can_queue, &message, 1))
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
			
			// TODO: this should be notify! We don't need a response.
			esp_ble_gatts_send_indicate(
				spp_gatts_if, 
				spp_conn_id, 
				ble_handle_table[IDX_CHAR_VAL_CAN], 
				11+message.data_length_code,
				(uint8_t*)&buffer, 
				false);
				
			if (message.identifier == 0x101)
				ESP_LOGE(DEBUG_TAG, "Sending 0x101\n");
				
			old_can_id = 0;
		}
		//vTaskDelay(5 / portTICK_PERIOD_MS);
	}
}

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
			ESP_LOGE(DEBUG_TAG, "Failed to receive message\n");
			continue;
		}
		
		if (!(message.flags & CAN_MSG_FLAG_RTR)) 
		{
			if (can_queue != NULL)
			{
				/*
				if (uxQueueSpacesAvailable(can_queue) == 0)
				{
					if (old_can_id != 0x101)
					{
						// Queue is full but the item isn't important, overwrite
						old_can_id = message.identifier;
						xQueueOverwrite(can_queue, &message);
					}
					else
					{
						//ESP_LOGE(DEBUG_TAG, "Didn't overwrite 0x101 in queue\n");
					}
				}
				else
				{
					// Space in the queue
					old_can_id = message.identifier;
					xQueueOverwrite(can_queue, &message);
				}
				*/
				
				// TODO: clumsy attempt at priority messages
				if (uxQueueSpacesAvailable(can_queue) > 5 || message.identifier == 0x101)
					xQueueSend(can_queue, &message, pdMS_TO_TICKS(1));
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

void app_main()
{
	esp_err_t ret;
	
	// Queue length must be 1 for the overwrite function
	//can_queue = xQueueCreate(1, sizeof(can_message_t));
	can_queue = xQueueCreate(10, sizeof(can_message_t));
	
	// Setup wifi manager to manage saved SSIDs
	wifi_manager_start();
	wifi_manager_set_callback(EVENT_STA_GOT_IP, &cb_connection_ok);
	
	// Start BLE
	adv_data.set_scan_rsp        = false;
    adv_data.include_name        = true;
    adv_data.include_txpower     = true;
    adv_data.min_interval        = 0x0006; //slave connection min interval, Time = min_interval * 1.25 msec
    adv_data.max_interval        = 0x0010; //slave connection max interval, Time = max_interval * 1.25 msec
    adv_data.appearance          = 0x00;
    adv_data.manufacturer_len    = 7;
    adv_data.p_manufacturer_data = &manufacturer[0];
    adv_data.service_data_len    = 0;
    adv_data.p_service_data      = NULL;
    adv_data.service_uuid_len    = sizeof(service_uuid);
    adv_data.p_service_uuid      = service_uuid;
    adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
	
	scan_rsp_data.set_scan_rsp        = true;
    scan_rsp_data.include_name        = true;
    scan_rsp_data.include_txpower     = true;
    scan_rsp_data.min_interval        = 0x0006;
    scan_rsp_data.max_interval        = 0x0010;
    scan_rsp_data.appearance          = 0x00;
    scan_rsp_data.manufacturer_len    = 7;
    scan_rsp_data.p_manufacturer_data = &manufacturer[0];
    scan_rsp_data.service_data_len    = 0;
    scan_rsp_data.p_service_data      = NULL;
    scan_rsp_data.service_uuid_len    = sizeof(service_uuid);
    scan_rsp_data.p_service_uuid      = service_uuid;
    scan_rsp_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
	
	adv_params.adv_int_min         = 0x20;
    adv_params.adv_int_max         = 0x40;
    adv_params.adv_type            = ADV_TYPE_IND;
    adv_params.own_addr_type       = BLE_ADDR_TYPE_PUBLIC;
    adv_params.channel_map         = ADV_CHNL_ALL;
    adv_params.adv_filter_policy   = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
	
    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
	{
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
	
	
	esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) 
        ESP_LOGE(DEBUG_TAG, "%s initialize controller failed\n", __func__);

	if (!ret)
		ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret)
        ESP_LOGE(DEBUG_TAG, "%s enable controller failed\n", __func__);
	
	if (!ret)
		ret = esp_bluedroid_init();
    if (ret)
        ESP_LOGE(DEBUG_TAG, "%s init bluetooth failed\n", __func__);

	if (!ret)
		ret = esp_bluedroid_enable();
    if (ret)
        ESP_LOGE(DEBUG_TAG, "%s enable bluetooth failed\n", __func__);

	if (!ret)
		ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret)
        ESP_LOGE(DEBUG_TAG, "gatts register error, error code = %x", ret);

	if (!ret)
		ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret)
        ESP_LOGE(DEBUG_TAG, "gap register error, error code = %x", ret);
    
	if (!ret)
		ret = esp_ble_gatts_app_register(ESP_APP_ID);
    if (ret)
        ESP_LOGE(DEBUG_TAG, "gatts app register error, error code = %x", ret);

	if (!ret)
		ret = esp_ble_gatt_set_local_mtu(512);
    if (ret)
        ESP_LOGE(DEBUG_TAG, "set local  MTU failed, error code = %x", ret);
	
	// Start CAN
    ESP_LOGI(DEBUG_TAG, "Starting CAN");
    can_general_config_t g_config = CAN_GENERAL_CONFIG_DEFAULT(GPIO_NUM_5, GPIO_NUM_4, CAN_MODE_NORMAL);
    can_timing_config_t t_config = CAN_TIMING_CONFIG_500KBITS();
    can_filter_config_t f_config = CAN_FILTER_CONFIG_ACCEPT_ALL();

    //Install CAN driver
    if (can_driver_install(&g_config, &t_config, &f_config) != ESP_OK)
	{
        ESP_LOGE(DEBUG_TAG, "Failed to install CAN driver\n");
        return;
    }

    //Start CAN driver
    if (can_start() != ESP_OK) 
	{
        ESP_LOGE(DEBUG_TAG, "Failed to start CAN driver\n");
        return;
    }
	
	xTaskCreate(&task_CAN, "CAN", 2048, NULL, 5, NULL);
	xTaskCreate(&task_BLE, "BLE", 2048, NULL, 5, NULL);
	xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
}

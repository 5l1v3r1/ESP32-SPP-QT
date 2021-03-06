#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <driver/gpio.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"

#include "ts_proto.h"




#define SPP_TAG "SPP_SLAVE"
#define SPP_SERVER_NAME "SPP_SERVER"
#define SPP_DEVICE_NAME "Kolsha_SPP#" __TIME__


static const esp_spp_mode_t esp_spp_mode = ESP_SPP_MODE_CB;
static const esp_spp_sec_t sec_mask = ESP_SPP_SEC_NONE;
static const esp_spp_role_t role_slave = ESP_SPP_ROLE_SLAVE;


uint32_t bt_handle = 0;

#define MSG_IN_QUEUE_SIZE 10
static xQueueHandle msg_in_queue = NULL;
static xQueueHandle btn_boot_queue = NULL ;


static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_INIT_EVT");
        esp_bt_dev_set_device_name(SPP_DEVICE_NAME);
        esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
        esp_spp_start_srv(sec_mask, role_slave, 0, SPP_SERVER_NAME);
        break;
    case ESP_SPP_DISCOVERY_COMP_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_DISCOVERY_COMP_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_OPEN_EVT");


        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_CLOSE_EVT");
        if(param->close.handle == bt_handle) {
            bt_handle = 0;
        }
        break;
    case ESP_SPP_START_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_START_EVT");
        break;
    case ESP_SPP_CL_INIT_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_CL_INIT_EVT");
        break;
    case ESP_SPP_DATA_IND_EVT:
    {


        if(parse_raw_data(param->data_ind.data) == NULL){
            ESP_LOGI(SPP_TAG, "bad message, skip it");
            break;
        }

        tsMsg_t *msg_buff = (tsMsg_t *)malloc(param->data_ind.len * sizeof(uint8_t));
        if(msg_buff == NULL) {
            ESP_LOGE(SPP_TAG, "%s malloc failed\n", __func__);
            break;
        }

        memcpy(msg_buff, param->data_ind.data, param->data_ind.len);


        xQueueSend(msg_in_queue, &msg_buff, (10 / portTICK_PERIOD_MS) );
    }
    break;
    case ESP_SPP_CONG_EVT:
        //ESP_LOGI(SPP_TAG, "ESP_SPP_CONG_EVT");
        break;
    case ESP_SPP_WRITE_EVT:
        //ESP_LOGI(SPP_TAG, "ESP_SPP_WRITE_EVT");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGI(SPP_TAG, "ESP_SPP_SRV_OPEN_EVT");
        if(!bt_handle) {
            bt_handle = param->srv_open.handle;
        } else {
            esp_spp_disconnect(param->srv_open.handle);
        }
        break;
    default:
        break;
    }
}



void app_send_msgs_t(tsMsg_t *msg, uint8_t count) {
    if(msg == NULL || count < 1) {
        return;
    }

    esp_spp_write(bt_handle, sizeof(tsMsg_t) * count, (uint8_t *)msg);
}

void app_send_msg(tsProtoCmds_t cmd, uint8_t * data) {
    tsMsg_t msg = {
        .timestamp =  get_ts_time(),
        .cmd = cmd,
        .data = ""
    };

    if(data != NULL) {
        memcpy(msg.data, data, tsProto_MSG_DATA_LEN);
    }

    prepare_msg(&msg);

    app_send_msgs_t(&msg, 1);
}



void app_check_incoming_msg() {

    tsMsg_t * msg = NULL;
    if(!xQueueReceive(msg_in_queue, &msg, 0)) {
        return ;
    }

    /*
    ESP_LOGI(
        SPP_TAG,
        "Incoming msg: cmd = %d\n%llu.%llu\n%20s",
        msg->cmd,
        msg->timestamp.tv_sec, msg->timestamp.tv_usec,
        msg->data
    );
    */

    switch(msg->cmd) {
    case timeSyncReq:
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        tv.tv_sec = msg->timestamp.tv_sec;
        tv.tv_usec = msg->timestamp.tv_usec;

        if(settimeofday(&tv, NULL) != 0) {
            break;
        }
        app_send_msg(timeSyncResponse, NULL);

    }





    break;
    default:
        ESP_LOGI(
            SPP_TAG,
            "Unknown cmd"
        );
    }

    free(msg);

}






void app_logic_task(void *pvParameters)
{


    uint8_t info[tsProto_MSG_DATA_LEN] = {"btn_boot"};
    gpio_num_t gpio;

    for(;;) {
        vTaskDelay(1 / portTICK_PERIOD_MS);

        if(!bt_handle) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        app_check_incoming_msg();

        if(!!xQueueReceive(btn_boot_queue, &gpio, 1 / portTICK_PERIOD_MS)) {
            app_send_msg(dataOut, info);
        }

    }

    vTaskDelete(NULL);

}




static void btn_boot_handler(void *args) {
    gpio_num_t gpio;
    gpio = GPIO_NUM_0;
    xQueueSendToBackFromISR(btn_boot_queue, &gpio, NULL);
}

void init_btn_boot(void) {

    ESP_LOGI(SPP_TAG, "init_btn_boot");

    btn_boot_queue = xQueueCreate(10, sizeof(gpio_num_t));

    if(btn_boot_queue == NULL) {
        ESP_LOGE(SPP_TAG, "%s btn_boot_queue create failed\n", __func__);
        return;
    }


    esp_err_t ret;


    gpio_config_t gpioConfig;
    gpioConfig.pin_bit_mask = GPIO_SEL_0;
    gpioConfig.mode         = GPIO_MODE_INPUT;
    gpioConfig.pull_up_en   = GPIO_PULLUP_DISABLE;
    gpioConfig.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpioConfig.intr_type    = GPIO_INTR_POSEDGE;

    if((ret = gpio_config(&gpioConfig)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s gpio_config failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if((ret = gpio_install_isr_service(0)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s gpio_install_isr_service failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if((ret = gpio_isr_handler_add(GPIO_NUM_0, btn_boot_handler, NULL)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s gpio_isr_handler_add failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }



}


void app_main()
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );


    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((ret = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_register_callback(esp_spp_cb)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s spp register failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    if ((ret = esp_spp_init(esp_spp_mode)) != ESP_OK) {
        ESP_LOGE(SPP_TAG, "%s spp init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }

    msg_in_queue = xQueueCreate(MSG_IN_QUEUE_SIZE, sizeof(tsMsg_t *));
    if(msg_in_queue == NULL) {
        ESP_LOGE(SPP_TAG, "%s incoming queue create failed\n", __func__);
        return;
    }

    init_btn_boot();

    xTaskCreatePinnedToCore(&app_logic_task, "app_logic_task", 2048, NULL, 5, NULL, 1);
}


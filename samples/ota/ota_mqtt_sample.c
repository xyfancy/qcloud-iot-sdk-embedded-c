/*
 * Tencent is pleased to support the open source community by making IoT Hub available.
 * Copyright (C) 2016 THL A29 Limited, a Tencent company. All rights reserved.

 * Licensed under the MIT License (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://opensource.org/licenses/MIT

 * Unless required by applicable law or agreed to in writing, software distributed under the License is
 * distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "qcloud_iot_export.h"
#include "qcloud_iot_import.h"

/* 产品名称, 与云端同步设备状态时需要  */
#ifdef AUTH_MODE_CERT
    static char sg_cert_file[PATH_MAX + 1];      //客户端证书全路径
    static char sg_key_file[PATH_MAX + 1];       //客户端密钥全路径
#endif

static DeviceInfo sg_devInfo;


#define OTA_BUF_LEN (5000)

static bool sg_pub_ack = false;
static int sg_packet_id = 0;

static void event_handler(void *pclient, void *handle_context, MQTTEventMsg *msg) 
{	
	uintptr_t packet_id = (uintptr_t)msg->msg;

	switch(msg->event_type) {
		case MQTT_EVENT_UNDEF:
			Log_i("undefined event occur.");
			break;

		case MQTT_EVENT_DISCONNECT:
			Log_i("MQTT disconnect.");
			break;

		case MQTT_EVENT_RECONNECT:
			Log_i("MQTT reconnect.");
			break;

		case MQTT_EVENT_SUBCRIBE_SUCCESS:
			Log_i("subscribe success, packet-id=%u", (unsigned int)packet_id);
			break;

		case MQTT_EVENT_SUBCRIBE_TIMEOUT:
			Log_i("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
			break;

		case MQTT_EVENT_SUBCRIBE_NACK:
			Log_i("subscribe nack, packet-id=%u", (unsigned int)packet_id);
			break;

		case MQTT_EVENT_PUBLISH_SUCCESS:
			Log_i("publish success, packet-id=%u", (unsigned int)packet_id);
            if (sg_packet_id == packet_id)
                sg_pub_ack = true;
			break;

		case MQTT_EVENT_PUBLISH_TIMEOUT:
			Log_i("publish timeout, packet-id=%u", (unsigned int)packet_id);
			break;

		case MQTT_EVENT_PUBLISH_NACK:
			Log_i("publish nack, packet-id=%u", (unsigned int)packet_id);
			break;
		default:
			Log_i("Should NOT arrive here.");
			break;
	}
}

static int _setup_connect_init_params(MQTTInitParams* initParams)
{
	int ret;
	
	ret = HAL_GetDevInfo((void *)&sg_devInfo);	
	if(QCLOUD_ERR_SUCCESS != ret){
		return ret;
	}
	
	initParams->device_name = sg_devInfo.device_name;
	initParams->product_id = sg_devInfo.product_id;

#ifdef AUTH_MODE_CERT
	/* 使用非对称加密*/
	char certs_dir[PATH_MAX + 1] = "certs";
	char current_path[PATH_MAX + 1];
	char *cwd = getcwd(current_path, sizeof(current_path));
	if (cwd == NULL)
	{
		Log_e("getcwd return NULL");
		return QCLOUD_ERR_FAILURE;
	}
	sprintf(sg_cert_file, "%s/%s/%s", current_path, certs_dir, sg_devInfo.devCertFileName);
	sprintf(sg_key_file, "%s/%s/%s", current_path, certs_dir, sg_devInfo.devPrivateKeyFileName);

	initParams->cert_file = sg_cert_file;
	initParams->key_file = sg_key_file;
#else
	initParams->device_secret = sg_devInfo.devSerc;
#endif

    
	initParams->command_timeout = QCLOUD_IOT_MQTT_COMMAND_TIMEOUT;
	initParams->keep_alive_interval_ms = QCLOUD_IOT_MQTT_KEEP_ALIVE_INTERNAL;
	initParams->auto_connect_enable = 1;
    initParams->event_handle.h_fp = event_handler;

    return QCLOUD_ERR_SUCCESS;
}

int main(int argc, char **argv) 
{
    IOT_Log_Set_Level(DEBUG);
    int rc;

    MQTTInitParams init_params = DEFAULT_MQTTINIT_PARAMS;
    rc = _setup_connect_init_params(&init_params);
	if (rc != QCLOUD_ERR_SUCCESS) {
		Log_e("init params err,rc=%d", rc);
		return rc;
	}

    void *client = IOT_MQTT_Construct(&init_params);
    if (client != NULL) {
        Log_i("Cloud Device Construct Success");
    } else {
        Log_e("Cloud Device Construct Failed");
        return QCLOUD_ERR_FAILURE;
    }

    void *h_ota = IOT_OTA_Init(sg_devInfo.product_id, sg_devInfo.device_name, client);
    if (NULL == h_ota) {
        Log_e("initialize OTA failed");
        return QCLOUD_ERR_FAILURE;
    }

    /* Must report version first */
    if (0 > IOT_OTA_ReportVersion(h_ota, "1.0.0")) {
        Log_e("report OTA version failed");
        return QCLOUD_ERR_FAILURE;
    }

    HAL_SleepMs(2000);

    int ota_over = 0;
    bool upgrade_fetch_success = true;
    FILE *fp;
    char buf_ota[OTA_BUF_LEN];
    if (NULL == (fp = fopen("ota.bin", "wb+"))) {
        Log_e("open file failed");
        return QCLOUD_ERR_FAILURE;
    }

    do {
        uint32_t firmware_valid;
        Log_i("wait for ota upgrade command...");

        IOT_MQTT_Yield(client, 200);
        
        if (IOT_OTA_IsFetching(h_ota)) {
            char version[128], md5sum[33];
            uint32_t len, size_downloaded, size_file;
            do {
                len = IOT_OTA_FetchYield(h_ota, buf_ota, OTA_BUF_LEN, 1);
                if (len > 0) {
                    if (1 != fwrite(buf_ota, len, 1, fp)) {
                        Log_e("write data to file failed");
                        upgrade_fetch_success = false;
                        break;
                    }
                } else if (len < 0) {
                    Log_e("download fail rc=%d", len);
                    upgrade_fetch_success = false;
                    break;
                }

                /* get OTA information */
                IOT_OTA_Ioctl(h_ota, IOT_OTAG_FETCHED_SIZE, &size_downloaded, 4);
                IOT_OTA_Ioctl(h_ota, IOT_OTAG_FILE_SIZE, &size_file, 4);
                IOT_OTA_Ioctl(h_ota, IOT_OTAG_MD5SUM, md5sum, 33);
                IOT_OTA_Ioctl(h_ota, IOT_OTAG_VERSION, version, 128);

                IOT_MQTT_Yield(client, 100);
            } while (!IOT_OTA_IsFetchFinish(h_ota));

            /* Must check MD5 match or not */ 
            if (upgrade_fetch_success) {
                IOT_OTA_Ioctl(h_ota, IOT_OTAG_CHECK_FIRMWARE, &firmware_valid, 4);
                if (0 == firmware_valid) {
                    Log_e("The firmware is invalid");
                    upgrade_fetch_success = false;
                } else {
                    Log_i("The firmware is valid");
                    upgrade_fetch_success = true;
                }
            }

            ota_over = 1;
        }

        HAL_SleepMs(2000);
    } while(!ota_over);


    if (upgrade_fetch_success)
    {
        /* begin execute OTA files, should report upgrade begin */ 
        // sg_packet_id = IOT_OTA_ReportUpgradeBegin(h_ota);
        // if (0 > sg_packet_id) {
        //     Log_e("report OTA begin failed error:%d", sg_packet_id);
        //     return QCLOUD_ERR_FAILURE;
        // }
        // while (!sg_pub_ack) {
        //     HAL_SleepMs(1000);
        //     IOT_MQTT_Yield(client, 200);
        // }
        // sg_pub_ack = false;
        
        /* if upgrade success */
        /* after execute OTA files, should report upgrade result */
        // sg_packet_id = IOT_OTA_ReportUpgradeSuccess(h_ota, "1.0.1");
        // if (0 > sg_packet_id) {
        //     Log_e("report OTA result failed error:%d", sg_packet_id);
        //     return QCLOUD_ERR_FAILURE;
        // }
        // while (!sg_pub_ack) {
        //     HAL_SleepMs(1000);
        //     IOT_MQTT_Yield(client, 200);
        // }
        // sg_pub_ack = false;

         /* if upgrade fail */
        // sg_packet_id = IOT_OTA_ReportUpgradeFail(h_ota, "1.0.1"); 
        // if (0 > sg_packet_id) {
        //     Log_e("report OTA result failed error:%d", sg_packet_id);
        //     return QCLOUD_ERR_FAILURE;
        // }
        // while (!sg_pub_ack) {
        //     HAL_SleepMs(1000);
        //     IOT_MQTT_Yield(client, 200);
        // }
        // sg_pub_ack = false;
    }

    IOT_OTA_Destroy(h_ota);

    IOT_MQTT_Destroy(&client);
    
    return 0;
}

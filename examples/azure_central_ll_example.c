#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/util/datetime.h"

#include "wizchip_conf.h"
#include "dhcp.h"
#include "dns.h"
#include "netif.h"
#include "azure_c_shared_utility/tcpsocketconnection_c.h"
#include "iothub.h"
#include "iothub_device_client_ll.h"
#include "iothub_client_options.h"
#include "iothub_message.h"
#include "iothub_client_version.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/tickcounter.h"
#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/shared_util_options.h"
#include "azure_c_shared_utility/agenttime.h"
#include "azure_c_shared_utility/http_proxy_io.h"
#include "azure_prov_client/prov_device_ll_client.h"
#include "azure_prov_client/prov_security_factory.h"

#include "mbedtls/debug.h"
#include "azure_samples.h"

/* This sample uses the _LL APIs of iothub_client for example purposes.
Simply changing the using the convenience layer (functions not having _LL)
and removing calls to _DoWork will yield the same results. */

// The protocol you wish to use should be uncommented
//
#define SAMPLE_MQTT
//#define SAMPLE_MQTT_OVER_WEBSOCKETS
//#define SAMPLE_AMQP
//#define SAMPLE_AMQP_OVER_WEBSOCKETS
//#define SAMPLE_HTTP

#ifdef SAMPLE_MQTT
#include "iothubtransportmqtt.h"
#include "azure_prov_client/prov_transport_mqtt_client.h"
#endif // SAMPLE_MQTT
#ifdef SAMPLE_MQTT_OVER_WEBSOCKETS
#include "iothubtransportmqtt_websockets.h"
#include "azure_prov_client/prov_transport_mqtt_ws_client.h"
#endif // SAMPLE_MQTT_OVER_WEBSOCKETS
#ifdef SAMPLE_AMQP
#include "iothubtransportamqp.h"
#include "azure_prov_client/prov_transport_amqp_client.h"
#endif // SAMPLE_AMQP
#ifdef SAMPLE_AMQP_OVER_WEBSOCKETS
#include "iothubtransportamqp_websockets.h"
#include "azure_prov_client/prov_transport_amqp_ws_client.h"
#endif // SAMPLE_AMQP_OVER_WEBSOCKETS
#ifdef SAMPLE_HTTP
#include "iothubtransporthttp.h"
#include "azure_prov_client/prov_transport_http_client.h"
#endif // SAMPLE_HTTP

// This sample is to demostrate iothub reconnection with provisioning and should not
// be confused as production code
#ifdef SET_TRUSTED_CERT_IN_SAMPLES
#include "certs.h"
#endif // SET_TRUSTED_CERT_IN_SAMPLES

static const MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(PROV_DEVICE_RESULT, PROV_DEVICE_RESULT_VALUE);
static const MU_DEFINE_ENUM_STRINGS_WITHOUT_INVALID(PROV_DEVICE_REG_STATUS, PROV_DEVICE_REG_STATUS_VALUES);

static const char* global_prov_uri = "global.azure-devices-provisioning.net";
//static const char* id_scope = "[ID Scope]";
static const char* id_scope = pico_az_id_scope;

static bool g_use_proxy = false;
static const char* PROXY_ADDRESS = "127.0.0.1";

#define PROXY_PORT                  8888
//#define MESSAGES_TO_SEND            10
#define TIME_BETWEEN_MESSAGES       1000

#if 1
#define ONBOARD_LED    PICO_DEFAULT_LED_PIN
static bool IoTHub_Running = true;
#endif

typedef struct CLIENT_SAMPLE_INFO_TAG
{
    unsigned int sleep_time;
    char* iothub_uri;
    char* access_key_name;
    char* device_key;
    char* device_id;
    int registration_complete;
} CLIENT_SAMPLE_INFO;

typedef struct IOTHUB_CLIENT_SAMPLE_INFO_TAG
{
    int connected;
    int stop_running;
} IOTHUB_CLIENT_SAMPLE_INFO;


static int deviceMethodCallback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size, void* userContextCallback)
{
    (void)userContextCallback;
    (void)payload;
    (void)size;

    int result;

    gpio_init(ONBOARD_LED);
    gpio_set_dir(ONBOARD_LED, GPIO_OUT);

    //printf("Device method %s arrived...\n", method_name);

    if (strcmp("LEDON", method_name) == 0) {
        
        printf("\nReceived LEDON command from Azure IoT Central\n");

        const char deviceMethodResponse[] = "{ \"Response\": \"LEDON\" }";
        *response_size = sizeof(deviceMethodResponse)-1;
        *response = malloc(*response_size);
        (void)memcpy(*response, deviceMethodResponse, *response_size);
        
        gpio_put(ONBOARD_LED, 1);

        result = 200;

    }
    else if (strcmp("LEDOFF", method_name) == 0)
    {
        printf("\nReceived LEDOFF command from Azure IoT Central\n");

        const char deviceMethodResponse[] = "{ \"Response\": \"LEDOFF\" }";
        *response_size = sizeof(deviceMethodResponse)-1;
        *response = malloc(*response_size);
        (void)memcpy(*response, deviceMethodResponse, *response_size);
        
        gpio_put(ONBOARD_LED, 0);

        result = 200;
    }
    else if (strcmp("STOP", method_name) == 0)
    {
        printf("\nReceived STOP command from Azure IoT Central\n");

        const char deviceMethodResponse[] = "{ \"Response\": \"DEVICE STOP\" }";
        *response_size = sizeof(deviceMethodResponse)-1;
        *response = malloc(*response_size);
        (void)memcpy(*response, deviceMethodResponse, *response_size);
        
        gpio_put(ONBOARD_LED, 0);
        IoTHub_Running = false;

        result = 200;
    } else {
        // All other entries are ignored.
        const char deviceMethodResponse[] = "{ }";
        *response_size = sizeof(deviceMethodResponse)-1;
        *response = malloc(*response_size);
        (void)memcpy(*response, deviceMethodResponse, *response_size);
        result = -1;
    }

    return result;
}

static void registration_status_callback(PROV_DEVICE_REG_STATUS reg_status, void* user_context)
{
    (void)user_context;
    (void)printf("Provisioning Status: %s\r\n", MU_ENUM_TO_STRING(PROV_DEVICE_REG_STATUS, reg_status));
}

static void iothub_connection_status(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* user_context)
{
    (void)reason;
    if (user_context == NULL)
    {
        printf("iothub_connection_status user_context is NULL\r\n");
    }
    else
    {
        IOTHUB_CLIENT_SAMPLE_INFO* iothub_info = (IOTHUB_CLIENT_SAMPLE_INFO*)user_context;
        if (result == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
        {
            iothub_info->connected = 1;
        }
        else
        {
            iothub_info->connected = 0;
            iothub_info->stop_running = 1;
        }
    }
}

static void register_device_callback(PROV_DEVICE_RESULT register_result, const char* iothub_uri, const char* device_id, void* user_context)
{
    if (user_context == NULL)
    {
        printf("user_context is NULL\r\n");
    }
    else
    {
        CLIENT_SAMPLE_INFO* user_ctx = (CLIENT_SAMPLE_INFO*)user_context;
        if (register_result == PROV_DEVICE_RESULT_OK)
        {
            (void)printf("Registration Information received from service: %s!\r\n", iothub_uri);
            (void)mallocAndStrcpy_s(&user_ctx->iothub_uri, iothub_uri);
            (void)mallocAndStrcpy_s(&user_ctx->device_id, device_id);
            user_ctx->registration_complete = 1;
        }
        else
        {
            (void)printf("Failure encountered on registration %s\r\n", MU_ENUM_TO_STRING(PROV_DEVICE_RESULT, register_result) );
            user_ctx->registration_complete = 2;
        }
    }
}

#if 1
static void reportedStateCallback(int status_code, void* userContextCallback)
{
    (void)userContextCallback;
    printf("Device Twin reported properties update completed with result: %d\r\n", status_code);
}

static void getCompleteDeviceTwinOnDemandCallback(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payLoad, size_t size, void* userContextCallback)
{
    (void)update_state;
    (void)userContextCallback;
    printf("GetTwinAsync result:\r\n%.*s\r\n", (int)size, payLoad);
}
#endif 

void azure_central_ll_example(void)
{
    SECURE_DEVICE_TYPE hsm_type;
    //hsm_type = SECURE_DEVICE_TYPE_TPM;
    hsm_type = SECURE_DEVICE_TYPE_X509;
    //hsm_type = SECURE_DEVICE_TYPE_SYMMETRIC_KEY;

    bool traceOn = false;

    (void)IoTHub_Init();
    (void)prov_dev_security_init(hsm_type);
    // Set the symmetric key if using they auth type
    //prov_dev_set_symmetric_key_info("symm-key-device-007", "hyKuFyHXCUVE1fW9f+phyNSs0X+8ZInphXD5riBsN7023c7WonbefqCaRtjBo6al3fhb3lXL2cV8qJafpU0kmA==");

    PROV_DEVICE_TRANSPORT_PROVIDER_FUNCTION prov_transport;
    HTTP_PROXY_OPTIONS http_proxy;
    CLIENT_SAMPLE_INFO user_ctx;

    memset(&http_proxy, 0, sizeof(HTTP_PROXY_OPTIONS));
    memset(&user_ctx, 0, sizeof(CLIENT_SAMPLE_INFO));

    // Protocol to USE - HTTP, AMQP, AMQP_WS, MQTT, MQTT_WS
#ifdef SAMPLE_MQTT
    prov_transport = Prov_Device_MQTT_Protocol;
#endif // SAMPLE_MQTT
#ifdef SAMPLE_MQTT_OVER_WEBSOCKETS
    prov_transport = Prov_Device_MQTT_WS_Protocol;
#endif // SAMPLE_MQTT_OVER_WEBSOCKETS
#ifdef SAMPLE_AMQP
    prov_transport = Prov_Device_AMQP_Protocol;
#endif // SAMPLE_AMQP
#ifdef SAMPLE_AMQP_OVER_WEBSOCKETS
    prov_transport = Prov_Device_AMQP_WS_Protocol;
#endif // SAMPLE_AMQP_OVER_WEBSOCKETS
#ifdef SAMPLE_HTTP
    prov_transport = Prov_Device_HTTP_Protocol;
#endif // SAMPLE_HTTP

    // Set ini
    user_ctx.registration_complete = 0;
    user_ctx.sleep_time = 10;

#if 1
    float telemetry_temperature;
    float telemetry_humidity;
    char telemetry_msg_buffer[80];    
    size_t messages_sent = 0;
#endif

    printf("Provisioning API Version: %s\r\n", Prov_Device_LL_GetVersionString());
    printf("Iothub API Version: %s\r\n", IoTHubClient_GetVersionString());

    if (g_use_proxy)
    {
        http_proxy.host_address = PROXY_ADDRESS;
        http_proxy.port = PROXY_PORT;
    }

    PROV_DEVICE_LL_HANDLE handle;
    if ((handle = Prov_Device_LL_Create(global_prov_uri, id_scope, prov_transport)) == NULL)
    {
        (void)printf("failed calling Prov_Device_LL_Create\r\n");
    }
    else
    {
        if (http_proxy.host_address != NULL)
        {
            Prov_Device_LL_SetOption(handle, OPTION_HTTP_PROXY, &http_proxy);
        }

        Prov_Device_LL_SetOption(handle, PROV_OPTION_LOG_TRACE, &traceOn);
#ifdef SET_TRUSTED_CERT_IN_SAMPLES
        // Setting the Trusted Certificate. This is only necessary on systems without
        // built in certificate stores.
        Prov_Device_LL_SetOption(handle, OPTION_TRUSTED_CERT, certificates);
#endif // SET_TRUSTED_CERT_IN_SAMPLES

        if (Prov_Device_LL_Register_Device(handle, register_device_callback, &user_ctx, registration_status_callback, &user_ctx) != PROV_DEVICE_RESULT_OK)
        {
            (void)printf("failed calling Prov_Device_LL_Register_Device\r\n");
        }
        else
        {
            do
            {
                Prov_Device_LL_DoWork(handle);
                ThreadAPI_Sleep(user_ctx.sleep_time);
            } while (user_ctx.registration_complete == 0);
        }
        Prov_Device_LL_Destroy(handle);
    }

    if (user_ctx.registration_complete != 1)
    {
        (void)printf("registration failed!\r\n");
    }
    else
    {
        IOTHUB_CLIENT_TRANSPORT_PROVIDER iothub_transport;

        // Protocol to USE - HTTP, AMQP, AMQP_WS, MQTT, MQTT_WS
#if defined(SAMPLE_MQTT) || defined(SAMPLE_HTTP) // HTTP sample will use mqtt protocol
        iothub_transport = MQTT_Protocol;
#endif // SAMPLE_MQTT
#ifdef SAMPLE_MQTT_OVER_WEBSOCKETS
        iothub_transport = MQTT_WebSocket_Protocol;
#endif // SAMPLE_MQTT_OVER_WEBSOCKETS
#ifdef SAMPLE_AMQP
        iothub_transport = AMQP_Protocol;
#endif // SAMPLE_AMQP
#ifdef SAMPLE_AMQP_OVER_WEBSOCKETS
        iothub_transport = AMQP_Protocol_over_WebSocketsTls;
#endif // SAMPLE_AMQP_OVER_WEBSOCKETS

        IOTHUB_DEVICE_CLIENT_LL_HANDLE device_ll_handle;

        (void)printf("Creating IoTHub Device handle\r\n");
        if ((device_ll_handle = IoTHubDeviceClient_LL_CreateFromDeviceAuth(user_ctx.iothub_uri, user_ctx.device_id, iothub_transport) ) == NULL)
        {
            (void)printf("failed create IoTHub client from connection string %s!\r\n", user_ctx.iothub_uri);
        }
        else
        {
            (void)IoTHubDeviceClient_LL_SetConnectionStatusCallback(device_ll_handle, iothub_connection_status, NULL);

            // Set any option that are neccessary.
            // For available options please see the iothub_sdk_options.md documentation
            IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_LOG_TRACE, &traceOn);

#ifdef SET_TRUSTED_CERT_IN_SAMPLES
            // Setting the Trusted Certificate. This is only necessary on systems without
            // built in certificate stores.
            IoTHubDeviceClient_LL_SetOption(device_ll_handle, OPTION_TRUSTED_CERT, certificates);
#endif // SET_TRUSTED_CERT_IN_SAMPLES

            (void)IoTHubDeviceClient_LL_SetDeviceMethodCallback(device_ll_handle, deviceMethodCallback, NULL);
            (void)printf("Sending 1 messages to IoTHub every %d seconds (Send STOP command to stop)\r\n", TIME_BETWEEN_MESSAGES);

            do
            {
                // Construct the iothub message
                telemetry_temperature = 30.0f + ((float)rand() / RAND_MAX) * 15.0f;
                telemetry_humidity = 50.0f + ((float)rand() / RAND_MAX) * 20.0f;

                sprintf(telemetry_msg_buffer, "{\"temperature\":%.3f,\"humidity\":%.3f}", 
                    telemetry_temperature, telemetry_humidity);
                IOTHUB_MESSAGE_HANDLE message_handle = IoTHubMessage_CreateFromString(telemetry_msg_buffer);

                (void)printf("\r\nSending message %d to IoTHub\r\nMessage: %s\r\n", (int)(messages_sent + 1), telemetry_msg_buffer);
                IoTHubDeviceClient_LL_SendEventAsync(device_ll_handle, message_handle, NULL, NULL);

                // The message is copied to the sdk so the we can destroy it
                IoTHubMessage_Destroy(message_handle);

                messages_sent++;

                IoTHubDeviceClient_LL_DoWork(device_ll_handle);
                ThreadAPI_Sleep(TIME_BETWEEN_MESSAGES);

            } while (IoTHub_Running);

            IoTHubDeviceClient_LL_Destroy(device_ll_handle);
        }
    }
    free(user_ctx.iothub_uri);
    free(user_ctx.device_id);
    prov_dev_security_deinit();

    // Free all the sdk subsystem
    IoTHub_Deinit();
}
//===========================
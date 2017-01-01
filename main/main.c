/* HTTP GET Example using plain POSIX sockets
   This example code is in the Public Domain (or CC0 licensed, at your option.)
   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

//Configs for GBA SIO
//  GBA_SI is our input
//  GBA_SO is our output
//  We control the clock
#define GPIO_TEST_LED GPIO_NUM_21
#define GPIO_GBA_SO GPIO_NUM_15
#define GPIO_GBA_SI GPIO_NUM_17
#define GPIO_GBA_SC GPIO_NUM_16

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID "yourssid"
#define EXAMPLE_WIFI_PASS "yourpass"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

/* Constants that aren't configurable in menuconfig */
#define WEB_SERVER "someserver"
#define WEB_PORT 80
#define WEB_URL "http://someserver/test.txt"

static const char *TAG = "example";

static const char *REQUEST = "GET " WEB_URL " HTTP/1.1\n"
    "Host: "WEB_SERVER"\n"
    "User-Agent: esp-idf/1.0 esp32\n"
    "\n";

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

char recv_buf[64];
void gba_init_task(void *pvParameter);
unsigned int gba_xfer32(unsigned int to_send);

static void http_get_task(void *pvParameters)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    
    xTaskCreate(&gba_init_task, "gba_init_task", 512, NULL, 5, NULL);

    while(1) {
        /* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
        xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                            false, true, portMAX_DELAY);
        ESP_LOGI(TAG, "Connected to AP");

        int err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_RATE_MS);
            continue;
        }

        /* Code to print the resolved IP.
           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_RATE_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket\r\n");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_RATE_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_RATE_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        //Get on the same page with the GBA
        while(gba_xfer32(0xDEADB00F) != 0xDEADB00F);
        
        /* Read HTTP response */
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            
            for(int i = 0; i < r; i++) {
                putchar(recv_buf[i]);
                
                if(i % 4 == 0)
                    gba_xfer32(*(unsigned int*)(&recv_buf[i])); //Send 32 bits
            }
        } while(r > 0);

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d\r\n", r, errno);
        close(s);
        for(int countdown = 2; countdown >= 0; countdown--) {
            ESP_LOGI(TAG, "%d... ", countdown);
            vTaskDelay(1000 / portTICK_RATE_MS);
        }
        ESP_LOGI(TAG, "Starting again!");
    }
}

void gba_init_task(void *pvParameter)
{
    gpio_pad_select_gpio(GPIO_TEST_LED);
    gpio_set_direction(GPIO_TEST_LED, GPIO_MODE_OUTPUT);
    
    gpio_pad_select_gpio(GPIO_GBA_SC);
    gpio_set_direction(GPIO_GBA_SC, GPIO_MODE_OUTPUT);
    
    gpio_pad_select_gpio(GPIO_GBA_SO);
    gpio_set_direction(GPIO_GBA_SO, GPIO_MODE_OUTPUT);
    
    gpio_pad_select_gpio(GPIO_GBA_SI);
    gpio_set_direction(GPIO_GBA_SI, GPIO_MODE_INPUT);
    
    //CLK defaults high
    gpio_set_level(GPIO_TEST_LED, 1);
    gpio_set_level(GPIO_GBA_SC, 1);
    gpio_set_level(GPIO_GBA_SO, 1);
    
    vTaskDelete( NULL );
}

unsigned int gba_xfer32(unsigned int to_send)
{
    // Wait for the GBA to time out from whatever it's
    // trying to do, if it's still trying to do a
    // transfer for some reason.
    int attempts = 0;
    while(gpio_get_level(GPIO_GBA_SI))
    {
        vTaskDelay((1 / portTICK_RATE_MS) / 2);
         if(!gpio_get_level(GPIO_GBA_SI)) break; //If they're not busy, break
         
         if(attempts++ > 10000) break;
    }

    unsigned int to_ret = 0x0;      
    for(int i = 0; i < 32; i++)
    {
        gpio_set_level(GPIO_GBA_SO, (to_send >> (31-i)) & 1);
        gpio_set_level(GPIO_TEST_LED, (to_send >> (31-i)) & 1);
                
        gpio_set_level(GPIO_GBA_SC, 0);
        for(int j = 0; j < 10; j++)
            asm("nop");
            
        gpio_set_level(GPIO_GBA_SC, 1);
        to_ret |= gpio_get_level(GPIO_GBA_SI) << (31-i);
        for(int j = 0; j < 10; j++)
            asm("nop");
    }
            
    gpio_set_level(GPIO_TEST_LED, 1);
    gpio_set_level(GPIO_GBA_SC, 1);
    gpio_set_level(GPIO_GBA_SO, 1);
    
    return to_ret;
}

void app_main()
{
    nvs_flash_init();
    initialise_wifi();
    xTaskCreate(&http_get_task, "http_get_task", 2048, NULL, 5, NULL);
}

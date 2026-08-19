#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <libusb.h>
#include <mosquitto.h>

#define VID 0x2207
#define PID 0x0013
#define USB_HEADER_BYTE 0xAA
#define USB_CMD_READ_DI 0x01
#define USB_CMD_WRITE_DO 0x02
#define USB_CMD_READ_AI 0x03
#define USB_CMD_READ_DO 0x05
#define USB_CMD_GET_BOARD_TYPE 0x0B

volatile bool keep_running = true;
volatile bool is_connected = false;

struct Config {
    char broker[256];
    int port;
    char topic_query[256];
    char topic_status[256];
    char topic_interrupt[256];
    char topic_write[256];
} config;

libusb_context *ctx = NULL;
libusb_device_handle *dev_handle = NULL;
int ep_out = -1, ep_in = -1, ep_intr = -1;
int claimed_interface = -1;
uint8_t board_type_id = 255;
struct mosquitto *mosq = NULL;
pthread_mutex_t usb_mutex = PTHREAD_MUTEX_INITIALIZER;

void sig_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        printf("\n🧹 收到中斷訊號，準備結束程式...\n");
        keep_running = false;
        is_connected = false;
    }
}

// Returns true if a libusb error code means the device is gone (disconnected)
bool usb_device_gone(int r) {
    return r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO ||
           r == LIBUSB_ERROR_PIPE || r == LIBUSB_ERROR_ACCESS;
}

// Handle a libusb transfer error: trigger reconnection on device loss,
// silently ignore timeouts, print other errors.
void handle_usb_error(int r, const char *action) {
    if (r == 0) return;
    if (usb_device_gone(r)) {
        printf("💔 設備已斷線 (%s)，觸發自動重連...\n", action);
        is_connected = false;
    } else if (r != LIBUSB_ERROR_TIMEOUT) {
        printf("⚠️ %s USB 通訊錯誤: %s\n", action, libusb_error_name(r));
    }
}

// Minimal string-based JSON parsing for config
void load_config() {
    FILE *f = fopen("mqtt_config.json", "r");
    if (!f) {
        printf("❌ 讀取 mqtt_config.json 失敗\n");
        exit(1);
    }
    
    char buffer[4096];
    fread(buffer, 1, sizeof(buffer)-1, f);
    fclose(f);
    
    char *ptr;
    if ((ptr = strstr(buffer, "\"broker\""))) sscanf(ptr, "\"broker\" : \" %255[^\"]", config.broker);
    if ((ptr = strstr(buffer, "\"port\""))) sscanf(ptr, "\"port\" : %d", &config.port);
    if ((ptr = strstr(buffer, "\"topic_query\""))) sscanf(ptr, "\"topic_query\" : \" %255[^\"]", config.topic_query);
    if ((ptr = strstr(buffer, "\"topic_status\""))) sscanf(ptr, "\"topic_status\" : \" %255[^\"]", config.topic_status);
    if ((ptr = strstr(buffer, "\"topic_interrupt\""))) sscanf(ptr, "\"topic_interrupt\" : \" %255[^\"]", config.topic_interrupt);
    if ((ptr = strstr(buffer, "\"topic_write\""))) sscanf(ptr, "\"topic_write\" : \" %255[^\"]", config.topic_write);
    
    printf("✅ 成功載入 MQTT 設定檔\n");
}

void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    if (rc == 0) {
        printf("✅ 成功連接到 MQTT Broker\n");
        mosquitto_subscribe(mosq, NULL, config.topic_query, 0);
        mosquitto_subscribe(mosq, NULL, config.topic_write, 0);
        printf("📡 已訂閱主題: %s 與 %s\n", config.topic_query, config.topic_write);
    } else {
        printf("❌ 連接 MQTT Broker 失敗\n");
    }
}

void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    if (!is_connected || ep_out == -1 || ep_in == -1) return;
    
    char payload[256];
    snprintf(payload, sizeof(payload), "%.*s", msg->payloadlen, (char*)msg->payload);
    
    if (strcmp(msg->topic, config.topic_query) == 0) {
        int ch = 0;
        char *ptr = strstr(payload, "\"channel\"");
        if (ptr) sscanf(ptr, "\"channel\" : %d", &ch);
        
        pthread_mutex_lock(&usb_mutex);
        if (board_type_id >= 5 && board_type_id <= 7) {
            unsigned char cmd[] = {USB_HEADER_BYTE, USB_CMD_READ_AI, (unsigned char)ch, 0x00};
            int actual_len;
            int r = libusb_bulk_transfer(dev_handle, ep_out, cmd, sizeof(cmd), &actual_len, 500);
            if (r == 0) {
                unsigned char resp[64];
                r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500);
                if (r == 0) {
                    if (actual_len >= 11 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_READ_AI) {
                        double val;
                        memcpy(&val, &resp[3], sizeof(double));
                        char res_payload[128];
                        snprintf(res_payload, sizeof(res_payload), "{\"channel\": %d, \"type\": \"AI\", \"value\": %.4f}", ch, val);
                        mosquitto_publish(mosq, NULL, config.topic_status, strlen(res_payload), res_payload, 0, false);
                    }
                } else {
                    handle_usb_error(r, "Query AI");
                }
            } else {
                handle_usb_error(r, "Query AI");
            }
        } else {
            char target[16] = "DI";
            ptr = strstr(payload, "\"target\"");
            if (ptr) sscanf(ptr, "\"target\" : \"%15[^\"]", target);
            
            if (strcmp(target, "DO") == 0) {
                unsigned char cmd[] = {USB_HEADER_BYTE, USB_CMD_READ_DO, (unsigned char)ch, 0x00};
                int actual_len;
                int r = libusb_bulk_transfer(dev_handle, ep_out, cmd, sizeof(cmd), &actual_len, 500);
                if (r == 0) {
                    unsigned char resp[64];
                    r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500);
                    if (r == 0) {
                        if (actual_len >= 4 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_READ_DO) {
                            int state = resp[3];
                            char res_payload[128];
                            snprintf(res_payload, sizeof(res_payload), "{\"channel\": %d, \"type\": \"DO\", \"state\": %d}", ch, state);
                            mosquitto_publish(mosq, NULL, config.topic_status, strlen(res_payload), res_payload, 0, false);
                        }
                    } else {
                        handle_usb_error(r, "Query DO");
                    }
                } else {
                    handle_usb_error(r, "Query DO");
                }
            } else {
                unsigned char cmd[] = {USB_HEADER_BYTE, USB_CMD_READ_DI, (unsigned char)ch, 0x00};
                int actual_len;
                int r = libusb_bulk_transfer(dev_handle, ep_out, cmd, sizeof(cmd), &actual_len, 500);
                if (r == 0) {
                    unsigned char resp[64];
                    r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500);
                    if (r == 0) {
                        if (actual_len >= 4 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_READ_DI) {
                            int state = resp[3];
                            char res_payload[128];
                            snprintf(res_payload, sizeof(res_payload), "{\"channel\": %d, \"type\": \"DI\", \"state\": %d}", ch, state);
                            mosquitto_publish(mosq, NULL, config.topic_status, strlen(res_payload), res_payload, 0, false);
                        }
                    } else {
                        handle_usb_error(r, "Query DI");
                    }
                } else {
                    handle_usb_error(r, "Query DI");
                }
            }
        }
        pthread_mutex_unlock(&usb_mutex);
    } else if (strcmp(msg->topic, config.topic_write) == 0) {
        if (board_type_id >= 5 && board_type_id <= 7) return; // Ignore for AI
        
        int ch = 0, state = 0;
        char *ptr = strstr(payload, "\"channel\"");
        if (ptr) sscanf(ptr, "\"channel\" : %d", &ch);
        ptr = strstr(payload, "\"state\"");
        if (ptr) sscanf(ptr, "\"state\" : %d", &state);
        
        pthread_mutex_lock(&usb_mutex);
        unsigned char cmd[] = {USB_HEADER_BYTE, USB_CMD_WRITE_DO, (unsigned char)ch, (unsigned char)state};
        int actual_len;
        int r = libusb_bulk_transfer(dev_handle, ep_out, cmd, sizeof(cmd), &actual_len, 500);
        if (r == 0) {
            unsigned char resp[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500);
            if (r == 0) {
                printf("✅ 成功寫入 DO %d -> %d\n", ch, state);
            } else {
                handle_usb_error(r, "Write DO");
            }
        } else {
            handle_usb_error(r, "Write DO");
        }
        pthread_mutex_unlock(&usb_mutex);
    }
}

void* interrupt_listener(void* arg) {
    unsigned char buf[64];
    int actual_length;
    
    printf("[監聽者] DI 狀態中斷監聽已啟動...\n");
    while (is_connected && keep_running) {
        if (ep_intr == -1) {
            usleep(1000000);
            continue;
        }
        int r = libusb_interrupt_transfer(dev_handle, ep_intr, buf, sizeof(buf), &actual_length, 50);
        if (r == 0 && actual_length >= 3 && buf[0] == 0xBB) {
            int ch = buf[1];
            int state = buf[2];
            printf("  ⚡ [中斷推播] DI %d -> %d\n", ch, state);
            
            char payload[128];
            snprintf(payload, sizeof(payload), "{\"channel\": %d, \"type\": \"DI\", \"state\": %d}", ch, state);
            mosquitto_publish(mosq, NULL, config.topic_interrupt, strlen(payload), payload, 0, false);
            
        } else if (r == LIBUSB_ERROR_TIMEOUT) {
            usleep(10000);
        } else if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            if (is_connected) {
                printf("[監聽者] 設備斷線或讀取中斷: %s\n", libusb_error_name(r));
                is_connected = false;
            }
            break;
        }
    }
    return NULL;
}

int main() {
    // Line-buffer stdout so logs appear in real time even when redirected
    setvbuf(stdout, NULL, _IOLBF, 0);
    load_config();
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    mosquitto_lib_init();
    mosq = mosquitto_new("rk3506_c_bridge", true, NULL);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    
    if (mosquitto_connect(mosq, config.broker, config.port, 60) != MOSQ_ERR_SUCCESS) {
        printf("❌ 無法連接 MQTT Broker\n");
        return 1;
    }
    
    mosquitto_loop_start(mosq);
    
    if (libusb_init(&ctx) < 0) return 1;

    pthread_t listener;
    bool listener_running = false;

    while (keep_running) {
        printf("\n🔍 正在尋找 RK3506 Native I/O 模組...\n");
        dev_handle = libusb_open_device_with_vid_pid(ctx, VID, PID);
        
        if (!dev_handle) {
            sleep(1);
            continue;
        }
        
        libusb_device *dev = libusb_get_device(dev_handle);
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
            libusb_close(dev_handle);
            dev_handle = NULL;
            sleep(1);
            continue;
        }
        
        int target_intf = -1;
        ep_out = -1; ep_in = -1; ep_intr = -1;
        
        for (int i = 0; i < cfg->bNumInterfaces; i++) {
            const struct libusb_interface *intf = &cfg->interface[i];
            for (int j = 0; j < intf->num_altsetting; j++) {
                const struct libusb_interface_descriptor *alt = &intf->altsetting[j];
                if (alt->bInterfaceClass == 255 && alt->bInterfaceSubClass == 0 && alt->bInterfaceProtocol == 0) {
                    target_intf = alt->bInterfaceNumber;
                    for (int k = 0; k < alt->bNumEndpoints; k++) {
                        const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT && 
                            (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                            ep_out = ep->bEndpointAddress;
                        } else if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN && 
                                   (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                            ep_in = ep->bEndpointAddress;
                        } else if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN && 
                                   (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                            ep_intr = ep->bEndpointAddress;
                        }
                    }
                    break;
                }
            }
            if (target_intf != -1) break;
        }
        
        libusb_free_config_descriptor(cfg);
        
        if (target_intf == -1 || ep_out == -1 || ep_in == -1) {
            libusb_close(dev_handle);
            dev_handle = NULL;
            sleep(1);
            continue;
        }
        
#ifdef __linux__
        if (libusb_kernel_driver_active(dev_handle, target_intf) == 1) {
            libusb_detach_kernel_driver(dev_handle, target_intf);
        }
#endif
        
        if (libusb_claim_interface(dev_handle, target_intf) < 0) {
            libusb_close(dev_handle);
            dev_handle = NULL;
            sleep(1);
            continue;
        }
        claimed_interface = target_intf;
        
        // Query board type
        unsigned char cmd[] = {USB_HEADER_BYTE, USB_CMD_GET_BOARD_TYPE, 0x00, 0x00};
        bool found_board = false;
        
        for (int attempt = 1; attempt <= 5; attempt++) {
            int actual_len;
            if (libusb_bulk_transfer(dev_handle, ep_out, cmd, sizeof(cmd), &actual_len, 500) == 0) {
                unsigned char resp[64];
                if (libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500) == 0) {
                    if (actual_len >= 4 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_GET_BOARD_TYPE) {
                        board_type_id = resp[3];
                        found_board = true;
                        break;
                    }
                }
            }
            usleep(10000);
        }
        
        if (found_board) {
            printf("✅ 成功獲取！RK3506 設備型號 ID: %d\n", board_type_id);
        } else {
            libusb_release_interface(dev_handle, claimed_interface);
            libusb_close(dev_handle);
            dev_handle = NULL;
            sleep(1);
            continue;
        }
        
        is_connected = true;
        
        if (board_type_id < 5 || board_type_id > 7) {
            if (ep_intr != -1 && !listener_running) {
                pthread_create(&listener, NULL, interrupt_listener, NULL);
                listener_running = true;
            }
        }
        
        printf("🚀 MQTT Bridge 運行中 (按 Ctrl+C 終止)...\n");
        while (is_connected && keep_running) {
            sleep(1);
        }
        
        is_connected = false;
        if (listener_running) {
            pthread_join(listener, NULL);
            listener_running = false;
        }
        
        if (claimed_interface != -1 && dev_handle) {
            libusb_release_interface(dev_handle, claimed_interface);
        }
        if (dev_handle) {
            libusb_close(dev_handle);
            dev_handle = NULL;
        }
        sleep(3);
    }
    
    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    libusb_exit(ctx);
    
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libusb-1.0/libusb.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <signal.h>

#define VID 0x2207
#define PID 0x0013
#define USB_HEADER_BYTE 0xAA
#define USB_CMD_READ_DI 0x01
#define USB_CMD_WRITE_DO 0x02
#define USB_CMD_READ_AI 0x03
#define USB_CMD_SET_AI_MODE 0x08
#define USB_CMD_GET_BOARD_TYPE 0x0B
#define NUM_CHANNELS 8
#define ITERATIONS 500

const char* get_board_name(uint8_t type_id) {
    switch (type_id) {
        case 0: return "DI8DO8";
        case 1: return "DI16";
        case 2: return "DO16";
        case 3: return "DI8RELAY4";
        case 4: return "DI8DO6PWM2";
        case 5: return "AIAO";
        case 6: return "AI8";
        case 7: return "AO8";
        default: return "未知型號";
    }
}

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec) * 1000.0 + (tv.tv_usec) / 1000.0;
}

volatile bool keep_running = true;
void int_handler(int dummy) {
    keep_running = false;
    printf("\n收到中斷訊號...\n");
}

struct ThreadData {
    libusb_device_handle *dev_handle;
    uint8_t ep_intr;
};

volatile bool is_connected = false;

void *listen_for_interrupts(void *arg) {
    struct ThreadData *data = (struct ThreadData *)arg;
    unsigned char buf[64];
    int actual_len;
    
    printf("[監聽者] DI 狀態中斷監聽已啟動...\n");
    
    while (is_connected && keep_running) {
        int r = libusb_interrupt_transfer(data->dev_handle, data->ep_intr, buf, sizeof(buf), &actual_len, 50);
        if (r == 0 && actual_len >= 3) {
            if (buf[0] == 0xBB) {
                printf("  ⚡ [中斷推播] DI %d -> %d\n", buf[1], buf[2]);
            }
        } else if (r == LIBUSB_ERROR_TIMEOUT) {
            // continue
        } else {
            // Error
            break;
        }
    }
    return NULL;
}

void run_ai_loop(libusb_device_handle *dev_handle, uint8_t ep_out, uint8_t ep_in) {
    printf("\n=======================================================\n");
    printf("🚀 進入 AI/AO 測試模式\n");
    printf("=======================================================\n");
    
    printf("正在設定 AI 通道模式為 Voltage (0x00)...\n");
    bool mode_setup_supported = true;
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        if (!mode_setup_supported) break;
        
        unsigned char cmd[4] = {USB_HEADER_BYTE, USB_CMD_SET_AI_MODE, ch, 0x00};
        int actual_len;
        int r = libusb_bulk_transfer(dev_handle, ep_out, cmd, 4, &actual_len, 200);
        if (r != 0) {
            printf("⚠️ 警告: 通道 %d 模式設定寫入失敗 (%s)\n", ch, libusb_error_name(r));
            mode_setup_supported = false;
            usleep(500000);
            break;
        }
        
        unsigned char resp[64];
        r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 200);
        if (r != 0 || actual_len < 4 || resp[0] != USB_HEADER_BYTE || resp[1] != USB_CMD_SET_AI_MODE) {
            printf("⚠️ 警告: 通道 %d 模式設定回應異常\n", ch);
        }
    }
    
    int success_count = 0, error_count = 0;
    double latest_ai_vals[NUM_CHANNELS] = {0};
    unsigned char latest_payloads[NUM_CHANNELS][8] = {0};
    
    printf("\n[🚀 AI 8 通道讀取測試] 準備執行 %d 次全通道循環...\n", ITERATIONS);
    double start_time = get_time_ms();
    
    for (int i = 0; i < ITERATIONS && keep_running; i++) {
        if (i > 0 && i % 50 == 0) printf("目前進度: %d / %d 全通道循環...\n", i, ITERATIONS);
        
        for (int ch = 0; ch < NUM_CHANNELS && keep_running; ch++) {
            unsigned char cmd_read[] = {USB_HEADER_BYTE, USB_CMD_READ_AI, ch, 0x00};
            int actual_len = 0;
            int r = libusb_bulk_transfer(dev_handle, ep_out, cmd_read, 4, &actual_len, 100);
            if (r != 0) {
                error_count++;
                printf("[USB 錯誤] AI 通道 %d 寫入失敗\n", ch);
                continue;
            }
            
            unsigned char resp[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 100);
            if (r == 0 && actual_len >= 11 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_READ_AI) {
                success_count++;
                double val;
                memcpy(&val, &resp[3], 8);
                latest_ai_vals[ch] = val;
                memcpy(latest_payloads[ch], &resp[3], 8);
            } else {
                error_count++;
            }
            usleep(1000); // 1ms
        }
    }
    
    double total_time_s = (get_time_ms() - start_time) / 1000.0;
    double tps = (total_time_s > 0) ? (success_count / total_time_s) : 0;
    
    printf("-------------------------------------------------------\n");
    printf("📊 AI 通道讀取與壓力測試報告 (AI Channels Test Report)\n");
    printf("-------------------------------------------------------\n");
    printf("總耗時          : %.4f 秒\n", total_time_s);
    printf("測試總通道請求  : %d 次\n", ITERATIONS * NUM_CHANNELS);
    printf("成功次數        : %d 次\n", success_count);
    printf("失敗/超時 (USB) : %d 次\n", error_count);
    printf("-------------------------------------------------------\n");
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        printf("  AI%d 通道 -> 數值: %10.4f | 封包[3:11]: ", ch, latest_ai_vals[ch]);
        for(int k=0; k<8; k++) printf("%02X ", latest_payloads[ch][k]);
        printf("\n");
    }
    printf("-------------------------------------------------------\n");
    printf("🚀 每秒實際 I/O 吞吐量 (TPS) : %.2f 次/秒\n", tps);
}

void run_dodi_loop(libusb_device_handle *dev_handle, uint8_t ep_out, uint8_t ep_in, uint8_t ep_intr) {
    printf("\n=======================================================\n");
    printf("🚀 進入 DO/DI 測試模式\n");
    printf("=======================================================\n");
    
    is_connected = true;
    pthread_t listener;
    struct ThreadData t_data = {dev_handle, ep_intr};
    
    if (ep_intr != 0) {
        pthread_create(&listener, NULL, listen_for_interrupts, &t_data);
        usleep(500000);
    } else {
        printf("⚠️ 找不到 Interrupt IN 端點，中斷監聽可能無效。\n");
    }
    
    int success_count = 0, error_count = 0, match_count = 0, mismatch_count = 0;
    int consecutive_timeouts = 0;
    
    printf("\n[🚀 8 通道迴圈驗證測試] 準備執行 %d 次全通道循環...\n", ITERATIONS);
    double start_time = get_time_ms();
    
    for (int i = 0; i < ITERATIONS && is_connected && keep_running; i++) {
        if (i > 0 && i % 50 == 0) printf("目前進度: %d / %d 全通道循環...\n", i, ITERATIONS);
        
        for (int ch = 0; ch < NUM_CHANNELS && is_connected && keep_running; ch++) {
            uint8_t target_state = (i + ch) % 2;
            
            unsigned char cmd_do[] = {USB_HEADER_BYTE, USB_CMD_WRITE_DO, ch, target_state};
            int actual_len = 0;
            int r = libusb_bulk_transfer(dev_handle, ep_out, cmd_do, 4, &actual_len, 50);
            if (r != 0) goto dodi_error;
            
            unsigned char resp_do[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp_do, sizeof(resp_do), &actual_len, 50);
            if (r != 0) goto dodi_error;
            
            usleep(10000); // 10ms settling time
            
            unsigned char cmd_di[] = {USB_HEADER_BYTE, USB_CMD_READ_DI, ch, 0x00};
            r = libusb_bulk_transfer(dev_handle, ep_out, cmd_di, 4, &actual_len, 50);
            if (r != 0) goto dodi_error;
            
            unsigned char resp_di[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp_di, sizeof(resp_di), &actual_len, 50);
            if (r != 0) goto dodi_error;
            
            if (actual_len >= 4 && resp_di[0] == USB_HEADER_BYTE && resp_di[1] == USB_CMD_READ_DI) {
                success_count++;
                if (resp_di[3] == target_state) {
                    match_count++;
                } else {
                    mismatch_count++;
                    if (mismatch_count <= 10) {
                        printf("[警告] 通道 %d 狀態不吻合！寫入: %d, 讀取: %d\n", ch, target_state, resp_di[3]);
                    }
                }
            } else {
                error_count++;
            }
            usleep(1000);
            consecutive_timeouts = 0;
            continue;
            
        dodi_error:
            error_count++;
            consecutive_timeouts++;
            if (consecutive_timeouts >= 8) {
                printf("💥 偵測到 Bulk 通道連續超時失去回應！\n");
                is_connected = false;
                break;
            }
        }
    }
    
    is_connected = false;
    if (ep_intr != 0) {
        pthread_join(listener, NULL);
    }
    
    double total_time_s = (get_time_ms() - start_time) / 1000.0;
    double tps = (total_time_s > 0) ? ((success_count * 2) / total_time_s) : 0;
    
    printf("-------------------------------------------------------\n");
    printf("📊 全通道 DO/DI 迴圈比對測試報告\n");
    printf("-------------------------------------------------------\n");
    printf("總耗時          : %.4f 秒\n", total_time_s);
    printf("測試總通道次數  : %d 次\n", success_count);
    printf("失敗/超時 (USB) : %d 次\n", error_count);
    printf("-------------------------------------------------------\n");
    printf("✅ 狀態完全吻合  : %d 次\n", match_count);
    if (mismatch_count == 0) {
        printf("❌ 狀態不吻合    : %d 次 (完美！)\n", mismatch_count);
    } else {
        printf("❌ 狀態不吻合    : %d 次\n", mismatch_count);
    }
    printf("-------------------------------------------------------\n");
    printf("🚀 每秒實際 TPS : %.2f 次傳輸/秒\n", tps);
}

int main_loop(libusb_context *ctx) {
    libusb_device_handle *dev_handle = NULL;
    int r;
    
    printf("\n🔍 正在尋找 RK3506 Native I/O 模組...\n");
    dev_handle = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (dev_handle == NULL) {
        sleep(1);
        return 0;
    }

    libusb_device *dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor *config;
    r = libusb_get_active_config_descriptor(dev, &config);
    if (r < 0) {
        printf("❌ 取得 Config 失敗\n");
        libusb_close(dev_handle);
        return 1;
    }

    int target_intf = -1;
    uint8_t ep_in = 0, ep_out = 0, ep_intr = 0;

    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *intf = &config->interface[i];
        for (int j = 0; j < intf->num_altsetting; j++) {
            const struct libusb_interface_descriptor *desc = &intf->altsetting[j];
            if (desc->bInterfaceClass == 255 && desc->bInterfaceSubClass == 0 && desc->bInterfaceProtocol == 0) {
                target_intf = desc->bInterfaceNumber;
                for (int k = 0; k < desc->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor *ep = &desc->endpoint[k];
                    if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
                            ep_intr = ep->bEndpointAddress;
                        } else {
                            ep_in = ep->bEndpointAddress;
                        }
                    } else {
                        ep_out = ep->bEndpointAddress;
                    }
                }
                break;
            }
        }
        if (target_intf != -1) break;
    }
    libusb_free_config_descriptor(config);

    if (target_intf == -1) {
        printf("❌ 找不到 Class 255 的自訂 I/O Interface\n");
        libusb_close(dev_handle);
        return 1;
    }

#ifdef __linux__
    if (libusb_kernel_driver_active(dev_handle, target_intf) == 1) {
        libusb_detach_kernel_driver(dev_handle, target_intf);
    }
#endif

    r = libusb_claim_interface(dev_handle, target_intf);
    if (r < 0) {
        printf("❌ 無法接管介面\n");
        libusb_close(dev_handle);
        return 1;
    }

    printf("🔍 正在向設備發送 UsbCmdGetBoardType (0x0B)...\n");
    unsigned char cmd[4] = {USB_HEADER_BYTE, USB_CMD_GET_BOARD_TYPE, 0x00, 0x00};
    int actual_len;
    r = libusb_bulk_transfer(dev_handle, ep_out, cmd, 4, &actual_len, 500);
    
    unsigned char resp[64];
    r = libusb_bulk_transfer(dev_handle, ep_in, resp, sizeof(resp), &actual_len, 500);
    
    uint8_t board_id = 255;
    if (r == 0 && actual_len >= 4 && resp[0] == USB_HEADER_BYTE && resp[1] == USB_CMD_GET_BOARD_TYPE) {
        board_id = resp[3];
        printf("✅ 成功獲取！RK3506 設備回報之 I/O Board Type 為: %s (ID: %d)\n", get_board_name(board_id), board_id);
    } else {
        printf("❌ 無法獲取板卡型號\n");
        libusb_release_interface(dev_handle, target_intf);
        libusb_close(dev_handle);
        return 1;
    }

    if (board_id >= 5 && board_id <= 7) {
        run_ai_loop(dev_handle, ep_out, ep_in);
    } else {
        run_dodi_loop(dev_handle, ep_out, ep_in, ep_intr);
    }

    libusb_release_interface(dev_handle, target_intf);
    libusb_close(dev_handle);
    return 0;
}

int main() {
    signal(SIGINT, int_handler);
    libusb_context *ctx = NULL;
    int r;
    
    printf("🚀 啟動 C USB Unified 測試工具 (libusb)\n");

    r = libusb_init(&ctx);
    if (r < 0) return 1;

    while (keep_running) {
        int ret = main_loop(ctx);
        if (ret == 0) {
            printf("\n✅ 測試圓滿結束！程式安全退出。\n");
            break;
        }
        if (keep_running) {
            sleep(1);
        }
    }

    libusb_exit(ctx);
    return 0;
}

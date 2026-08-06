#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <libusb-1.0/libusb.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define VID 0x2207
#define PID 0x0013
#define NUM_CHANNELS 8
#define ITERATIONS 500
#define USB_HEADER_BYTE 0xAA
#define USB_CMD_READ_AI 0x03
#define USB_CMD_SET_AI_MODE 0x08

volatile bool keep_running = true;
volatile bool is_connected = false;

libusb_context *ctx = NULL;
libusb_device_handle *dev_handle = NULL;
int claimed_interface = -1;

typedef struct {
    double val;
    unsigned char payload[8];
} AIData;

void sig_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        printf("\n🧹 收到中斷訊號，準備結束程式...\n");
        keep_running = false;
        is_connected = false;
    }
}

double get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec) * 1000.0 + (tv.tv_usec) / 1000.0;
}

// Returns:
//  0: Completed successfully
// -1: Device not found
// -2: Disconnected or I/O error
int run_ai_loop() {
    printf("\n🔍 正在尋找 RK3506 Native I/O 模組...\n");
    
    dev_handle = libusb_open_device_with_vid_pid(ctx, VID, PID);
    if (!dev_handle) {
        return -1; // Not found
    }
    
    libusb_device *dev = libusb_get_device(dev_handle);
    struct libusb_config_descriptor *config = NULL;
    int r = libusb_get_active_config_descriptor(dev, &config);
    if (r < 0) {
        printf("獲取 Configuration 失敗: %s\n", libusb_error_name(r));
        libusb_close(dev_handle);
        dev_handle = NULL;
        return -2;
    }
    
    int target_intf = -1;
    int ep_out = -1, ep_in = -1;
    
    for (int i = 0; i < config->bNumInterfaces; i++) {
        const struct libusb_interface *intf = &config->interface[i];
        for (int j = 0; j < intf->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &intf->altsetting[j];
            if (alt->bInterfaceClass == 255 && alt->bInterfaceSubClass == 0 && alt->bInterfaceProtocol == 0) {
                if (target_intf == -1 || alt->bInterfaceNumber < target_intf) {
                    target_intf = alt->bInterfaceNumber;
                }
                
                for (int k = 0; k < alt->bNumEndpoints; k++) {
                    const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];
                    if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT && 
                        (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                        ep_out = ep->bEndpointAddress;
                    } else if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN && 
                               (ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
                        ep_in = ep->bEndpointAddress;
                    }
                }
                break;
            }
        }
        if (target_intf != -1) break;
    }
    
    libusb_free_config_descriptor(config);
    
    if (target_intf == -1) {
        printf("找不到 Native I/O 介面\n");
        libusb_close(dev_handle);
        dev_handle = NULL;
        return -2;
    }
    
    if (ep_out == -1 || ep_in == -1) {
        printf("找不到正確的端點！\n");
        libusb_close(dev_handle);
        dev_handle = NULL;
        return -2;
    }
    
    printf("✅ 成功找到 Native I/O 介面，編號: Interface %d\n", target_intf);
    
#ifdef __linux__
    if (libusb_kernel_driver_active(dev_handle, target_intf) == 1) {
        libusb_detach_kernel_driver(dev_handle, target_intf);
        printf("已解除 Linux 預設驅動對介面的佔用\n");
    }
#elif defined(__APPLE__)
    // For macOS, libusb handles things mostly natively without kernel driver detaching,
    // just proceed to claim interface
#endif
    
    r = libusb_claim_interface(dev_handle, target_intf);
    if (r < 0) {
        printf("⚠️ 無法強制佔用介面: %s，這可能導致後續權限不足。\n", libusb_error_name(r));
    } else {
        claimed_interface = target_intf;
    }
    
    is_connected = true;
    
    // =========================================================
    // 初始化 AI 通道模式 (0x00: Voltage)
    // =========================================================
    printf("正在設定 AI 通道模式為 Voltage (0x00)...\n");
    bool mode_setup_supported = true;
    for (int ch = 0; ch < NUM_CHANNELS; ch++) {
        if (!mode_setup_supported) break;
        unsigned char cmd_mode[] = {USB_HEADER_BYTE, USB_CMD_SET_AI_MODE, ch, 0x00};
        int actual_len = 0;
        int r_mode = libusb_bulk_transfer(dev_handle, ep_out, cmd_mode, sizeof(cmd_mode), &actual_len, 200);
        if (r_mode != 0) {
            printf("⚠️ 警告: 通道 %d 模式設定寫入失敗: %s\n", ch, libusb_error_name(r_mode));
            continue;
        }

        unsigned char resp_mode[64];
        r_mode = libusb_bulk_transfer(dev_handle, ep_in, resp_mode, sizeof(resp_mode), &actual_len, 200);
        if (r_mode != 0) {
            printf("⚠️ 警告: 通道 %d 模式設定超時/失敗 (%s)。\n", ch, libusb_error_name(r_mode));
            printf("   -> 可能是 RK3506 設備上的韌體尚未更新支援 0x08 指令。將跳過模式設定以避免中斷通訊。\n");
            mode_setup_supported = false;
            libusb_clear_halt(dev_handle, ep_in);
            libusb_clear_halt(dev_handle, ep_out);
            usleep(500000); // 0.5s
            continue;
        }
        if (actual_len < 4 || resp_mode[0] != USB_HEADER_BYTE || resp_mode[1] != USB_CMD_SET_AI_MODE) {
            printf("⚠️ 警告: 通道 %d 模式設定可能未成功\n", ch);
        }
    }

    printf("\n[🚀 AI (Analog Input) 8 通道讀取測試] 準備執行 %d 次全通道循環...\n", ITERATIONS);
    printf("測試進行中，請稍候...\n\n");
    
    int success_count = 0, error_count = 0;
    int consecutive_timeouts = 0;
    int total_bytes_transferred = 0;
    bool aborted = false;
    AIData latest_ai_data[NUM_CHANNELS] = {0};
    
    double start_time = get_time_ms();
    
    for (int i = 0; i < ITERATIONS && is_connected && keep_running; i++) {
        if (i > 0 && i % 50 == 0) {
            printf("目前進度: %d / %d 全通道循環...\n", i, ITERATIONS);
        }
        
        for (int ch = 0; ch < NUM_CHANNELS && is_connected && keep_running; ch++) {
            // Read AI
            unsigned char cmd_ai[] = {USB_HEADER_BYTE, USB_CMD_READ_AI, ch, 0x00};
            int actual_len = 0;
            r = libusb_bulk_transfer(dev_handle, ep_out, cmd_ai, sizeof(cmd_ai), &actual_len, 100);
            if (r != 0) {
                error_count++;
                consecutive_timeouts++;
                if (consecutive_timeouts >= 8) {
                    printf("Bulk 寫入連續失敗\n");
                    aborted = true;
                    break;
                }
                continue;
            }
            
            unsigned char resp_ai[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp_ai, sizeof(resp_ai), &actual_len, 100);
            if (r != 0) {
                if (r == LIBUSB_ERROR_TIMEOUT) {
                    error_count++;
                    consecutive_timeouts++;
                    if (consecutive_timeouts >= 8) {
                        aborted = true;
                        break;
                    }
                    continue;
                } else {
                    error_count++;
                    printf("[USB 錯誤] AI 通道 %d 傳輸失敗: %s\n", ch, libusb_error_name(r));
                    continue;
                }
            }
            
            consecutive_timeouts = 0;
            
            if (actual_len >= 11 && resp_ai[0] == USB_HEADER_BYTE && resp_ai[1] == USB_CMD_READ_AI) {
                success_count++;
                total_bytes_transferred += sizeof(cmd_ai) + actual_len;
                
                // Decode double float (little-endian)
                double ai_value = 0;
                memcpy(&ai_value, &resp_ai[3], 8);
                
                latest_ai_data[ch].val = ai_value;
                memcpy(latest_ai_data[ch].payload, &resp_ai[3], 8);
            } else {
                error_count++;
            }
            
            usleep(1000); // 1ms
        }
        
        if (aborted) {
            is_connected = false;
            break;
        }
    }
    
    double total_time = (get_time_ms() - start_time) / 1000.0;
    
    if (keep_running && is_connected && !aborted) {
        int total_requests = ITERATIONS * NUM_CHANNELS;
        double tps = success_count / (total_time > 0 ? total_time : 1.0);
        double avg_latency_ms = (total_requests > 0) ? ((total_time * 1000.0) / total_requests) : 0.0;
        
        printf("-------------------------------------------------------\n");
        printf("📊 AI 通道讀取與壓力測試報告 (AI Channels Test Report - C libusb)\n");
        printf("-------------------------------------------------------\n");
        printf("總耗時          : %.4f 秒\n", total_time);
        printf("測試總通道請求  : %d 次 (%d 輪 x %d 通道)\n", total_requests, ITERATIONS, NUM_CHANNELS);
        printf("成功次數        : %d 次\n", success_count);
        printf("失敗/超時 (USB) : %d 次\n", error_count);
        printf("總傳輸資料量    : %d Bytes\n", total_bytes_transferred);
        printf("-------------------------------------------------------\n");
        printf("📈 最新各 AI 通道採樣電壓/電流值:\n");
        for (int ch = 0; ch < NUM_CHANNELS; ch++) {
            printf("  AI%d 通道 -> 數值: %10.4f | 封包[3:11]: [", ch, latest_ai_data[ch].val);
            for (int i = 0; i < 8; i++) {
                printf("%02X%s", latest_ai_data[ch].payload[i], i < 7 ? " " : "");
            }
            printf("]\n");
        }
        printf("-------------------------------------------------------\n");
        printf("⏱️ 單通道採樣延遲            : %.4f 毫秒/通道\n", avg_latency_ms);
        printf("🚀 每秒實際 I/O 吞吐量 (TPS) : %.2f 次/秒\n", tps);
        printf("-------------------------------------------------------\n");
    }
    
    is_connected = false;
    
    printf("🧹 正在歸還設備控制權並清理資源...\n");
    if (claimed_interface != -1) {
        libusb_release_interface(dev_handle, claimed_interface);
        claimed_interface = -1;
    }
    libusb_close(dev_handle);
    dev_handle = NULL;
    
    if (!keep_running) {
        return 0; // Exited cleanly via Ctrl+C
    }
    
    return aborted ? -2 : 0;
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    printf("🚀 啟動 C USB Host AI 測試工具 (libusb)\n");
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    int r = libusb_init(&ctx);
    if (r < 0) {
        printf("libusb init failed: %s\n", libusb_error_name(r));
        return 1;
    }
    
    while (keep_running) {
        int res = run_ai_loop();
        
        if (!keep_running) break;
        
        if (res == -1) {
            // Device not found
            sleep(1);
        } else if (res == -2) {
            // Disconnected or IO error
            printf("💥 [主程式] 偵測到 USB 斷線或發生錯誤\n");
            printf("⏳ 等待 5 秒後啟動自動重連機制...\n");
            sleep(5);
        } else {
            break;
        }
    }
    
    // Cleanup if exited abruptly
    if (dev_handle) {
        if (claimed_interface != -1) {
            libusb_release_interface(dev_handle, claimed_interface);
        }
        libusb_close(dev_handle);
    }
    libusb_exit(ctx);
    printf("程式結束\n");
    return 0;
}

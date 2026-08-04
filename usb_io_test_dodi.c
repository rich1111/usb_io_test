//go:build ignore
// +build ignore

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <libusb-1.0/libusb.h>

#define VID 0x2207
#define PID 0x0013
#define NUM_CHANNELS 8
#define ITERATIONS 500
#define USB_HEADER_BYTE 0xAA
#define USB_CMD_READ_DI 0x01
#define USB_CMD_WRITE_DO 0x02

volatile bool keep_running = true;
volatile bool is_connected = false;

libusb_context *ctx = NULL;
libusb_device_handle *dev_handle = NULL;
int claimed_interface = -1;

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

struct ThreadData {
    libusb_device_handle *dev;
    int ep_intr;
};

void* listen_for_interrupts(void* arg) {
    struct ThreadData *data = (struct ThreadData*)arg;
    unsigned char buf[64];
    int actual_length;
    
    printf("[監聽者] DI 狀態中斷監聽已啟動...\n");
    while (is_connected && keep_running) {
        int r = libusb_interrupt_transfer(data->dev, data->ep_intr, buf, sizeof(buf), &actual_length, 50);
        if (r == 0 && actual_length >= 3 && buf[0] == 0xBB) {
            printf("  ⚡ [中斷推播] DI %d -> %d\n", buf[1], buf[2]);
        } else if (r == LIBUSB_ERROR_TIMEOUT) {
            usleep(10000); // 10ms
        } else if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            // Only print if we think we are still connected, to avoid spam on shutdown
            if (is_connected) {
                printf("[監聽者] 設備斷線或讀取中斷: %s\n", libusb_error_name(r));
            }
            break;
        }
    }
    return NULL;
}

// Returns:
//  0: Completed successfully
// -1: Device not found
// -2: Disconnected or I/O error
int run_daq_loop() {
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
    int ep_out = -1, ep_in = -1, ep_intr = -1;
    
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
    
    libusb_free_config_descriptor(config);
    
    if (target_intf == -1) {
        printf("找不到 Native I/O 介面\n");
        libusb_close(dev_handle);
        dev_handle = NULL;
        return -2;
    }
    
    if (ep_out == -1 || ep_in == -1 || ep_intr == -1) {
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
    
    pthread_t listener;
    struct ThreadData t_data = {dev_handle, ep_intr};
    pthread_create(&listener, NULL, listen_for_interrupts, &t_data);
    usleep(500000); // Wait 0.5s for listener to start up
    
    printf("\n[🚀 8 通道迴圈驗證測試] 準備執行 %d 次全通道循環...\n", ITERATIONS);
    
    int success_count = 0, error_count = 0, match_count = 0, mismatch_count = 0;
    int consecutive_timeouts = 0;
    bool aborted = false;
    
    double start_time = get_time_ms();
    
    for (int i = 0; i < ITERATIONS && is_connected && keep_running; i++) {
        if (i > 0 && i % 50 == 0) {
            printf("目前進度: %d / %d 全通道循環...\n", i, ITERATIONS);
        }
        
        for (int ch = 0; ch < NUM_CHANNELS && is_connected && keep_running; ch++) {
            uint8_t target_state = (i + ch) % 2;
            
            // 1. Write DO
            unsigned char cmd_do[] = {USB_HEADER_BYTE, USB_CMD_WRITE_DO, ch, target_state};
            int actual_len = 0;
            r = libusb_bulk_transfer(dev_handle, ep_out, cmd_do, sizeof(cmd_do), &actual_len, 50);
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
            
            // Read DO resp
            unsigned char resp_do[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp_do, sizeof(resp_do), &actual_len, 50);
            
            // Wait for hardware to settle
            usleep(10000); // 10ms
            
            // 2. Read DI
            unsigned char cmd_di[] = {USB_HEADER_BYTE, USB_CMD_READ_DI, ch, 0x00};
            r = libusb_bulk_transfer(dev_handle, ep_out, cmd_di, sizeof(cmd_di), &actual_len, 50);
            if (r != 0) {
                error_count++;
                consecutive_timeouts++;
                if (consecutive_timeouts >= 8) {
                    aborted = true;
                    break;
                }
                continue;
            }
            
            unsigned char resp_di[64];
            r = libusb_bulk_transfer(dev_handle, ep_in, resp_di, sizeof(resp_di), &actual_len, 50);
            if (r != 0) {
                error_count++;
                consecutive_timeouts++;
                if (consecutive_timeouts >= 8) {
                    aborted = true;
                    break;
                }
                continue;
            }
            
            consecutive_timeouts = 0;
            
            if (actual_len >= 4 && resp_di[0] == USB_HEADER_BYTE && resp_di[1] == USB_CMD_READ_DI && resp_di[2] == ch) {
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
            
            usleep(1000); // 1ms
        }
        
        if (aborted) {
            is_connected = false;
            break;
        }
    }
    
    double total_time = (get_time_ms() - start_time) / 1000.0;
    
    if (keep_running && is_connected && !aborted) {
        double tps = (success_count * 2) / (total_time > 0 ? total_time : 1.0);
        printf("\n-------------------------------------------------------\n");
        printf("📊 全通道迴圈測試報告 (C libusb)\n");
        printf("-------------------------------------------------------\n");
        printf("總耗時         : %.4f 秒\n", total_time);
        printf("✅ 狀態完全吻合 : %d 次\n", match_count);
        if (mismatch_count == 0) {
            printf("❌ 狀態不吻合   : 0 次 (完美！)\n");
        } else {
            printf("❌ 狀態不吻合   : %d 次\n", mismatch_count);
        }
        printf("🚀 每秒 I/O 吞吐 : %.2f TPS\n", tps);
        printf("-------------------------------------------------------\n");
        
        printf("測試完成！按 Ctrl+C 結束程式...\n");
        while (keep_running) {
            sleep(1);
        }
    }
    
    is_connected = false;
    pthread_join(listener, NULL);
    
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
    printf("🚀 啟動 C USB Host 測試工具 (libusb)\n");
    
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    int r = libusb_init(&ctx);
    if (r < 0) {
        printf("libusb init failed: %s\n", libusb_error_name(r));
        return 1;
    }
    
    // Set debug level for libusb if needed
    // libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);

    while (keep_running) {
        int res = run_daq_loop();
        
        if (!keep_running) break;
        
        if (res == -1) {
            // Device not found
            sleep(1);
        } else if (res == -2) {
            // Disconnected or IO error
            printf("💥 [主程式] 偵測到 USB 斷線或發生錯誤\n");
            printf("⏳ 等待 5 秒後啟動自動重連機制...\n");
            sleep(5);
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

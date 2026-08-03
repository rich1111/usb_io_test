import usb.core
import usb.util
import time
import threading
import sys
import struct
import libusb_package

VID = 0x2207
PID = 0x0013

def main_daq_loop():
    print("\n🔍 正在尋找 RK3506 Native I/O 模組...")
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())

    if dev is None:
        raise ValueError("找不到設備，等待重試...")

    cfg = dev.get_active_configuration()
    if cfg is None:
        dev.set_configuration()
        cfg = dev.get_active_configuration()

    intf = usb.util.find_descriptor(cfg, bInterfaceClass=255, bInterfaceSubClass=0, bInterfaceProtocol=0)
    if intf is None:
        raise ValueError("找不到自訂的 I/O Interface。")
    
    if sys.platform == 'linux':
        try:
            if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                dev.detach_kernel_driver(intf.bInterfaceNumber)
                print(f"已解除 Linux 預設驅動對介面 {intf.bInterfaceNumber} 的佔用")
        except NotImplementedError:
            pass
        except usb.core.USBError as e:
            print(f"解除核心驅動失敗: {e}")
    elif sys.platform == 'darwin':
        # =========================================================
        # 🌟 關鍵修復 1：明確向 macOS 宣告接管此介面 (Claim Interface)
        # =========================================================
        try:
            usb.util.claim_interface(dev, intf.bInterfaceNumber)
        except usb.core.USBError as e:
            print(f"⚠️ 無法強制佔用介面: {e}，這可能導致後續權限不足。")

    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT and \
            usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)

    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN and \
            usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)

    ep_intr = usb.util.find_descriptor(
        intf, custom_match=lambda e: \
            usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN and \
            usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_INTR)
    
    if None in (ep_out, ep_in, ep_intr):
        raise ValueError("找不到正確的端點！")

    # 狀態變數 (用來通知監聽執行緒該結束了)
    is_connected = True

    # 內部監聽函數 (綁定當前的 ep_intr)
    def listen_for_interrupts():
        print("[監聽者] DI 狀態中斷監聽已啟動...")
        while is_connected:
            try:
                data = ep_intr.read(64, timeout=50) 
                if len(data) >= 3 and data[0] == 0xBB:
                    print(f"  ⚡ [中斷推播] DI {data[1]} -> {data[2]}")
            except usb.core.USBError as e:
                if e.errno == 60 or e.errno == 110 or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                    time.sleep(0.01)
                    continue
                print(f"[監聽者] 設備斷線或發生嚴重錯誤: {e}")
                break

    listener_thread = threading.Thread(target=listen_for_interrupts, daemon=True)
    listener_thread.start()
    time.sleep(0.5)

    try:
        # =========================================================
        # 壓力測試迴圈
        # =========================================================
        print("🚀 開始執行通訊任務...")
        
        ITERATIONS = 500   # 執行 500 次「全 8 通道」循環
        NUM_CHANNELS = 8   # 通道數量 (0 ~ 7)

        success_count = 0
        error_count = 0
        match_count = 0    # DO 與 DI 狀態吻合的次數
        mismatch_count = 0 # DO 與 DI 狀態不吻合的次數
        total_bytes_transferred = 0

        print(f"\n[🚀 8 通道迴圈驗證測試] 準備執行 {ITERATIONS} 次全通道循環...")
        print("⚠️ 請確保硬體已將 DO0~7 與 DI0~7 互相連接 (Loopback)")
        print("測試進行中，請稍候...\n")

        start_time = time.perf_counter()

        # 在進入迴圈前，新增一個連續錯誤計數器
        consecutive_timeouts = 0

        for i in range(ITERATIONS):
            if i > 0 and i % 50 == 0:
                print(f"目前進度: {i} / {ITERATIONS} 全通道循環...")

            # 依序測試 通道 0 到 通道 7
            for ch in range(NUM_CHANNELS):
                try:
                    # 1. 寫入 DO 狀態 
                    # 讓每個通道的 0/1 狀態交替錯開，確保不是讀到殘留電位
                    target_state = (i + ch) % 2 
                    packet_do_write = bytes([0xAA, 0x02, ch, target_state])
                    ep_out.write(packet_do_write)
                    resp_do = ep_in.read(64, timeout=50)
                    total_bytes_transferred += (len(packet_do_write) + len(resp_do))

                    # 🌟 關鍵解法：加入 2 毫秒的硬體穩定時間 (Settling Time)
                    # 讓 DO 的電壓有足夠時間爬升，並穿越 DI 的邏輯判斷閾值
                    time.sleep(0.01) 

                    # 2. 讀取 DI 狀態
                    packet_di_read = bytes([0xAA, 0x01, ch, 0x00])
                    ep_out.write(packet_di_read)
                    resp_di = ep_in.read(64, timeout=50)
                    total_bytes_transferred += (len(packet_di_read) + len(resp_di))

                    # 3. 數據比對 (Data Verification)
                    if len(resp_do) >= 4 and len(resp_di) >= 4:
                        success_count += 1
                        
                        # 解析回傳的 DI 狀態 (封包第 4 個 byte)
                        read_state = resp_di[3] 
                        
                        if read_state == target_state:
                            match_count += 1
                        else:
                            mismatch_count += 1
                            # 只有在極少數錯誤發生時才印出，避免洗頻
                            if mismatch_count <= 10:
                                print(f"[警告] 通道 {ch} 狀態不吻合！寫入: {target_state}, 讀取: {read_state}")
                    
                    # 給硬體 1 毫秒的喘息時間
                    time.sleep(0.001)

                    # 如果成功執行到這裡，代表通訊正常，將計數器歸零
                    consecutive_timeouts = 0

                except usb.core.USBError as e:
                    if e.errno in (60, 110) or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                        error_count += 1
                        print(f"[USB 錯誤] 通道 {ch} 傳輸超時: {e}")
                        
                        # 🌟 軟體看門狗：累加連續超時次數
                        consecutive_timeouts += 1
                        
                        # 如果連續 8 個通道都無回應，判定 Bulk 通道已死鎖！
                        if consecutive_timeouts >= 8:
                            print("\n💥 偵測到 Bulk 通道連續超時失去回應！強制啟動重連機制...")
                            # 強制拋出異常，打破迴圈，交給外層的 except 與 finally 處理重連
                            raise usb.core.USBError("軟體層級 Bulk 通道死鎖")
                    else:
                        # 斷線或其他嚴重錯誤，往上拋以觸發重連
                        raise e

        end_time = time.perf_counter()

        # =========================================================
        # 數據統計與分析
        # =========================================================
        total_time = end_time - start_time
        # 每次主迴圈有 8 個通道，每個通道有 2 次請求 (DO寫 + DI讀)
        total_transactions = success_count * 2 
        tps = total_transactions / total_time if total_time > 0 else 0

        avg_loop_latency_ms = (total_time / (ITERATIONS * NUM_CHANNELS)) * 1000

        print("-" * 55)
        print("📊 全通道 DO/DI 迴圈比對測試報告 (8-Ch Loopback Report)")
        print("-" * 55)
        print(f"總耗時          : {total_time:.4f} 秒")
        print(f"測試總通道次數  : {success_count} 次 (每個通道驗證算 1 次)")
        print(f"失敗/超時 (USB) : {error_count} 次")
        print("-" * 55)
        print(f"✅ 狀態完全吻合  : {match_count} 次")
        if mismatch_count == 0:
            print(f"❌ 狀態不吻合    : {mismatch_count} 次 (完美！)")
        else:
            print(f"❌ 狀態不吻合    : {mismatch_count} 次 (請檢查硬體接線或延遲)")
        print("-" * 55)
        print(f"⏱️ 單通道驗證延遲 (寫+讀)    : {avg_loop_latency_ms:.4f} 毫秒/通道")
        print(f"🚀 每秒實際 I/O 吞吐量 (TPS) : {tps:.2f} 次傳輸/秒")
        print("-" * 55)

        print("主程式繼續執行...")
        while True:
            time.sleep(1) # 模擬主程式運作

    except usb.core.USBError as e:
        print(f"\n💥 [主程式] 偵測到 USB 斷線 ({e})，準備啟動自動重連機制...")
    finally:
        is_connected = False # 通知背景監聽執行緒安全退出
        
        # =========================================================
        # 🌟 關鍵修復 2：歸還 macOS 權限並徹底清理資源
        # =========================================================
        print("🧹 正在歸還 macOS 設備控制權並清理緩衝區...")
        try:
            # 必須先 Release 再 Dispose，macOS 才會真正放開設備
            usb.util.release_interface(dev, intf.bInterfaceNumber)
        except:
            pass
            
        try:
            usb.util.dispose_resources(dev) 
        except:
            pass
            
        # 延長等待時間，確保 macOS 的網路管理員與 IOKit 完成重新列舉
        print("⏳ 等待 macOS 重新載入 USB 設備與網路卡權限 (約 5 秒)...")
        time.sleep(5)

# =========================================================
# 🌟 頂層守護迴圈 (Top-level Guardian Loop)
# =========================================================
if __name__ == '__main__':
    while True:
        try:
            main_daq_loop()
        except ValueError as ve:
            # 找不到設備時，每秒重試一次
            time.sleep(1)
        except Exception as ex:
            print(f"發生未預期錯誤: {ex}，5 秒後重試...")
            time.sleep(5)
        except KeyboardInterrupt:
            print("\n使用者手動結束程式。")
            sys.exit(0)
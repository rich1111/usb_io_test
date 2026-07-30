import usb.core
import usb.util
import time
import libusb_package
import threading

# RK3506 預設的 VID 與 PID
VID = 0x2207
PID = 0x0013

print("正在尋找 RK3506 Native I/O 模組...")
dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())

if dev is None:
    raise ValueError("找不到設備！請確認 USB 已連接。")

# ❌ 刪除或註解掉這行會引發 Resource busy 的程式碼
# dev.set_configuration()

# ✅ 改為：先檢查是否已經有作用中的組態
cfg = dev.get_active_configuration()
if cfg is None:
    # 只有在設備完全沒被初始化的情況下才設定
    dev.set_configuration()
    cfg = dev.get_active_configuration()

# 精準尋找 Native I/O 介面
intf = usb.util.find_descriptor(
    cfg, 
    bInterfaceClass=255, 
    bInterfaceSubClass=0, 
    bInterfaceProtocol=0
)

if intf is None:
    raise ValueError("找不到自訂的 I/O Interface。")

# =========================================================
# 🌟 修改這裡：將跨平台判斷改為精準鎖定 'linux'
# 因為 macOS ('darwin') 不支援也不需要呼叫 detach_kernel_driver
# =========================================================
import sys
if sys.platform == 'linux':
    try:
        if dev.is_kernel_driver_active(intf.bInterfaceNumber):
            dev.detach_kernel_driver(intf.bInterfaceNumber)
            print(f"已解除 Linux 預設驅動對介面 {intf.bInterfaceNumber} 的佔用")
    except NotImplementedError:
        pass # 防呆：如果某些閹割版 Linux 沒實作，直接忽略
    except usb.core.USBError as e:
        print(f"解除核心驅動失敗: {e}")
elif sys.platform == 'darwin':
    print("偵測到 macOS 系統，自動跳過驅動解除步驟。")
    
# =========================================================
# 🌟 修正 1：嚴格過濾 BULK 端點，避免抓錯
# =========================================================
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

# 建立一個背景監聽函數
def listen_for_interrupts():
    print("[監聽者] DI 狀態中斷監聽執行緒已啟動...")
    while True:
        try:
            # 🌟 關鍵修正：絕對不能用 timeout=0！
            # 改用 timeout=50，讓 libusb 有機會把底層鎖 (Lock) 讓給主執行緒
            data = ep_intr.read(64, timeout=50) 
            
            # 解析我們自訂的 0xBB 通知封包
            if len(data) >= 3 and data[0] == 0xBB:
                channel = data[1]
                state = data[2]
                # 為了避免洗頻干擾效能測試，我們可以稍微縮減輸出的視覺佔用
                print(f"  ⚡ [中斷推播] DI {channel} -> {state}")
                
        except usb.core.USBError as e:
            # 如果是正常的 Timeout，直接忽略並繼續下一次監聽
            if e.errno == 60 or e.errno == 110 or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                
                # 🌟 終極防護：讓硬體稍微喘息，避免瘋狂輪詢塞爆 DWC2 的 NAK 處理器
                time.sleep(0.01) 
                
                continue
                
            # 如果是設備拔除等嚴重錯誤，才印出並退出
            print(f"中斷監聽停止: {e}")
            break

# 將監聽器放進背景 Thread 執行
listener_thread = threading.Thread(target=listen_for_interrupts, daemon=True)
listener_thread.start()
time.sleep(0.5) # 給執行緒一點時間啟動

# ==================================
# 下面您可以繼續寫您的主程式 (GUI 或其他控制迴圈)
# 主程式可以繼續用 ep_out 和 ep_in 控制 Bulk 傳輸
# ==================================
# =========================================================
# 壓力測試參數設定
# =========================================================
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
            time.sleep(0.005) 

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

        except usb.core.USBError as e:
            error_count += 1
            print(f"[USB 錯誤] 通道 {ch} 傳輸失敗: {e}")

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
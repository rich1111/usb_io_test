import usb.core
import usb.util
import time
import sys
import libusb_package

# RK3506 預設的 VID 與 PID
VID = 0x2207
PID = 0x0013

print("正在尋找 RK3506 Native I/O 模組...")
dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())

if dev is None:
    raise ValueError("找不到設備！請確認 USB 已連接。")

dev.set_configuration()
cfg = dev.get_active_configuration()

# 精準尋找我們的 Native I/O 介面
intf = usb.util.find_descriptor(
    cfg, 
    bInterfaceClass=255, 
    bInterfaceSubClass=0, 
    bInterfaceProtocol=0
)

if intf is None:
    raise ValueError("找不到自訂的 I/O Interface。")

# 自動抓取端點
ep_out = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

# =========================================================
# 壓力測試參數設定
# =========================================================
ITERATIONS = 10000  # 測試次數：發送一萬次請求
packet_ai = bytes([0xAA, 0x03, 0, 0x00]) # 測試封包：讀取 AI 通道 0

success_count = 0
error_count = 0
total_bytes_transferred = 0

print(f"\n[🚀 壓力測試開始] 準備發送 {ITERATIONS} 次 I/O 請求...")
print("測試進行中，請稍候...\n")

# 使用 perf_counter 取得最高精度的時間
start_time = time.perf_counter()

for i in range(ITERATIONS):
    try:
        # 1. 寫入 4 bytes 指令
        ep_out.write(packet_ai)
        
        # 2. 讀取 11 bytes 回傳值
        response = ep_in.read(64, timeout=100) # Timeout 縮短為 100ms
        
        if len(response) > 0:
            success_count += 1
            total_bytes_transferred += (len(packet_ai) + len(response))
            
    except usb.core.USBError as e:
        error_count += 1

end_time = time.perf_counter()

# =========================================================
# 數據統計與分析
# =========================================================
total_time = end_time - start_time
tps = success_count / total_time if total_time > 0 else 0
avg_latency_ms = (total_time / ITERATIONS) * 1000

print("-" * 40)
print("📊 壓力測試報告 (Stress Test Report)")
print("-" * 40)
print(f"總耗時      : {total_time:.4f} 秒")
print(f"成功次數    : {success_count} 次")
print(f"失敗/超時   : {error_count} 次")
print(f"總傳輸量    : {total_bytes_transferred} Bytes")
print("-" * 40)
print(f"⚡ 平均延遲 (Latency) : {avg_latency_ms:.4f} 毫秒 (ms) / 次")
print(f"🚀 每秒請求數 (TPS)   : {tps:.2f} 次/秒")
print("-" * 40)
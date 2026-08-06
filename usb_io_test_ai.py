import usb.core
import usb.util
import time
import sys
import struct
import libusb_package

# RK3506 預設的 VID 與 PID
VID = 0x2207
PID = 0x0013

print("正在尋找 RK3506 Native I/O 模組...")
dev = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())

if dev is None:
    raise ValueError("找不到設備！請確認 USB 已連接。")

# ✅ 避免 Resource busy 錯誤：先檢查是否已經有作用中的組態
cfg = dev.get_active_configuration()
if cfg is None:
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

# 跨平台判斷：在 Linux 下精準解除核心驅動對該介面的佔用
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
    print("偵測到 macOS 系統，自動跳過驅動解除步驟。")

# 自動抓取端點
ep_out = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
ep_in = usb.util.find_descriptor(
    intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)

# =========================================================
# 壓力與功能測試參數設定
# =========================================================
ITERATIONS = 500    # 執行 500 次全通道循環
NUM_CHANNELS = 8    # AI 通道數量 (0 ~ 7)

# =========================================================
# 初始化 AI 通道模式 (0x00: Voltage)
# =========================================================
print("正在設定 AI 通道模式為 Voltage (0x00)...")
mode_setup_supported = True
for ch in range(NUM_CHANNELS):
    if not mode_setup_supported:
        break
    try:
        packet_ai_mode = bytes([0xAA, 0x08, ch, 0x00])
        ep_out.write(packet_ai_mode)
        response = ep_in.read(64, timeout=200)
        if len(response) < 4 or response[0] != 0xAA or response[1] != 0x08:
            print(f"⚠️ 警告: 通道 {ch} 模式設定可能未成功")
    except usb.core.USBError as e:
        print(f"⚠️ 警告: 通道 {ch} 模式設定超時/失敗 ({e})。")
        print("   -> 可能是 RK3506 設備上的韌體尚未更新支援 0x08 指令。將跳過模式設定以避免中斷通訊。")
        mode_setup_supported = False
        try:
            ep_in.clear_halt()
            ep_out.clear_halt()
        except:
            pass
        time.sleep(0.5)

success_count = 0
error_count = 0
total_bytes_transferred = 0

# 各通道最新數據紀錄
latest_ai_data = {ch: {"val": 0.0, "payload": []} for ch in range(NUM_CHANNELS)}

print(f"\n[🚀 AI (Analog Input) 8 通道讀取測試] 準備執行 {ITERATIONS} 次全通道循環...")
print("測試進行中，請稍候...\n")

start_time = time.perf_counter()

for i in range(ITERATIONS):
    if i > 0 and i % 50 == 0:
        print(f"目前進度: {i} / {ITERATIONS} 全通道循環...")

    for ch in range(NUM_CHANNELS):
        try:
            # 讀取 AI 通道 (指令 0xAA, 0x03, ch, 0x00)
            packet_ai_read = bytes([0xAA, 0x03, ch, 0x00])
            ep_out.write(packet_ai_read)
            
            response = ep_in.read(64, timeout=100)
            
            if len(response) >= 11 and response[0] == 0xAA and response[1] == 0x03:
                success_count += 1
                total_bytes_transferred += (len(packet_ai_read) + len(response))
                
                # 解碼 8 Byte 雙精度浮點數 (<d: Little-Endian float64)
                ai_bytes = bytearray(response[3:11])
                ai_value = struct.unpack('<d', ai_bytes)[0]
                
                latest_ai_data[ch] = {
                    "val": ai_value,
                    "payload": list(response[3:11])
                }
            else:
                error_count += 1

            time.sleep(0.001)

        except usb.core.USBError as e:
            error_count += 1
            print(f"[USB 錯誤] AI 通道 {ch} 傳輸失敗: {e}")

end_time = time.perf_counter()

# =========================================================
# 數據統計與分析
# =========================================================
total_time = end_time - start_time
total_requests = ITERATIONS * NUM_CHANNELS
tps = success_count / total_time if total_time > 0 else 0
avg_latency_ms = (total_time / total_requests) * 1000 if total_requests > 0 else 0

print("-" * 55)
print("📊 AI 通道讀取與壓力測試報告 (AI Channels Test Report)")
print("-" * 55)
print(f"總耗時          : {total_time:.4f} 秒")
print(f"測試總通道請求  : {total_requests} 次 ({ITERATIONS} 輪 x {NUM_CHANNELS} 通道)")
print(f"成功次數        : {success_count} 次")
print(f"失敗/超時 (USB) : {error_count} 次")
print(f"總傳輸資料量    : {total_bytes_transferred} Bytes")
print("-" * 55)
print("📈 最新各 AI 通道採樣電壓/電流值:")
for ch in range(NUM_CHANNELS):
    data = latest_ai_data[ch]
    payload_str = " ".join([f"{b:02X}" for b in data["payload"]])
    print(f"  AI{ch} 通道 -> 數值: {data['val']:10.4f} | 封包[3:11]: [{payload_str}]")
print("-" * 55)
print(f"⏱️ 單通道採樣延遲            : {avg_latency_ms:.4f} 毫秒/通道")
print(f"🚀 每秒實際 I/O 吞吐量 (TPS) : {tps:.2f} 次/秒")
print("-" * 55)
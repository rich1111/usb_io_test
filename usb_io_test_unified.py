import usb.core
import usb.util
import time
import threading
import sys
import struct
import libusb_package

VID = 0x2207
PID = 0x0013
USB_HEADER_BYTE = 0xAA
USB_CMD_GET_BOARD_TYPE = 0x0B

BOARD_TYPES = {
    0: "DI8DO8",
    1: "DI16",
    2: "DO16",
    3: "DI8RELAY4",
    4: "DI8DO6PWM2",
    5: "AIAO",
    6: "AI8",
    7: "AO8"
}

def ai_test_loop(ep_out, ep_in):
    NUM_CHANNELS = 8
    ITERATIONS = 500

    print("\n=======================================================")
    print("🚀 進入 AI/AO 測試模式")
    print("=======================================================")
    print("正在設定 AI 通道模式為 Voltage (0x00)...")
    
    mode_setup_supported = True
    for ch in range(NUM_CHANNELS):
        if not mode_setup_supported:
            break
        try:
            packet_ai_mode = bytes([0xAA, 0x08, ch, 0x00])
            ep_out.write(packet_ai_mode)
            response = ep_in.read(64, timeout=500)
            if len(response) < 4 or response[0] != 0xAA or response[1] != 0x08:
                print(f"⚠️ 警告: 通道 {ch} 模式設定可能未成功")
        except usb.core.USBError as e:
            print(f"⚠️ 警告: 通道 {ch} 模式設定超時/失敗 ({e})。可能是舊韌體，跳過模式設定。")
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
    latest_ai_data = {ch: {"val": 0.0, "payload": []} for ch in range(NUM_CHANNELS)}

    start_time = time.perf_counter()

    try:
        for i in range(ITERATIONS):
            if i > 0 and i % 50 == 0:
                print(f"目前進度: {i} / {ITERATIONS} 全通道循環...")

            for ch in range(NUM_CHANNELS):
                try:
                    packet_ai_read = bytes([0xAA, 0x03, ch, 0x00])
                    ep_out.write(packet_ai_read)
                    response = ep_in.read(64, timeout=500)
                    
                    if len(response) >= 11 and response[0] == 0xAA and response[1] == 0x03:
                        success_count += 1
                        total_bytes_transferred += (len(packet_ai_read) + len(response))
                        ai_bytes = bytearray(response[3:11])
                        ai_value = struct.unpack('<d', ai_bytes)[0]
                        latest_ai_data[ch] = {"val": ai_value, "payload": list(response[3:11])}
                    else:
                        error_count += 1
                    time.sleep(0.001)

                except usb.core.USBError as e:
                    error_count += 1
                    print(f"[USB 錯誤] AI 通道 {ch} 傳輸失敗: {e}")

    except usb.core.USBError as e:
        print(f"\n💥 [主程式] 偵測到 USB 斷線 ({e})")
        raise e
        
    end_time = time.perf_counter()

    total_time = end_time - start_time
    total_requests = ITERATIONS * NUM_CHANNELS
    tps = success_count / total_time if total_time > 0 else 0
    avg_latency_ms = (total_time / total_requests) * 1000 if total_requests > 0 else 0

    print("-" * 55)
    print("📊 AI 通道讀取與壓力測試報告")
    print("-" * 55)
    print(f"總耗時          : {total_time:.4f} 秒")
    print(f"測試總通道請求  : {total_requests} 次")
    print(f"成功次數        : {success_count} 次")
    print(f"失敗/超時 (USB) : {error_count} 次")
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


def dodi_test_loop(ep_out, ep_in, ep_intr):
    NUM_CHANNELS = 8
    ITERATIONS = 500

    print("\n=======================================================")
    print("🚀 進入 DO/DI 測試模式")
    print("=======================================================")

    is_connected = True
    def listen_for_interrupts():
        nonlocal is_connected
        print("[監聽者] DI 狀態中斷監聽已啟動...")
        while is_connected:
            try:
                data = ep_intr.read(64, timeout=50) 
                if len(data) >= 3 and data[0] == 0xBB:
                    print(f"  ⚡ [中斷推播] DI {data[1]} -> {data[2]}")
            except usb.core.USBError as e:
                if not is_connected:
                    break
                if e.errno in (60, 110) or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                    time.sleep(0.01)
                    continue
                print(f"[監聽者] 設備斷線或發生嚴重錯誤: {e}")
                break

    listener_thread = threading.Thread(target=listen_for_interrupts, daemon=True)
    listener_thread.start()
    time.sleep(0.5)

    try:
        success_count = 0
        error_count = 0
        match_count = 0
        mismatch_count = 0
        consecutive_timeouts = 0
        total_bytes_transferred = 0

        print(f"\n[🚀 8 通道迴圈驗證測試] 準備執行 {ITERATIONS} 次全通道循環...")
        print("⚠️ 請確保硬體已將 DO0~7 與 DI0~7 互相連接 (Loopback)")
        print("測試進行中，請稍候...\n")

        start_time = time.perf_counter()

        for i in range(ITERATIONS):
            if i > 0 and i % 50 == 0:
                print(f"目前進度: {i} / {ITERATIONS} 全通道循環...")

            for ch in range(NUM_CHANNELS):
                try:
                    target_state = (i + ch) % 2 
                    packet_do_write = bytes([0xAA, 0x02, ch, target_state])
                    ep_out.write(packet_do_write)
                    resp_do = ep_in.read(64, timeout=500)
                    total_bytes_transferred += (len(packet_do_write) + len(resp_do))

                    time.sleep(0.01) 

                    packet_di_read = bytes([0xAA, 0x01, ch, 0x00])
                    ep_out.write(packet_di_read)
                    resp_di = ep_in.read(64, timeout=500)
                    total_bytes_transferred += (len(packet_di_read) + len(resp_di))

                    if len(resp_do) >= 4 and len(resp_di) >= 4:
                        success_count += 1
                        read_state = resp_di[3] 
                        if read_state == target_state:
                            match_count += 1
                        else:
                            mismatch_count += 1
                            if mismatch_count <= 10:
                                print(f"[警告] 通道 {ch} 狀態不吻合！寫入: {target_state}, 讀取: {read_state}")
                    
                    time.sleep(0.001)
                    consecutive_timeouts = 0

                except usb.core.USBError as e:
                    if e.errno in (60, 110) or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                        error_count += 1
                        print(f"[USB 錯誤] 通道 {ch} 傳輸超時: {e}")
                        consecutive_timeouts += 1
                        if consecutive_timeouts >= 8:
                            print("\n💥 偵測到 Bulk 通道連續超時失去回應！強制啟動重連機制...")
                            raise usb.core.USBError("軟體層級 Bulk 通道死鎖")
                    else:
                        raise e

        end_time = time.perf_counter()

        total_time = end_time - start_time
        total_transactions = success_count * 2 
        tps = total_transactions / total_time if total_time > 0 else 0
        avg_loop_latency_ms = (total_time / (ITERATIONS * NUM_CHANNELS)) * 1000

        print("-" * 55)
        print("📊 全通道 DO/DI 迴圈比對測試報告")
        print("-" * 55)
        print(f"總耗時          : {total_time:.4f} 秒")
        print(f"測試總通道次數  : {success_count} 次")
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
        time.sleep(1)

    except usb.core.USBError as e:
        print(f"\n💥 [主程式] 偵測到 USB 斷線 ({e})")
        raise e
    finally:
        is_connected = False


def main():
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
        try:
            usb.util.claim_interface(dev, intf.bInterfaceNumber)
        except usb.core.USBError as e:
            print(f"⚠️ 無法強制佔用介面: {e}")

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

    if None in (ep_out, ep_in):
        raise ValueError("找不到正確的端點！")

    try:
        print("🔍 正在向設備發送 UsbCmdGetBoardType (0x0B)...")
        cmd = bytearray([USB_HEADER_BYTE, USB_CMD_GET_BOARD_TYPE, 0x00, 0x00])
        board_id = 255
        found_board = False
        
        for attempt in range(1, 6):
            try:
                ep_out.write(cmd, timeout=500)
                resp = ep_in.read(64, timeout=500)
                
                if len(resp) >= 4 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_GET_BOARD_TYPE:
                    board_id = resp[3]
                    found_board = True
                    break
                else:
                    time.sleep(0.01)
            except usb.core.USBError:
                time.sleep(0.01)
                
        if found_board:
            board_name = BOARD_TYPES.get(board_id, f"未知型號 ({board_id})")
            print(f"✅ 成功獲取！RK3506 設備回報之 I/O Board Type 為: {board_name} (ID: {board_id})")
        else:
            print("⚠️ 設備回應格式異常，已達最大重試次數")
            raise ValueError("無法讀取 Board Type")

        if board_id in [5, 6, 7]:
            # AI / AO Boards
            ai_test_loop(ep_out, ep_in)
        else:
            # DO / DI Boards (0, 1, 2, 3, 4)
            if ep_intr is None:
                print("⚠️ 警告: 找不到 Interrupt IN 端點，中斷監聽可能無法運作。")
            dodi_test_loop(ep_out, ep_in, ep_intr)

    except usb.core.USBError as e:
        print(f"\n💥 執行期間 USB 錯誤: {e}")
        raise e
    finally:
        print("🧹 正在歸還設備控制權並清理緩衝區...")
        try:
            usb.util.release_interface(dev, intf.bInterfaceNumber)
        except:
            pass
        try:
            usb.util.dispose_resources(dev) 
        except:
            pass
        time.sleep(2)

if __name__ == '__main__':
    while True:
        try:
            main()
            print("\n✅ 測試圓滿結束！程式安全退出。")
            break
        except ValueError as ve:
            time.sleep(1)
        except Exception as ex:
            print(f"發生未預期錯誤: {ex}，5 秒後重試...")
            time.sleep(5)
        except KeyboardInterrupt:
            print("\n使用者手動結束程式。")
            sys.exit(0)

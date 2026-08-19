import usb.core
import usb.util
import time
import threading
import sys
import struct
import json
import libusb_package
import paho.mqtt.client as mqtt

VID = 0x2207
PID = 0x0013
USB_HEADER_BYTE = 0xAA
USB_CMD_READ_DI = 0x01
USB_CMD_WRITE_DO = 0x02
USB_CMD_READ_AI = 0x03
USB_CMD_READ_DO = 0x05
USB_CMD_GET_BOARD_TYPE = 0x0B

BOARD_TYPES = {
    0: "DI8DO8", 1: "DI16", 2: "DO16", 3: "DI8RELAY4", 4: "DI8DO6PWM2",
    5: "AIAO", 6: "AI8", 7: "AO8"
}

# Global state
mqtt_client = None
usb_ep_out = None
usb_ep_in = None
usb_ep_intr = None
board_type_id = -1
is_connected = False
config = {}
usb_lock = threading.Lock()

def load_config():
    global config
    try:
        with open("mqtt_config.json", "r") as f:
            config = json.load(f)
        print("✅ 成功載入 MQTT 設定檔 (mqtt_config.json)")
    except Exception as e:
        print(f"❌ 讀取 mqtt_config.json 失敗: {e}")
        sys.exit(1)

def on_mqtt_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"✅ 成功連接到 MQTT Broker ({config['broker']}:{config['port']})")
        client.subscribe(config['topic_query'])
        client.subscribe(config['topic_write'])
        print(f"📡 已訂閱主題: {config['topic_query']} 與 {config['topic_write']}")
    else:
        print(f"❌ 連接 MQTT Broker 失敗，返回碼: {reason_code}")

def on_mqtt_message(client, userdata, msg):
    global is_connected
    if not is_connected or usb_ep_out is None or usb_ep_in is None:
        return
        
    topic = msg.topic
    payload = msg.payload.decode('utf-8')
    try:
        data = json.loads(payload)
    except:
        print(f"⚠️ 收到無效的 JSON 格式資料: {payload}")
        return

    if topic == config['topic_query']:
        handle_query(data)
    elif topic == config['topic_write']:
        handle_write(data)

def handle_query(data):
    global is_connected
    ch = data.get("channel", 0)
    
    with usb_lock:
        try:
            if board_type_id in [5, 6, 7]:
                # AI / AO
                packet = bytes([USB_HEADER_BYTE, USB_CMD_READ_AI, ch, 0x00])
                usb_ep_out.write(packet)
                resp = usb_ep_in.read(64, timeout=500)
                if len(resp) >= 11 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_READ_AI:
                    ai_bytes = bytearray(resp[3:11])
                    val = struct.unpack('<d', ai_bytes)[0]
                    res_payload = json.dumps({"channel": ch, "type": "AI", "value": val})
                    mqtt_client.publish(config['topic_status'], res_payload)
            else:
                target = data.get("target", "DI")
                if target == "DO":
                    packet = bytes([USB_HEADER_BYTE, USB_CMD_READ_DO, ch, 0x00])
                    usb_ep_out.write(packet)
                    resp = usb_ep_in.read(64, timeout=500)
                    if len(resp) >= 4 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_READ_DO:
                        val = resp[3]
                        res_payload = json.dumps({"channel": ch, "type": "DO", "state": val})
                        mqtt_client.publish(config['topic_status'], res_payload)
                else:
                    # DI / DO
                    packet = bytes([USB_HEADER_BYTE, USB_CMD_READ_DI, ch, 0x00])
                    usb_ep_out.write(packet)
                    resp = usb_ep_in.read(64, timeout=500)
                    if len(resp) >= 4 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_READ_DI:
                        val = resp[3]
                        res_payload = json.dumps({"channel": ch, "type": "DI", "state": val})
                        mqtt_client.publish(config['topic_status'], res_payload)
        except usb.core.USBError as e:
            print(f"⚠️ Query USB 通訊錯誤: {e}")
            if e.errno == 19 or 'No such device' in str(e):
                print("💔 設備已斷線，觸發自動重連...")
                is_connected = False

def handle_write(data):
    global is_connected
    ch = data.get("channel", 0)
    state = data.get("state", 0)
    
    if board_type_id not in [5, 6, 7]:
        with usb_lock:
            try:
                packet = bytes([USB_HEADER_BYTE, USB_CMD_WRITE_DO, ch, state])
                usb_ep_out.write(packet)
                # Read ACK
                resp = usb_ep_in.read(64, timeout=500)
                if len(resp) >= 4 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_WRITE_DO:
                    print(f"✅ 成功寫入 DO {ch} -> {state}")
            except usb.core.USBError as e:
                print(f"⚠️ Write USB 通訊錯誤: {e}")
                if e.errno == 19 or 'No such device' in str(e):
                    print("💔 設備已斷線，觸發自動重連...")
                    is_connected = False

def usb_interrupt_listener():
    global is_connected
    print("[監聽者] DI 狀態中斷監聽已啟動...")
    while is_connected:
        if usb_ep_intr is None:
            time.sleep(1)
            continue
        try:
            data = usb_ep_intr.read(64, timeout=50)
            if len(data) >= 3 and data[0] == 0xBB:
                ch = data[1]
                state = data[2]
                print(f"  ⚡ [中斷推播] DI {ch} -> {state}")
                payload = json.dumps({"channel": ch, "type": "DI", "state": state})
                mqtt_client.publish(config['topic_interrupt'], payload)
        except usb.core.USBError as e:
            if e.errno in (60, 110) or 'timed out' in str(e).lower() or 'timeout' in str(e).lower():
                time.sleep(0.01)
                continue
            if is_connected:
                print(f"[監聽者] 設備斷線或發生嚴重錯誤: {e}")
                is_connected = False
            break

def main():
    global mqtt_client, usb_ep_out, usb_ep_in, usb_ep_intr, board_type_id, is_connected
    
    load_config()
    
    # 建立 MQTT Client
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.on_connect = on_mqtt_connect
    mqtt_client.on_message = on_mqtt_message
    
    try:
        mqtt_client.connect(config['broker'], config['port'], 60)
        mqtt_client.loop_start()
    except Exception as e:
        print(f"❌ 無法連接 MQTT Broker: {e}")
        sys.exit(1)

    while True:
        try:
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
                except:
                    pass
            elif sys.platform == 'darwin':
                try:
                    usb.util.claim_interface(dev, intf.bInterfaceNumber)
                except:
                    pass

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

            usb_ep_out = ep_out
            usb_ep_in = ep_in
            usb_ep_intr = ep_intr
            
            # 取得板卡型號
            cmd = bytearray([USB_HEADER_BYTE, USB_CMD_GET_BOARD_TYPE, 0x00, 0x00])
            found_board = False
            for attempt in range(1, 6):
                try:
                    usb_ep_out.write(cmd, timeout=500)
                    resp = usb_ep_in.read(64, timeout=500)
                    if len(resp) >= 4 and resp[0] == USB_HEADER_BYTE and resp[1] == USB_CMD_GET_BOARD_TYPE:
                        board_type_id = resp[3]
                        found_board = True
                        break
                except:
                    time.sleep(0.01)
                    
            if found_board:
                board_name = BOARD_TYPES.get(board_type_id, f"未知型號 ({board_type_id})")
                print(f"✅ 成功獲取！RK3506 設備型號為: {board_name}")
            else:
                raise ValueError("無法讀取 Board Type")

            is_connected = True
            
            # 如果是 DO/DI 板卡，啟動中斷監聽
            if board_type_id not in [5, 6, 7] and usb_ep_intr:
                listener_thread = threading.Thread(target=usb_interrupt_listener, daemon=True)
                listener_thread.start()

            print("🚀 MQTT Bridge 運行中 (按 Ctrl+C 終止)...")
            while is_connected:
                time.sleep(1)

        except ValueError as ve:
            time.sleep(1)
        except usb.core.USBError as e:
            print(f"\n💥 執行期間 USB 錯誤或設備斷線: {e}")
            is_connected = False
            usb_ep_out = None
            usb_ep_in = None
            usb_ep_intr = None
            time.sleep(3)
        except KeyboardInterrupt:
            print("\n使用者手動結束程式。")
            break
        except Exception as ex:
            print(f"發生未預期錯誤: {ex}，3 秒後重試...")
            is_connected = False
            time.sleep(3)
        finally:
            is_connected = False
            if 'dev' in locals() and dev is not None and 'intf' in locals() and intf is not None:
                try:
                    usb.util.release_interface(dev, intf.bInterfaceNumber)
                    usb.util.dispose_resources(dev)
                except:
                    pass

    mqtt_client.loop_stop()
    mqtt_client.disconnect()

if __name__ == '__main__':
    main()

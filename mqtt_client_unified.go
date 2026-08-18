package main

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"math"
	"os"
	"os/signal"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	"github.com/google/gousb"
)

const (
	VID                = 0x2207
	PID                = 0x0013
	UsbHeaderByte      = 0xAA
	UsbCmdReadDI       = 0x01
	UsbCmdWriteDO      = 0x02
	UsbCmdReadAI       = 0x03
	UsbCmdReadDO       = 0x05
	UsbCmdGetBoardType = 0x0B
)

var boardTypes = map[byte]string{
	0: "DI8DO8", 1: "DI16", 2: "DO16", 3: "DI8RELAY4", 4: "DI8DO6PWM2",
	5: "AIAO", 6: "AI8", 7: "AO8",
}

type Config struct {
	Broker         string `json:"broker"`
	Port           int    `json:"port"`
	TopicQuery     string `json:"topic_query"`
	TopicStatus    string `json:"topic_status"`
	TopicInterrupt string `json:"topic_interrupt"`
	TopicWrite     string `json:"topic_write"`
}

type MsgPayload struct {
	Channel int     `json:"channel"`
	Target  string  `json:"target,omitempty"`
	Type    string  `json:"type,omitempty"`
	State   int     `json:"state"`
	Value   float64 `json:"value"`
}

var (
	config      Config
	mqttClient  mqtt.Client
	epOut       *gousb.OutEndpoint
	epIn        *gousb.InEndpoint
	epIntr      *gousb.InEndpoint
	boardTypeID byte
	isConnected bool
	usbMutex    sync.Mutex
)

func loadConfig() {
	file, err := os.Open("mqtt_config.json")
	if err != nil {
		log.Fatalf("❌ 讀取 mqtt_config.json 失敗: %v", err)
	}
	defer file.Close()
	decoder := json.NewDecoder(file)
	err = decoder.Decode(&config)
	if err != nil {
		log.Fatalf("❌ 解析 mqtt_config.json 失敗: %v", err)
	}
	fmt.Println("✅ 成功載入 MQTT 設定檔")
}

func setupMQTT() {
	opts := mqtt.NewClientOptions()
	opts.AddBroker(fmt.Sprintf("tcp://%s:%d", config.Broker, config.Port))
	opts.SetClientID("rk3506_go_bridge")
	
	opts.SetOnConnectHandler(func(c mqtt.Client) {
		fmt.Printf("✅ 成功連接到 MQTT Broker (%s:%d)\n", config.Broker, config.Port)
		c.Subscribe(config.TopicQuery, 0, handleQuery)
		c.Subscribe(config.TopicWrite, 0, handleWrite)
		fmt.Printf("📡 已訂閱主題: %s 與 %s\n", config.TopicQuery, config.TopicWrite)
	})
	
	opts.SetConnectionLostHandler(func(c mqtt.Client, err error) {
		fmt.Printf("❌ MQTT 連接斷開: %v\n", err)
	})

	mqttClient = mqtt.NewClient(opts)
	if token := mqttClient.Connect(); token.Wait() && token.Error() != nil {
		log.Fatalf("❌ 無法連接 MQTT Broker: %v", token.Error())
	}
}

func handleQuery(client mqtt.Client, msg mqtt.Message) {
	if !isConnected || epOut == nil || epIn == nil {
		return
	}
	var data MsgPayload
	err := json.Unmarshal(msg.Payload(), &data)
	if err != nil {
		fmt.Printf("⚠️ 收到無效的 JSON 格式資料: %s\n", string(msg.Payload()))
		return
	}
	
	ch := byte(data.Channel)
	
	usbMutex.Lock()
	defer usbMutex.Unlock()

	if boardTypeID >= 5 && boardTypeID <= 7 {
		packet := []byte{UsbHeaderByte, UsbCmdReadAI, ch, 0x00}
		ctxW, cancelW := context.WithTimeout(context.Background(), 500*time.Millisecond)
		_, err := epOut.WriteContext(ctxW, packet)
		cancelW()
		if err != nil {
			return
		}
		
		resp := make([]byte, 64)
		ctxR, cancelR := context.WithTimeout(context.Background(), 500*time.Millisecond)
		n, _ := epIn.ReadContext(ctxR, resp)
		cancelR()
		
		if n >= 11 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadAI {
			bits := binary.LittleEndian.Uint64(resp[3:11])
			val := math.Float64frombits(bits)
			resPayload, _ := json.Marshal(MsgPayload{Channel: int(ch), Type: "AI", Value: val})
			client.Publish(config.TopicStatus, 0, false, string(resPayload))
		}
	} else {
		if data.Target == "DO" {
			packet := []byte{UsbHeaderByte, UsbCmdReadDO, ch, 0x00}
			ctxW, cancelW := context.WithTimeout(context.Background(), 500*time.Millisecond)
			_, err := epOut.WriteContext(ctxW, packet)
			cancelW()
			if err != nil {
				return
			}
			
			resp := make([]byte, 64)
			ctxR, cancelR := context.WithTimeout(context.Background(), 500*time.Millisecond)
			n, _ := epIn.ReadContext(ctxR, resp)
			cancelR()
			
			if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadDO {
				state := int(resp[3])
				resPayload, _ := json.Marshal(MsgPayload{Channel: int(ch), Type: "DO", State: state})
				client.Publish(config.TopicStatus, 0, false, string(resPayload))
			}
		} else {
			packet := []byte{UsbHeaderByte, UsbCmdReadDI, ch, 0x00}
			ctxW, cancelW := context.WithTimeout(context.Background(), 500*time.Millisecond)
			_, err := epOut.WriteContext(ctxW, packet)
			cancelW()
			if err != nil {
				return
			}
			
			resp := make([]byte, 64)
			ctxR, cancelR := context.WithTimeout(context.Background(), 500*time.Millisecond)
			n, _ := epIn.ReadContext(ctxR, resp)
			cancelR()
			
			if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadDI {
				state := int(resp[3])
				resPayload, _ := json.Marshal(MsgPayload{Channel: int(ch), Type: "DI", State: state})
				client.Publish(config.TopicStatus, 0, false, string(resPayload))
			}
		}
	}
}

func handleWrite(client mqtt.Client, msg mqtt.Message) {
	if !isConnected || epOut == nil || epIn == nil {
		return
	}
	if boardTypeID >= 5 && boardTypeID <= 7 {
		return // Ignore write for AI boards in this simple implementation
	}
	
	var data MsgPayload
	err := json.Unmarshal(msg.Payload(), &data)
	if err != nil {
		return
	}
	
	ch := byte(data.Channel)
	state := byte(data.State)
	
	usbMutex.Lock()
	defer usbMutex.Unlock()
	
	packet := []byte{UsbHeaderByte, UsbCmdWriteDO, ch, state}
	ctxW, cancelW := context.WithTimeout(context.Background(), 500*time.Millisecond)
	_, err = epOut.WriteContext(ctxW, packet)
	cancelW()
	if err != nil {
		return
	}
	
	resp := make([]byte, 64)
	ctxR, cancelR := context.WithTimeout(context.Background(), 500*time.Millisecond)
	n, _ := epIn.ReadContext(ctxR, resp)
	cancelR()
	
	if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdWriteDO {
		fmt.Printf("✅ 成功寫入 DO %d -> %d\n", ch, state)
	}
}

func interruptListener() {
	fmt.Println("[監聽者] DI 狀態中斷監聽已啟動...")
	for isConnected && epIntr != nil {
		buf := make([]byte, 64)
		ctxIntr, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
		n, err := epIntr.ReadContext(ctxIntr, buf)
		cancel()
		
		if err != nil {
			if err == context.DeadlineExceeded || err.Error() == "timeout" || strings.Contains(err.Error(), "transfer was cancelled") {
				time.Sleep(10 * time.Millisecond)
				continue
			}
			if isConnected {
				fmt.Printf("[監聽者] 設備斷線或嚴重錯誤: %v\n", err)
				isConnected = false
			}
			break
		}
		
		if n >= 3 && buf[0] == 0xBB {
			ch := buf[1]
			state := buf[2]
			fmt.Printf("  ⚡ [中斷推播] DI %d -> %d\n", ch, state)
			payload, _ := json.Marshal(MsgPayload{Channel: int(ch), Type: "DI", State: int(state)})
			mqttClient.Publish(config.TopicInterrupt, 0, false, string(payload))
		}
	}
}

func main() {
	loadConfig()
	setupMQTT()
	defer mqttClient.Disconnect(250)
	
	ctx := gousb.NewContext()
	defer ctx.Close()
	
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigChan
		fmt.Println("\n🧹 收到中斷訊號，正在清理...")
		isConnected = false
		mqttClient.Disconnect(250)
		os.Exit(0)
	}()

	for {
		fmt.Println("\n🔍 正在尋找 RK3506 Native I/O 模組...")
		dev, err := ctx.OpenDeviceWithVIDPID(gousb.ID(VID), gousb.ID(PID))
		if err != nil || dev == nil {
			time.Sleep(1 * time.Second)
			continue
		}
		
		if runtime.GOOS == "linux" {
			dev.SetAutoDetach(true)
		}
		
		cfg, err := dev.Config(1)
		if err != nil {
			dev.Close()
			time.Sleep(1 * time.Second)
			continue
		}
		
		var targetIntfNum = -1
		for intfNum, intfDesc := range cfg.Desc.Interfaces {
			for _, alt := range intfDesc.AltSettings {
				if alt.Class == gousb.ClassVendorSpec && alt.SubClass == 0 && alt.Protocol == 0 {
					targetIntfNum = intfNum
					break
				}
			}
			if targetIntfNum != -1 { break }
		}
		
		if targetIntfNum == -1 {
			cfg.Close()
			dev.Close()
			time.Sleep(1 * time.Second)
			continue
		}
		
		intf, err := cfg.Interface(targetIntfNum, 0)
		if err != nil {
			cfg.Close()
			dev.Close()
			time.Sleep(1 * time.Second)
			continue
		}
		
		epOut = nil
		epIn = nil
		epIntr = nil
		
		for _, epDesc := range intf.Setting.Endpoints {
			if epDesc.Direction == gousb.EndpointDirectionOut && epDesc.TransferType == gousb.TransferTypeBulk {
				epOut, _ = intf.OutEndpoint(epDesc.Number)
			} else if epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeBulk {
				epIn, _ = intf.InEndpoint(epDesc.Number)
			} else if epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeInterrupt {
				epIntr, _ = intf.InEndpoint(epDesc.Number)
			}
		}
		
		if epOut == nil || epIn == nil {
			intf.Close()
			cfg.Close()
			dev.Close()
			time.Sleep(1 * time.Second)
			continue
		}
		
		// Query board type
		cmd := []byte{UsbHeaderByte, UsbCmdGetBoardType, 0x00, 0x00}
		foundBoard := false
		for attempt := 1; attempt <= 5; attempt++ {
			ctxOut, cancelOut := context.WithTimeout(context.Background(), 500*time.Millisecond)
			_, err = epOut.WriteContext(ctxOut, cmd)
			cancelOut()
			if err != nil { continue }
			
			resp := make([]byte, 64)
			ctxIn, cancelIn := context.WithTimeout(context.Background(), 500*time.Millisecond)
			n, rErr := epIn.ReadContext(ctxIn, resp)
			cancelIn()
			if rErr != nil { continue }
			
			if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdGetBoardType {
				boardTypeID = resp[3]
				foundBoard = true
				break
			}
		}
		
		if foundBoard {
			name, ok := boardTypes[boardTypeID]
			if !ok { name = "Unknown" + strconv.Itoa(int(boardTypeID)) }
			fmt.Printf("✅ 成功獲取！RK3506 設備型號為: %s\n", name)
		} else {
			intf.Close()
			cfg.Close()
			dev.Close()
			time.Sleep(1 * time.Second)
			continue
		}
		
		isConnected = true
		
		if (boardTypeID < 5 || boardTypeID > 7) && epIntr != nil {
			go interruptListener()
		}
		
		fmt.Println("🚀 MQTT Bridge 運行中 (按 Ctrl+C 終止)...")
		for isConnected {
			time.Sleep(1 * time.Second)
		}
		
		intf.Close()
		cfg.Close()
		dev.Close()
		time.Sleep(3 * time.Second)
	}
}

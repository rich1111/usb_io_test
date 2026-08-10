package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"log"
	"math"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/google/gousb"
)

const (
	VID                = 0x2207
	PID                = 0x0013
	UsbHeaderByte      = 0xAA
	UsbCmdReadDI       = 0x01
	UsbCmdWriteDO      = 0x02
	UsbCmdReadAI       = 0x03
	UsbCmdSetAIMode    = 0x08
	UsbCmdGetBoardType = 0x0B
	NumChannels        = 8
	Iterations         = 500
)

var boardTypes = map[byte]string{
	0: "DI8DO8",
	1: "DI16",
	2: "DO16",
	3: "DI8RELAY4",
	4: "DI8DO6PWM2",
	5: "AIAO",
	6: "AI8",
	7: "AO8",
}

func main() {
	fmt.Println("🚀 啟動 Golang USB Unified 測試工具")

	ctx := gousb.NewContext()
	defer ctx.Close()

	for {
		err := runUnifiedLoop(ctx)
		if err != nil {
			log.Printf("💥 [主程式] 偵測到 USB 斷線或發生錯誤: %v\n", err)
			log.Println("⏳ 等待 5 秒後啟動自動重連機制...")
			time.Sleep(5 * time.Second)
		}
	}
}

func runUnifiedLoop(ctx *gousb.Context) error {
	fmt.Println("\n🔍 正在尋找 RK3506 Native I/O 模組...")
	dev, err := ctx.OpenDeviceWithVIDPID(gousb.ID(VID), gousb.ID(PID))
	if err != nil {
		return fmt.Errorf("無法開啟設備: %v", err)
	}
	if dev == nil {
		time.Sleep(1 * time.Second)
		return nil
	}
	defer dev.Close()

	if runtime.GOOS == "linux" {
		if err := dev.SetAutoDetach(true); err != nil {
			log.Printf("⚠️ 警告: 無法設定自動釋放驅動: %v\n", err)
		}
	}

	cfg, err := dev.Config(1)
	if err != nil {
		return fmt.Errorf("獲取 Configuration 失敗: %v", err)
	}
	defer cfg.Close()

	var targetIntfNum = -1
	for intfNum, intfDesc := range cfg.Desc.Interfaces {
		for _, alt := range intfDesc.AltSettings {
			if alt.Class == gousb.ClassVendorSpec && alt.SubClass == 0 && alt.Protocol == 0 {
				if targetIntfNum == -1 || intfNum < targetIntfNum {
					targetIntfNum = intfNum
				}
				break
			}
		}
	}

	if targetIntfNum == -1 {
		return fmt.Errorf("找不到 Native I/O 介面")
	}

	intf, err := cfg.Interface(targetIntfNum, 0)
	if err != nil {
		return fmt.Errorf("佔用 Interface 失敗: %v", err)
	}
	defer intf.Close()

	var epIn, epIntr *gousb.InEndpoint
	var epOutBulk *gousb.OutEndpoint

	for _, epDesc := range intf.Setting.Endpoints {
		if epDesc.Direction == gousb.EndpointDirectionOut && epDesc.TransferType == gousb.TransferTypeBulk {
			epOutBulk, _ = intf.OutEndpoint(epDesc.Number)
		} else if epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeBulk {
			epIn, _ = intf.InEndpoint(epDesc.Number)
		} else if epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeInterrupt {
			epIntr, _ = intf.InEndpoint(epDesc.Number)
		}
	}

	if epOutBulk == nil || epIn == nil {
		return fmt.Errorf("找不到必要的 Bulk 端點")
	}

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigChan
		fmt.Println("\n🧹 收到中斷訊號，正在清理...")
		intf.Close()
		cfg.Close()
		dev.Close()
		os.Exit(0)
	}()

	fmt.Println("🔍 正在向設備發送 UsbCmdGetBoardType (0x0B)...")
	cmd := []byte{UsbHeaderByte, UsbCmdGetBoardType, 0x00, 0x00}
	
	ctxOut, cancelOut := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancelOut()
	_, err = epOutBulk.WriteContext(ctxOut, cmd)
	if err != nil {
		return fmt.Errorf("寫入指令失敗: %v", err)
	}

	resp := make([]byte, 64)
	ctxIn, cancelIn := context.WithTimeout(context.Background(), 500*time.Millisecond)
	defer cancelIn()
	n, err := epIn.ReadContext(ctxIn, resp)
	if err != nil {
		return fmt.Errorf("讀取回應失敗: %v", err)
	}

	boardID := byte(255)
	if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdGetBoardType {
		boardID = resp[3]
		name, ok := boardTypes[boardID]
		if !ok {
			name = fmt.Sprintf("未知型號 (%d)", boardID)
		}
		fmt.Printf("✅ 成功獲取！RK3506 設備回報之 I/O Board Type 為: %s (ID: %d)\n", name, boardID)
	} else {
		return fmt.Errorf("無法獲取板卡型號")
	}

	if boardID >= 5 && boardID <= 7 {
		return runAILoop(epOutBulk, epIn)
	} else {
		if epIntr == nil {
			fmt.Println("⚠️ 找不到 Interrupt IN 端點，中斷監聽可能無效。")
		}
		return runDODILoop(epOutBulk, epIn, epIntr)
	}
}

func runAILoop(epOut *gousb.OutEndpoint, epIn *gousb.InEndpoint) error {
	fmt.Println("\n=======================================================")
	fmt.Println("🚀 進入 AI/AO 測試模式")
	fmt.Println("=======================================================")

	fmt.Println("正在設定 AI 通道模式為 Voltage (0x00)...")
	modeSetupSupported := true
	for ch := 0; ch < NumChannels; ch++ {
		if !modeSetupSupported {
			break
		}
		cmdMode := []byte{UsbHeaderByte, UsbCmdSetAIMode, byte(ch), 0x00}
		ctxMode, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
		_, err := epOut.WriteContext(ctxMode, cmdMode)
		cancel()

		if err != nil {
			fmt.Printf("⚠️ 警告: 通道 %d 模式設定寫入超時/失敗 (%v)。可能是舊韌體，跳過。\n", ch, err)
			modeSetupSupported = false
			time.Sleep(500 * time.Millisecond)
			break
		}

		resp := make([]byte, 64)
		ctxResp, cancelResp := context.WithTimeout(context.Background(), 200*time.Millisecond)
		n, _ := epIn.ReadContext(ctxResp, resp)
		cancelResp()
		if n < 4 || resp[0] != UsbHeaderByte || resp[1] != UsbCmdSetAIMode {
			fmt.Printf("⚠️ 警告: 通道 %d 模式設定回應異常\n", ch)
		}
	}

	successCount := 0
	errorCount := 0
	var totalBytesTransferred int64 = 0

	type AIData struct {
		Val     float64
		Payload []byte
	}
	latestAIData := make([]AIData, NumChannels)

	fmt.Printf("\n[🚀 AI 8 通道讀取測試] 準備執行 %d 次全通道循環...\n", Iterations)
	startTime := time.Now()

	for i := 0; i < Iterations; i++ {
		if i > 0 && i%50 == 0 {
			fmt.Printf("目前進度: %d / %d 全通道循環...\n", i, Iterations)
		}

		for ch := 0; ch < NumChannels; ch++ {
			packetRead := []byte{UsbHeaderByte, UsbCmdReadAI, byte(ch), 0x00}
			ctxW, cancelW := context.WithTimeout(context.Background(), 100*time.Millisecond)
			wN, wErr := epOut.WriteContext(ctxW, packetRead)
			cancelW()
			if wErr != nil {
				errorCount++
				fmt.Printf("[USB 錯誤] AI 通道 %d 寫入失敗: %v\n", ch, wErr)
				continue
			}
			totalBytesTransferred += int64(wN)

			resp := make([]byte, 64)
			ctxR, cancelR := context.WithTimeout(context.Background(), 100*time.Millisecond)
			rN, rErr := epIn.ReadContext(ctxR, resp)
			cancelR()
			if rErr != nil {
				errorCount++
				fmt.Printf("[USB 錯誤] AI 通道 %d 讀取超時: %v\n", ch, rErr)
				continue
			}

			if rN >= 11 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadAI {
				successCount++
				totalBytesTransferred += int64(rN)
				bits := binary.LittleEndian.Uint64(resp[3:11])
				aiValue := math.Float64frombits(bits)
				payload := make([]byte, 8)
				copy(payload, resp[3:11])
				latestAIData[ch] = AIData{Val: aiValue, Payload: payload}
			} else {
				errorCount++
			}
			time.Sleep(1 * time.Millisecond)
		}
	}

	totalTime := time.Since(startTime).Seconds()
	totalReqs := Iterations * NumChannels
	var tps float64
	if totalTime > 0 {
		tps = float64(successCount) / totalTime
	}

	fmt.Println("-------------------------------------------------------")
	fmt.Println("📊 AI 通道讀取與壓力測試報告 (AI Channels Test Report)")
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("總耗時          : %.4f 秒\n", totalTime)
	fmt.Printf("測試總通道請求  : %d 次\n", totalReqs)
	fmt.Printf("成功次數        : %d 次\n", successCount)
	fmt.Printf("失敗/超時 (USB) : %d 次\n", errorCount)
	fmt.Println("-------------------------------------------------------")
	for ch := 0; ch < NumChannels; ch++ {
		data := latestAIData[ch]
		payloadStr := ""
		for _, b := range data.Payload {
			payloadStr += fmt.Sprintf("%02X ", b)
		}
		fmt.Printf("  AI%d 通道 -> 數值: %10.4f | 封包[3:11]: [%s]\n", ch, data.Val, payloadStr)
	}
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("🚀 每秒實際 I/O 吞吐量 (TPS) : %.2f 次/秒\n", tps)
	return nil
}

func runDODILoop(epOut *gousb.OutEndpoint, epIn *gousb.InEndpoint, epIntr *gousb.InEndpoint) error {
	fmt.Println("\n=======================================================")
	fmt.Println("🚀 進入 DO/DI 測試模式")
	fmt.Println("=======================================================")

	isConnected := true
	if epIntr != nil {
		go func() {
			fmt.Println("[監聽者] DI 狀態中斷監聽已啟動...")
			for isConnected {
				buf := make([]byte, 64)
				ctxIntr, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
				n, err := epIntr.ReadContext(ctxIntr, buf)
				cancel()
				if err != nil {
					if err == context.DeadlineExceeded || err.Error() == "libusb: timeout [code -7]" {
						time.Sleep(10 * time.Millisecond)
						continue
					}
					if isConnected {
						fmt.Printf("[監聽者] 設備斷線或嚴重錯誤: %v\n", err)
					}
					break
				}
				if n >= 3 && buf[0] == 0xBB {
					fmt.Printf("  ⚡ [中斷推播] DI %d -> %d\n", buf[1], buf[2])
				}
			}
		}()
		time.Sleep(500 * time.Millisecond)
	}

	successCount := 0
	errorCount := 0
	matchCount := 0
	mismatchCount := 0
	consecutiveTimeouts := 0

	fmt.Printf("\n[🚀 8 通道迴圈驗證測試] 準備執行 %d 次全通道循環...\n", Iterations)
	startTime := time.Now()

	for i := 0; i < Iterations; i++ {
		if i > 0 && i%50 == 0 {
			fmt.Printf("目前進度: %d / %d 全通道循環...\n", i, Iterations)
		}

		for ch := 0; ch < NumChannels; ch++ {
			targetState := byte((i + ch) % 2)
			packetDOWrite := []byte{UsbHeaderByte, UsbCmdWriteDO, byte(ch), targetState}

			ctxW, cancelW := context.WithTimeout(context.Background(), 50*time.Millisecond)
			_, wErr := epOut.WriteContext(ctxW, packetDOWrite)
			cancelW()
			if wErr != nil {
				goto handleError
			}

			{
				resp := make([]byte, 64)
				ctxR, cancelR := context.WithTimeout(context.Background(), 50*time.Millisecond)
				_, rErr := epIn.ReadContext(ctxR, resp)
				cancelR()
				if rErr != nil {
					goto handleError
				}
			}

			time.Sleep(10 * time.Millisecond)

			{
				packetDIRead := []byte{UsbHeaderByte, UsbCmdReadDI, byte(ch), 0x00}
				ctxW2, cancelW2 := context.WithTimeout(context.Background(), 50*time.Millisecond)
				_, wErr2 := epOut.WriteContext(ctxW2, packetDIRead)
				cancelW2()
				if wErr2 != nil {
					goto handleError
				}

				respDI := make([]byte, 64)
				ctxR2, cancelR2 := context.WithTimeout(context.Background(), 50*time.Millisecond)
				nDI, rErr2 := epIn.ReadContext(ctxR2, respDI)
				cancelR2()
				if rErr2 != nil {
					goto handleError
				}

				if nDI >= 4 {
					successCount++
					readState := respDI[3]
					if readState == targetState {
						matchCount++
					} else {
						mismatchCount++
						if mismatchCount <= 10 {
							fmt.Printf("[警告] 通道 %d 狀態不吻合！寫入: %d, 讀取: %d\n", ch, targetState, readState)
						}
					}
				}
			}

			time.Sleep(1 * time.Millisecond)
			consecutiveTimeouts = 0
			continue

		handleError:
			errorCount++
			consecutiveTimeouts++
			if consecutiveTimeouts >= 8 {
				isConnected = false
				return fmt.Errorf("軟體層級 Bulk 通道死鎖")
			}
		}
	}

	isConnected = false
	totalTime := time.Since(startTime).Seconds()
	tps := float64(successCount*2) / totalTime

	fmt.Println("-------------------------------------------------------")
	fmt.Println("📊 全通道 DO/DI 迴圈比對測試報告")
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("總耗時          : %.4f 秒\n", totalTime)
	fmt.Printf("測試總通道次數  : %d 次\n", successCount)
	fmt.Printf("失敗/超時 (USB) : %d 次\n", errorCount)
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("✅ 狀態完全吻合  : %d 次\n", matchCount)
	if mismatchCount == 0 {
		fmt.Printf("❌ 狀態不吻合    : %d 次 (完美！)\n", mismatchCount)
	} else {
		fmt.Printf("❌ 狀態不吻合    : %d 次 (請檢查硬體接線)\n", mismatchCount)
	}
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("🚀 每秒實際 TPS  : %.2f 次傳輸/秒\n", tps)
	fmt.Println("-------------------------------------------------------")

	return nil
}

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
	"strings"
	"syscall"
	"time"

	"github.com/google/gousb"
)

const (
	VID             = 0x2207
	PID             = 0x0013
	UsbHeaderByte   = 0xAA
	UsbCmdReadAI    = 0x03
	UsbCmdSetAIMode = 0x08
	NumChannels     = 8
	Iterations      = 500
)

type AIData struct {
	Val     float64
	Payload []byte
}

func main() {
	fmt.Println("🚀 啟動 Golang USB Host AI 測試工具")

	ctx := gousb.NewContext()
	defer ctx.Close()

	for {
		err := runAILoop(ctx)
		if err != nil {
			log.Printf("💥 [主程式] 偵測到 USB 斷線或發生錯誤: %v\n", err)
			log.Println("⏳ 等待 5 秒後啟動自動重連機制...")
			time.Sleep(5 * time.Second)
		} else {
			break
		}
	}
}

func runAILoop(ctx *gousb.Context) error {
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

	// ==========================================
	// 🌟 啟動自動釋放驅動！
	// ==========================================
	switch runtime.GOOS {
	case "linux":
		if err := dev.SetAutoDetach(true); err != nil {
			log.Printf("⚠️ 警告: 無法設定自動釋放驅動: %v\n", err)
		}
	case "darwin":
		fmt.Println("偵測到 macOS 系統，自動跳過驅動解除步驟。")
	}

	activeCfg, err := dev.ActiveConfigNum()
	log.Printf("Current Active Config: %d, err: %v", activeCfg, err)

	cfg, err := dev.Config(1)
	if err != nil {
		return fmt.Errorf("獲取 Configuration 失敗: %v", err)
	}
	defer cfg.Close()

	// 動態尋找 Vendor Specific 介面 (Class=255, SubClass=0, Protocol=0)
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

	fmt.Printf("✅ 成功找到 Native I/O 介面，編號: Interface %d\n", targetIntfNum)

	intf, err := cfg.Interface(targetIntfNum, 0)
	if err != nil {
		return fmt.Errorf("佔用 Interface %d 失敗: %v", targetIntfNum, err)
	}
	defer intf.Close()

	var epOut *gousb.OutEndpoint
	var epIn *gousb.InEndpoint

	for _, epDesc := range intf.Setting.Endpoints {
		if epOut == nil && epDesc.Direction == gousb.EndpointDirectionOut && epDesc.TransferType == gousb.TransferTypeBulk {
			epOut, _ = intf.OutEndpoint(epDesc.Number)
		} else if epIn == nil && epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeBulk {
			epIn, _ = intf.InEndpoint(epDesc.Number)
		}
	}

	if epOut == nil || epIn == nil {
		return fmt.Errorf("找不到必要的 Bulk 端點！(epOut, epIn)")
	}

	// ==========================================
	// 🌟 攔截 Ctrl+C 以確保資源被正確釋放 (避免 macOS 鎖死 USB)
	// ==========================================
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigChan
		fmt.Println("\n🧹 收到中斷訊號，正在歸還設備控制權並清理...")
		intf.Close()
		cfg.Close()
		dev.Close()
		os.Exit(0)
	}()

	// =========================================================
	// 初始化 AI 通道模式 (0x00: Voltage)
	// =========================================================
	fmt.Println("正在設定 AI 通道模式為 Voltage (0x00)...")
	modeSetupSupported := true
	for ch := 0; ch < NumChannels; ch++ {
		if !modeSetupSupported {
			break
		}
		cmdMode := []byte{UsbHeaderByte, UsbCmdSetAIMode, byte(ch), 0x00}
		ctxMode, cancelMode := context.WithTimeout(context.Background(), 200*time.Millisecond)
		_, err := epOut.WriteContext(ctxMode, cmdMode)
		cancelMode()
		if err != nil {
			fmt.Printf("⚠️ 警告: 通道 %d 模式設定寫入失敗: %v\n", ch, err)
			continue
		}

		resp := make([]byte, 64)
		ctxResp, cancelResp := context.WithTimeout(context.Background(), 200*time.Millisecond)
		n, err := epIn.ReadContext(ctxResp, resp)
		cancelResp()
		if err != nil {
			fmt.Printf("⚠️ 警告: 通道 %d 模式設定超時/失敗 (%v)。\n", ch, err)
			fmt.Println("   -> 可能是 RK3506 設備上的韌體尚未更新支援 0x08 指令。將跳過模式設定以避免中斷通訊。")
			modeSetupSupported = false
			time.Sleep(500 * time.Millisecond)
			continue
		}
		if n < 4 || resp[0] != UsbHeaderByte || resp[1] != UsbCmdSetAIMode {
			fmt.Printf("⚠️ 警告: 通道 %d 模式設定可能未成功\n", ch)
		}
	}

	fmt.Printf("\n[🚀 AI (Analog Input) 8 通道讀取測試] 準備執行 %d 次全通道循環...\n", Iterations)
	fmt.Println("測試進行中，請稍候...\n")

	startTime := time.Now()
	var successCount, errorCount int
	var totalBytesTransferred int
	consecutiveTimeouts := 0

	latestAIData := make(map[int]AIData)
	for i := 0; i < NumChannels; i++ {
		latestAIData[i] = AIData{Val: 0.0, Payload: make([]byte, 8)}
	}

	for i := 0; i < Iterations; i++ {
		if i > 0 && i%50 == 0 {
			fmt.Printf("目前進度: %d / %d 全通道循環...\n", i, Iterations)
		}

		for ch := 0; ch < NumChannels; ch++ {
			cmdAI := []byte{UsbHeaderByte, UsbCmdReadAI, byte(ch), 0x00}
			ctxAI, cancelAI := context.WithTimeout(context.Background(), 100*time.Millisecond)
			
			if _, err := epOut.WriteContext(ctxAI, cmdAI); err != nil {
				errorCount++
				consecutiveTimeouts++
				cancelAI()
				if consecutiveTimeouts >= 8 {
					return fmt.Errorf("Bulk 寫入連續失敗: %v", err)
				}
				continue
			}
			cancelAI()

			resp := make([]byte, 64)
			ctxResp, cancelResp := context.WithTimeout(context.Background(), 100*time.Millisecond)
			n, err := epIn.ReadContext(ctxResp, resp)
			cancelResp()

			if err != nil {
				if err == context.DeadlineExceeded || strings.Contains(err.Error(), "timed out") {
					errorCount++
					consecutiveTimeouts++
					if consecutiveTimeouts >= 8 {
						return fmt.Errorf("Bulk 讀取連續失敗 (Timeout)")
					}
					continue
				}
				errorCount++
				log.Printf("[USB 錯誤] AI 通道 %d 傳輸失敗: %v\n", ch, err)
				continue
			}

			consecutiveTimeouts = 0

			if n >= 11 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadAI {
				successCount++
				totalBytesTransferred += len(cmdAI) + n
				
				// 解碼 8 Byte 雙精度浮點數 (<d: Little-Endian float64)
				aiBytes := resp[3:11]
				bits := binary.LittleEndian.Uint64(aiBytes)
				aiValue := math.Float64frombits(bits)

				latestAIData[ch] = AIData{
					Val:     aiValue,
					Payload: append([]byte(nil), aiBytes...),
				}
			} else {
				errorCount++
			}
			
			time.Sleep(1 * time.Millisecond)
		}
	}

	elapsed := time.Since(startTime)
	totalRequests := Iterations * NumChannels
	tps := float64(successCount) / elapsed.Seconds()
	avgLatencyMs := 0.0
	if totalRequests > 0 {
		avgLatencyMs = (elapsed.Seconds() / float64(totalRequests)) * 1000
	}

	fmt.Println("-------------------------------------------------------")
	fmt.Println("📊 AI 通道讀取與壓力測試報告 (AI Channels Test Report)")
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("總耗時          : %.4f 秒\n", elapsed.Seconds())
	fmt.Printf("測試總通道請求  : %d 次 (%d 輪 x %d 通道)\n", totalRequests, Iterations, NumChannels)
	fmt.Printf("成功次數        : %d 次\n", successCount)
	fmt.Printf("失敗/超時 (USB) : %d 次\n", errorCount)
	fmt.Printf("總傳輸資料量    : %d Bytes\n", totalBytesTransferred)
	fmt.Println("-------------------------------------------------------")
	fmt.Println("📈 最新各 AI 通道採樣電壓/電流值:")
	for ch := 0; ch < NumChannels; ch++ {
		data := latestAIData[ch]
		payloadStr := ""
		for _, b := range data.Payload {
			payloadStr += fmt.Sprintf("%02X ", b)
		}
		payloadStr = strings.TrimSpace(payloadStr)
		fmt.Printf("  AI%d 通道 -> 數值: %10.4f | 封包[3:11]: [%s]\n", ch, data.Val, payloadStr)
	}
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("⏱️ 單通道採樣延遲            : %.4f 毫秒/通道\n", avgLatencyMs)
	fmt.Printf("🚀 每秒實際 I/O 吞吐量 (TPS) : %.2f 次/秒\n", tps)
	fmt.Println("-------------------------------------------------------")

	return nil
}

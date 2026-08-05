package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/google/gousb"
)

const (
	VID           = 0x2207
	PID           = 0x0013
	UsbHeaderByte = 0xAA
	UsbCmdReadDI  = 0x01
	UsbCmdWriteDO = 0x02
	NumChannels   = 8
	Iterations    = 500 // 測試圈數
)

func main() {
	fmt.Println("🚀 啟動 Golang USB Host 測試工具")

	ctx := gousb.NewContext()
	defer ctx.Close()

	for {
		err := runDaqLoop(ctx)
		if err != nil {
			log.Printf("💥 [主程式] 偵測到 USB 斷線或發生錯誤: %v\n", err)
			log.Println("⏳ 等待 5 秒後啟動自動重連機制...")
			time.Sleep(5 * time.Second)
		}
	}
}

func runDaqLoop(ctx *gousb.Context) error {

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
	var epIn, epIntr *gousb.InEndpoint

	for _, epDesc := range intf.Setting.Endpoints {
		if epOut == nil && epDesc.Direction == gousb.EndpointDirectionOut && epDesc.TransferType == gousb.TransferTypeBulk {
			epOut, _ = intf.OutEndpoint(epDesc.Number)
		} else if epIn == nil && epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeBulk {
			epIn, _ = intf.InEndpoint(epDesc.Number)
		} else if epIntr == nil && epDesc.Direction == gousb.EndpointDirectionIn && epDesc.TransferType == gousb.TransferTypeInterrupt {
			epIntr, _ = intf.InEndpoint(epDesc.Number)
		}
	}

	if epOut == nil || epIn == nil || epIntr == nil {
		return fmt.Errorf("找不到必要的端點！(epOut, epIn, epIntr)")
	}

	// ==========================================
	// 🌟 攔截 Ctrl+C 以確保資源被正確釋放 (避免 macOS 鎖死 USB)
	// ==========================================
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigChan
		fmt.Println("\n🧹 收到中斷訊號，正在歸還 macOS 設備控制權並清理緩衝區...")
		intf.Close()
		cfg.Close()
		dev.Close()
		os.Exit(0)
	}()

	// ==========================================
	// 🌟 背景監聽執行緒 (Interrupt 推播)
	// ==========================================
	done := make(chan struct{})
	go func() {
		fmt.Println("[監聽者] DI 狀態中斷監聽已啟動...")
		buf := make([]byte, 64)
		for {
			select {
			case <-done:
				return
			default:
				ctxIntr, cancelIntr := context.WithTimeout(context.Background(), 50*time.Millisecond)
				n, err := epIntr.ReadContext(ctxIntr, buf)
				cancelIntr()

				if err != nil {
					// 逾時處理: 若 50ms 內沒有中斷資料，則休眠 10ms 再試
					if err == context.DeadlineExceeded || err.Error() == "usb: transfer timed out" {
						time.Sleep(10 * time.Millisecond)
						continue
					}
					log.Printf("[監聽者] 設備斷線或讀取中斷: %v\n", err)
					return
				}
				if n >= 3 && buf[0] == 0xBB {
					fmt.Printf("  ⚡ [中斷推播] DI %d -> %d\n", buf[1], buf[2])
				}
			}
		}
	}()

	fmt.Printf("\n[🚀 8 通道迴圈驗證測試] 準備執行 %d 次全通道循環...\n", Iterations)

	startTime := time.Now()
	var successCount, errorCount, mismatchCount int
	consecutiveTimeouts := 0

	for i := 0; i < Iterations; i++ {
		if i > 0 && i%50 == 0 {
			fmt.Printf("目前進度: %d / %d 全通道循環...\n", i, Iterations)
		}

		for ch := 0; ch < NumChannels; ch++ {
			targetState := byte((i + ch) % 2)

			cmdDO := []byte{UsbHeaderByte, UsbCmdWriteDO, byte(ch), targetState}
			ctxDO, cancelDO := context.WithTimeout(context.Background(), 50*time.Millisecond)
			if _, err := epOut.WriteContext(ctxDO, cmdDO); err != nil {
				errorCount++
				consecutiveTimeouts++
				if consecutiveTimeouts >= 8 {
					close(done)
					cancelDO()
					return fmt.Errorf("Bulk 寫入連續失敗")
				}
				cancelDO()
				continue
			}
			cancelDO()

			// 🌟 必須讀取 DO 的回應，否則裝置端會因為 buffer 滿而死鎖
			respDO := make([]byte, 64)
			ctxDOResp, cancelDOResp := context.WithTimeout(context.Background(), 50*time.Millisecond)
			epIn.ReadContext(ctxDOResp, respDO)
			cancelDOResp()

			time.Sleep(10 * time.Millisecond)

			cmdDI := []byte{UsbHeaderByte, UsbCmdReadDI, byte(ch), 0x00}
			ctxDI, cancelDI := context.WithTimeout(context.Background(), 50*time.Millisecond)
			if _, err := epOut.WriteContext(ctxDI, cmdDI); err != nil {
				errorCount++
				consecutiveTimeouts++
				cancelDI()
				continue
			}
			cancelDI()

			resp := make([]byte, 64)
			ctxResp, cancelResp := context.WithTimeout(context.Background(), 50*time.Millisecond)
			n, err := epIn.ReadContext(ctxResp, resp)
			cancelResp()
			if err != nil {
				errorCount++
				consecutiveTimeouts++
				if consecutiveTimeouts >= 8 {
					close(done)
					return fmt.Errorf("Bulk 讀取連續失敗")
				}
				continue
			}

			consecutiveTimeouts = 0

			if n >= 4 && resp[0] == UsbHeaderByte && resp[1] == UsbCmdReadDI && resp[2] == byte(ch) {
				if resp[3] == targetState {
					successCount++
				} else {
					mismatchCount++
				}
			} else {
				errorCount++
			}
		}
	}

	elapsed := time.Since(startTime)
	totalOps := float64(NumChannels * Iterations * 2)
	tps := totalOps / elapsed.Seconds()

	fmt.Println("\n-------------------------------------------------------")
	fmt.Println("📊 全通道迴圈測試報告 (改造版 gousb)")
	fmt.Println("-------------------------------------------------------")
	fmt.Printf("總耗時         : %.4f 秒\n", elapsed.Seconds())
	fmt.Printf("✅ 狀態完全吻合 : %d 次\n", successCount)
	fmt.Printf("❌ 狀態不吻合   : %d 次\n", mismatchCount)
	fmt.Printf("🚀 每秒 I/O 吞吐 : %.2f TPS\n", tps)
	fmt.Println("-------------------------------------------------------")

	close(done)
	fmt.Println("測試完成！按 Ctrl+C 結束程式...")
	select {}
}

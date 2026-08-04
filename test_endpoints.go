package main

import (
	"fmt"
	"log"

	"github.com/google/gousb"
)

const (
	VID = 0x2207
	PID = 0x0013
)

func main() {
	ctx := gousb.NewContext()
	defer ctx.Close()

	dev, err := ctx.OpenDeviceWithVIDPID(VID, PID)
	if err != nil || dev == nil {
		log.Fatalf("Cannot open: %v", err)
	}
	defer dev.Close()

	cfg, _ := dev.Config(1)
	defer cfg.Close()

	for intfNum, intfDesc := range cfg.Desc.Interfaces {
		for _, alt := range intfDesc.AltSettings {
			if alt.Class == gousb.ClassVendorSpec && alt.SubClass == 0 && alt.Protocol == 0 {
				fmt.Printf("Interface %d:\n", intfNum)
				for _, ep := range alt.Endpoints {
					fmt.Printf("  Endpoint %d: Dir=%s Transfer=%s\n", ep.Number, ep.Direction, ep.TransferType)
				}
			}
		}
	}
}

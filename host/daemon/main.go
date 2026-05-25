package main

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"
)

const (
	svcUUID    = "de900001-4467-42f0-a359-000000000000"
	stateUUID  = "de900002-4467-42f0-a359-000000000000"
	statsUUID  = "de900003-4467-42f0-a359-000000000000"
	actionUUID = "de900004-4467-42f0-a359-000000000000"
	nameUUID   = "de900005-4467-42f0-a359-000000000000"
	sockPath   = "/tmp/agent-viewer.sock"
)

var eventToState = map[string]byte{
	"SessionStart": 0,
	"SessionEnd":   0,
	"PreToolUse":   1,
	"Notification": 2,
	"Stop":         3,
}

var (
	adapter   = bluetooth.DefaultAdapter
	stateCh   bluetooth.DeviceCharacteristic
	statsCh   bluetooth.DeviceCharacteristic
	actionCh  bluetooth.DeviceCharacteristic
	nameCh    bluetooth.DeviceCharacteristic
	connected bool
	connMu    sync.RWMutex
)

type claudeStats struct {
	TotalInputTokens  int     `json:"totalInputTokens"`
	TotalOutputTokens int     `json:"totalOutputTokens"`
	TotalCost         float64 `json:"totalCost"`
}

func readStats() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return "home err"
	}
	path := filepath.Join(home, ".claude", "stats-cache.json")
	f, err := os.Open(path)
	if err != nil {
		return "no stats"
	}
	defer f.Close()
	var s claudeStats
	if json.NewDecoder(f).Decode(&s) != nil {
		return "parse err"
	}
	total := s.TotalInputTokens + s.TotalOutputTokens
	return fmt.Sprintf("Cost: $%.2f | T: %.1fk", s.TotalCost, float64(total)/1000.0)
}

func sendState(v byte) {
	connMu.RLock()
	defer connMu.RUnlock()
	if !connected {
		return
	}
	fmt.Printf("[ble] state %d\n", v)
	stateCh.Write([]byte{v})
}

func sendStats(text string) {
	connMu.RLock()
	defer connMu.RUnlock()
	if !connected {
		return
	}
	fmt.Printf("[ble] stats: %s\n", text)
	statsCh.Write([]byte(text))
}

func onAction(buf []byte) {
	if len(buf) > 0 && buf[0] == 1 {
		fmt.Println("[ble] touch acknowledged")
		exec.Command("notify-send", "Agent Viewer", "User acknowledged alert").Run()
	}
}

func connectLoop() {
	srvUUID, _ := bluetooth.ParseUUID(svcUUID)
	stUUID, _ := bluetooth.ParseUUID(stateUUID)
	stsUUID, _ := bluetooth.ParseUUID(statsUUID)
	actUUID, _ := bluetooth.ParseUUID(actionUUID)
	nmUUID, _ := bluetooth.ParseUUID(nameUUID)

	for {
		connMu.RLock()
		if connected {
			connMu.RUnlock()
			time.Sleep(2 * time.Second)
			continue
		}
		connMu.RUnlock()

		fmt.Println("[ble] scanning for Agent-Viewer...")
		found := make(chan bluetooth.ScanResult, 1)

		adapter.Scan(func(ad *bluetooth.Adapter, r bluetooth.ScanResult) {
			if r.LocalName() == "Agent-Viewer" {
				ad.StopScan()
				select {
				case found <- r:
				default:
				}
			}
		})

		var result bluetooth.ScanResult
		select {
		case result = <-found:
		case <-time.After(15 * time.Second):
			fmt.Println("[ble] scan timeout")
			continue
		}

		fmt.Printf("[ble] connecting to %s\n", result.Address.String())
		dev, err := adapter.Connect(result.Address, bluetooth.ConnectionParams{})
		if err != nil {
			fmt.Printf("[ble] connect failed: %v\n", err)
			time.Sleep(2 * time.Second)
			continue
		}

		svcs, err := dev.DiscoverServices([]bluetooth.UUID{srvUUID})
		if err != nil || len(svcs) == 0 {
			fmt.Printf("[ble] service discovery failed: %v\n", err)
			dev.Disconnect()
			continue
		}

		svc := svcs[0]

		chars, err := svc.DiscoverCharacteristics([]bluetooth.UUID{stUUID})
		if err != nil || len(chars) == 0 {
			fmt.Println("[ble] state char not found")
			dev.Disconnect()
			continue
		}
		sc := chars[0]

		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{stsUUID})
		if err != nil || len(chars) == 0 {
			fmt.Println("[ble] stats char not found")
			dev.Disconnect()
			continue
		}
		ssc := chars[0]

		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{actUUID})
		if err != nil || len(chars) == 0 {
			fmt.Println("[ble] action char not found")
			dev.Disconnect()
			continue
		}
		ac := chars[0]

		ac.EnableNotifications(onAction)

		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{nmUUID})
		if err == nil && len(chars) > 0 {
			nc := chars[0]
			hostname, _ := os.Hostname()
			nc.Write([]byte(hostname))
			fmt.Printf("[ble] sent hostname: %s\n", hostname)
		}

		connMu.Lock()
		stateCh = sc
		statsCh = ssc
		actionCh = ac
		connected = true
		connMu.Unlock()

		fmt.Println("[ble] connected")

		sendStats(readStats())
		sendState(0)
	}
}

func unixServer() {
	os.Remove(sockPath)
	listener, err := net.Listen("unix", sockPath)
	if err != nil {
		panic(fmt.Sprintf("unix listen failed: %v", err))
	}
	defer listener.Close()
	defer os.Remove(sockPath)
	fmt.Printf("[unix] listening on %s\n", sockPath)

	for {
		conn, err := listener.Accept()
		if err != nil {
			continue
		}
		go handleConn(conn)
	}
}

func handleConn(conn net.Conn) {
	defer conn.Close()
	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		return
	}
	event := strings.TrimSpace(string(buf[:n]))
	fmt.Printf("[unix] event: %s\n", event)
	if v, ok := eventToState[event]; ok {
		go sendState(v)
		if event == "Stop" {
			go func() {
				time.Sleep(2 * time.Second)
				sendStats(readStats())
			}()
		}
	}
}

func statsSync() {
	for range time.NewTicker(10 * time.Second).C {
		connMu.RLock()
		ok := connected
		connMu.RUnlock()
		if ok {
			sendStats(readStats())
		}
	}
}

func main() {
	fmt.Println("=== Agent Viewer Daemon ===")
	adapter.Enable()
	go unixServer()
	go statsSync()
	connectLoop()
}

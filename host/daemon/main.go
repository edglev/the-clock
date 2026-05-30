package main

import (
	"encoding/json"
	"fmt"
	"hash/fnv"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"
)

const (
	svcUUID    = "00000000-0000-a359-42f0-4467de900001"
	stateUUID  = "00000000-0000-a359-42f0-4467de900002"
	statsUUID  = "00000000-0000-a359-42f0-4467de900003"
	actionUUID = "00000000-0000-a359-42f0-4467de900004"
	nameUUID   = "00000000-0000-a359-42f0-4467de900005"
	multiUUID  = "00000000-0000-a359-42f0-4467de900006"
	sockPath   = "/tmp/agent-viewer.sock"

	maxLabelLen  = 32
	maxStatusLen = 32
)

var eventToState = map[string]byte{
	"SessionStart": 0,
	"SessionEnd":   0,
	"PreToolUse":   1,
	"Notification": 2,
	"Stop":         3,
}

var eventToStatus = map[string]string{
	"SessionStart": "Started",
	"SessionEnd":   "Ended",
	"PreToolUse":   "Thinking",
	"Notification": "Waiting",
	"Stop":         "Success",
}

var (
	adapter        = bluetooth.DefaultAdapter
	stateCh        bluetooth.DeviceCharacteristic
	statsCh        bluetooth.DeviceCharacteristic
	actionCh       bluetooth.DeviceCharacteristic
	nameCh         bluetooth.DeviceCharacteristic
	multiCh        bluetooth.DeviceCharacteristic
	connected      bool
	multiSupported bool
	connMu         sync.RWMutex

	instancesMu sync.Mutex
	instances   = map[string]*agentInstance{}
	focusedID   string
)

type hookEvent struct {
	Event       string `json:"event"`
	CWD         string `json:"cwd"`
	SessionID   string `json:"session_id,omitempty"`
	Label       string `json:"label,omitempty"`
	TimestampMS int64  `json:"timestamp_ms"`
}

type agentInstance struct {
	ID        string
	Key       string
	UserLabel string
	Label     string
	Event     string
	Status    string
	State     byte
	Updated   time.Time
	EndedAt   time.Time
}

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
	return fmt.Sprintf("Cost $%.2f T %.1fk", s.TotalCost, float64(total)/1000.0)
}

func markDisconnected(err error) {
	if err != nil {
		fmt.Printf("[ble] write failed: %v\n", err)
	}
	connMu.Lock()
	connected = false
	multiSupported = false
	connMu.Unlock()
}

func sendState(v byte) {
	connMu.RLock()
	ok := connected
	ch := stateCh
	connMu.RUnlock()
	if !ok {
		return
	}
	fmt.Printf("[ble] state %d\n", v)
	if _, err := ch.WriteWithoutResponse([]byte{v}); err != nil {
		markDisconnected(err)
	}
}

func sendStats(text string) {
	connMu.RLock()
	ok := connected
	ch := statsCh
	connMu.RUnlock()
	if !ok {
		return
	}
	text = sanitizeField(text, maxStatusLen)
	fmt.Printf("[ble] stats: %s\n", text)
	if _, err := ch.WriteWithoutResponse([]byte(text)); err != nil {
		markDisconnected(err)
	}
}

func sendMultiPayload(payload string) bool {
	connMu.RLock()
	ok := connected && multiSupported
	ch := multiCh
	connMu.RUnlock()
	if !ok {
		return false
	}
	fmt.Printf("[ble] multi: %s\n", payload)
	if _, err := ch.WriteWithoutResponse([]byte(payload)); err != nil {
		markDisconnected(err)
		return false
	}
	return true
}

func sendInstance(inst agentInstance) {
	payload := fmt.Sprintf("U\t%s\t%d\t%s\t%s",
		inst.ID,
		inst.State,
		sanitizeField(inst.Label, maxLabelLen),
		sanitizeField(inst.Status, maxStatusLen),
	)
	if !sendMultiPayload(payload) {
		sendState(inst.State)
		sendStats(inst.Status)
	}
}

func sendDelete(id string) {
	sendMultiPayload("D\t" + id)
}

func onAction(buf []byte) {
	if len(buf) == 0 {
		return
	}
	action := buf[0]
	id := ""
	if action == '1' {
		action = 1
		if len(buf) > 2 && buf[1] == '\t' {
			id = string(buf[2:])
		}
	} else if len(buf) > 1 {
		id = string(buf[1:])
	}
	if action != 1 {
		return
	}

	label := "focused agent"
	instancesMu.Lock()
	if inst, ok := instances[id]; ok {
		label = inst.Label
	}
	instancesMu.Unlock()

	fmt.Printf("[ble] touch acknowledged for %s\n", label)
	exec.Command("notify-send", "Agent Viewer", "Acknowledged "+label).Run()
}

func connectLoop() {
	srvUUID, _ := bluetooth.ParseUUID(svcUUID)
	stUUID, _ := bluetooth.ParseUUID(stateUUID)
	stsUUID, _ := bluetooth.ParseUUID(statsUUID)
	actUUID, _ := bluetooth.ParseUUID(actionUUID)
	nmUUID, _ := bluetooth.ParseUUID(nameUUID)
	maUUID, _ := bluetooth.ParseUUID(multiUUID)

	for {
		connMu.RLock()
		if connected {
			connMu.RUnlock()
			time.Sleep(2 * time.Second)
			continue
		}
		connMu.RUnlock()

		var result bluetooth.ScanResult
		if configured, ok := configuredAddress(); ok {
			result.Address = configured
			fmt.Printf("[ble] using configured Agent-Viewer address %s\n", configured.String())
		} else {
			fmt.Println("[ble] scanning for Agent-Viewer...")
			found := make(chan bluetooth.ScanResult, 1)

			if err := adapter.Scan(func(ad *bluetooth.Adapter, r bluetooth.ScanResult) {
				if r.LocalName() == "Agent-Viewer" {
					ad.StopScan()
					select {
					case found <- r:
					default:
					}
				}
			}); err != nil {
				fmt.Printf("[ble] scan failed: %v\n", err)
				adapter.StopScan()
				time.Sleep(2 * time.Second)
				continue
			}

			select {
			case result = <-found:
			case <-time.After(15 * time.Second):
				fmt.Println("[ble] scan timeout")
				adapter.StopScan()
				continue
			}
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

		var nc bluetooth.DeviceCharacteristic
		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{nmUUID})
		if err == nil && len(chars) > 0 {
			nc = chars[0]
			hostname, _ := os.Hostname()
			nc.WriteWithoutResponse([]byte(hostname))
			fmt.Printf("[ble] sent hostname: %s\n", hostname)
		}

		var mc bluetooth.DeviceCharacteristic
		multiOK := false
		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{maUUID})
		if err == nil && len(chars) > 0 {
			mc = chars[0]
			multiOK = true
			fmt.Println("[ble] multi-agent char found")
		} else {
			fmt.Println("[ble] multi-agent char not found, using legacy protocol")
		}

		connMu.Lock()
		stateCh = sc
		statsCh = ssc
		actionCh = ac
		nameCh = nc
		multiCh = mc
		connected = true
		multiSupported = multiOK
		connMu.Unlock()

		fmt.Println("[ble] connected")

		if multiOK {
			sendSnapshot()
		} else {
			sendStats(readStats())
			sendState(0)
		}
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
	data, err := io.ReadAll(io.LimitReader(conn, 4096))
	if err != nil {
		return
	}

	ev := parseEvent(data)
	if ev.Event == "" {
		return
	}
	fmt.Printf("[unix] event: %s cwd=%s\n", ev.Event, ev.CWD)

	state, ok := eventToState[ev.Event]
	if !ok {
		return
	}

	updated := time.Now()
	if ev.TimestampMS > 0 {
		updated = time.UnixMilli(ev.TimestampMS)
	}

	key := canonicalPath(ev.CWD)
	id := instanceID(key)
	status := eventToStatus[ev.Event]
	if status == "" {
		status = ev.Event
	}

	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok {
		inst = &agentInstance{ID: id, Key: key}
		instances[id] = inst
	}
	inst.UserLabel = strings.TrimSpace(ev.Label)
	inst.Event = ev.Event
	inst.State = state
	inst.Status = status
	inst.Updated = updated
	if ev.Event == "SessionEnd" {
		inst.EndedAt = updated
	} else {
		inst.EndedAt = time.Time{}
	}
	focusedID = id
	deriveLabelsLocked()
	instancesMu.Unlock()

	sendSnapshot()

	if ev.Event == "Stop" {
		go autoIdle(id, updated)
	}
}

func parseEvent(data []byte) hookEvent {
	text := strings.TrimSpace(string(data))
	if text == "" {
		return hookEvent{}
	}

	var ev hookEvent
	if strings.HasPrefix(text, "{") && json.Unmarshal(data, &ev) == nil {
		return ev
	}

	cwd, _ := os.Getwd()
	return hookEvent{
		Event:       text,
		CWD:         cwd,
		TimestampMS: time.Now().UnixMilli(),
	}
}

func autoIdle(id string, eventTime time.Time) {
	time.Sleep(2 * time.Second)
	stats := readStats()

	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok || focusedID != id || !inst.Updated.Equal(eventTime) || inst.State != eventToState["Stop"] {
		instancesMu.Unlock()
		return
	}
	inst.State = eventToState["SessionEnd"]
	inst.Status = stats
	inst.Updated = time.Now()
	deriveLabelsLocked()
	instancesMu.Unlock()

	sendSnapshot()
}

func statsSync() {
	for range time.NewTicker(10 * time.Second).C {
		connMu.RLock()
		ok := connected
		multiOK := multiSupported
		connMu.RUnlock()
		if ok && !multiOK {
			sendStats(readStats())
		}
	}
}

func pruneLoop() {
	for range time.NewTicker(1 * time.Minute).C {
		now := time.Now()
		var deleted []string

		instancesMu.Lock()
		for id, inst := range instances {
			expiredEnded := !inst.EndedAt.IsZero() && now.Sub(inst.EndedAt) > 30*time.Minute
			expiredStale := now.Sub(inst.Updated) > 2*time.Hour
			if expiredEnded || expiredStale {
				deleted = append(deleted, id)
				delete(instances, id)
			}
		}
		if len(deleted) > 0 && instances[focusedID] == nil {
			focusedID = newestInstanceIDLocked()
		}
		deriveLabelsLocked()
		instancesMu.Unlock()

		for _, id := range deleted {
			sendDelete(id)
		}
		if len(deleted) > 0 {
			sendSnapshot()
		}
	}
}

func sendSnapshot() {
	items := snapshotInstances()
	if len(items) == 0 {
		return
	}
	for _, inst := range items {
		sendInstance(inst)
	}
}

func snapshotInstances() []agentInstance {
	instancesMu.Lock()
	items := make([]agentInstance, 0, len(instances))
	for _, inst := range instances {
		items = append(items, *inst)
	}
	instancesMu.Unlock()

	sort.Slice(items, func(i, j int) bool {
		return items[i].Updated.Before(items[j].Updated)
	})
	return items
}

func canonicalPath(cwd string) string {
	if strings.TrimSpace(cwd) == "" {
		var err error
		cwd, err = os.Getwd()
		if err != nil {
			return "unknown"
		}
	}
	abs, err := filepath.Abs(cwd)
	if err != nil {
		abs = cwd
	}
	if resolved, err := filepath.EvalSymlinks(abs); err == nil {
		abs = resolved
	}
	return filepath.Clean(abs)
}

func configuredAddress() (bluetooth.Address, bool) {
	text := strings.TrimSpace(os.Getenv("AGENT_VIEWER_ADDR"))
	if text == "" {
		return bluetooth.Address{}, false
	}
	mac, err := bluetooth.ParseMAC(strings.ToUpper(text))
	if err != nil {
		fmt.Printf("[ble] invalid AGENT_VIEWER_ADDR %q: %v\n", text, err)
		return bluetooth.Address{}, false
	}
	return bluetooth.Address{MACAddress: bluetooth.MACAddress{MAC: mac}}, true
}

func instanceID(key string) string {
	h := fnv.New32a()
	_, _ = h.Write([]byte(key))
	return fmt.Sprintf("%08x", h.Sum32())
}

func deriveLabelsLocked() {
	baseCounts := map[string]int{}
	for _, inst := range instances {
		baseCounts[filepath.Base(inst.Key)]++
	}

	candidates := map[string]string{}
	candidateCounts := map[string]int{}
	for id, inst := range instances {
		candidate := strings.TrimSpace(inst.UserLabel)
		if candidate == "" {
			base := filepath.Base(inst.Key)
			if baseCounts[base] == 1 {
				candidate = base
			} else {
				parent := filepath.Base(filepath.Dir(inst.Key))
				if parent == "." || parent == string(filepath.Separator) || parent == "" {
					candidate = base
				} else {
					candidate = parent + "/" + base
				}
			}
		}
		candidate = sanitizeField(candidate, maxLabelLen)
		candidates[id] = candidate
		candidateCounts[candidate]++
	}

	for id, inst := range instances {
		label := candidates[id]
		if candidateCounts[label] > 1 {
			label = sanitizeField(label+"-"+id[:4], maxLabelLen)
		}
		inst.Label = label
	}
}

func newestInstanceIDLocked() string {
	var newest string
	var newestTime time.Time
	for id, inst := range instances {
		if newest == "" || inst.Updated.After(newestTime) {
			newest = id
			newestTime = inst.Updated
		}
	}
	return newest
}

func sanitizeField(s string, maxLen int) string {
	s = strings.TrimSpace(s)
	s = strings.Map(func(r rune) rune {
		switch r {
		case '\t', '\r', '\n':
			return ' '
		default:
			return r
		}
	}, s)
	if s == "" {
		return "Agent"
	}
	runes := []rune(s)
	if len(runes) > maxLen {
		s = string(runes[:maxLen])
	}
	return s
}

func main() {
	fmt.Println("=== Agent Viewer Daemon ===")
	adapter.Enable()
	go unixServer()
	go statsSync()
	go pruneLoop()
	connectLoop()
}

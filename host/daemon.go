package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"io"
	"net"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	_ "github.com/mattn/go-sqlite3"
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

	maxLabelLen    = 32
	maxStatusLen   = 32
	maxProviderLen = 12

	stateIdle     byte = 0
	stateThinking byte = 1
	stateWaiting  byte = 2
	stateSuccess  byte = 3
)

var claudeEventToState = map[string]byte{
	"SessionStart": stateIdle,
	"SessionEnd":   stateIdle,
	"PreToolUse":   stateThinking,
	"Notification": stateWaiting,
	"Stop":         stateSuccess,
}

var claudeEventToStatus = map[string]string{
	"SessionStart": "Started",
	"SessionEnd":   "Ended",
	"PreToolUse":   "Thinking",
	"Notification": "Waiting",
	"Stop":         "Success",
}

var codexEventToState = map[string]byte{
	"SessionStart":      stateIdle,
	"SessionEnd":        stateIdle,
	"UserPromptSubmit":  stateThinking,
	"PreToolUse":        stateThinking,
	"PermissionRequest": stateWaiting,
	"Notification":      stateWaiting,
	"Stop":              stateSuccess,
}

var codexEventToStatus = map[string]string{
	"SessionStart":      "Started",
	"SessionEnd":        "Ended",
	"UserPromptSubmit":  "Thinking",
	"PreToolUse":        "Thinking",
	"PermissionRequest": "Approval",
	"Notification":      "Waiting",
	"Stop":              "Success",
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
	gattMu         sync.Mutex

	instancesMu sync.Mutex
	instances   = map[string]*agentInstance{}
	focusedID   string
)

type hookEvent struct {
	Event       string     `json:"event"`
	CWD         string     `json:"cwd"`
	SessionID   string     `json:"session_id,omitempty"`
	Label       string     `json:"label,omitempty"`
	Provider    string     `json:"provider,omitempty"`
	Model       string     `json:"model,omitempty"`
	ToolName    string     `json:"tool_name,omitempty"`
	TurnID      string     `json:"turn_id,omitempty"`
	Process     processRef `json:"process,omitempty"`
	TimestampMS int64      `json:"timestamp_ms"`
}

type agentInstance struct {
	ID        string
	Key       string
	UserLabel string
	Label     string
	Provider  string
	SessionID string
	Model     string
	Event     string
	Status    string
	State     byte
	Updated   time.Time
	EndedAt   time.Time
	Process   processRef
}

type claudeStats struct {
	TotalInputTokens  int     `json:"totalInputTokens"`
	TotalOutputTokens int     `json:"totalOutputTokens"`
	TotalCost         float64 `json:"totalCost"`
}

func readClaudeStats() string {
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

func readInstanceStats(inst agentInstance) string {
	if normalizeProvider(inst.Provider) == "Codex" {
		if stats := readCodexStats(inst.SessionID, inst.Key); stats != "" {
			return stats
		}
		return inst.Status
	}
	return readClaudeStats()
}

func readCodexStats(sessionID, cwd string) string {
	path := codexStatePath()
	if path == "" {
		return ""
	}

	dbURL := url.URL{
		Scheme:   "file",
		Path:     path,
		RawQuery: "mode=ro&_query_only=true",
	}
	db, err := sql.Open("sqlite3", dbURL.String())
	if err != nil {
		return ""
	}
	defer db.Close()
	db.SetMaxOpenConns(1)

	tokens, ok := codexTokensForThread(db, sessionID, cwd)
	if !ok {
		return ""
	}
	return formatTokenStats(tokens)
}

func codexStatePath() string {
	if path := strings.TrimSpace(os.Getenv("AGENT_VIEWER_CODEX_STATE_DB")); path != "" {
		return path
	}
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	return filepath.Join(home, ".codex", "state_5.sqlite")
}

func codexTokensForThread(db *sql.DB, sessionID, cwd string) (int64, bool) {
	if strings.TrimSpace(sessionID) != "" {
		var tokens sql.NullInt64
		if err := db.QueryRow("select tokens_used from threads where id = ? limit 1", sessionID).Scan(&tokens); err == nil && tokens.Valid {
			return tokens.Int64, true
		}
	}

	if strings.TrimSpace(cwd) != "" {
		var tokens sql.NullInt64
		err := db.QueryRow(
			"select tokens_used from threads where cwd = ? order by updated_at_ms desc limit 1",
			cwd,
		).Scan(&tokens)
		if err == nil && tokens.Valid {
			return tokens.Int64, true
		}
	}

	return 0, false
}

func formatTokenStats(tokens int64) string {
	if tokens < 1000 {
		return fmt.Sprintf("Tokens %d", tokens)
	}
	if tokens < 1000000 {
		return fmt.Sprintf("Tokens %.1fk", float64(tokens)/1000.0)
	}
	return fmt.Sprintf("Tokens %.1fm", float64(tokens)/1000000.0)
}

func eventStateAndStatus(provider, event string) (byte, string, bool) {
	if normalizeProvider(provider) == "Codex" {
		state, ok := codexEventToState[event]
		if !ok {
			return 0, "", false
		}
		status := codexEventToStatus[event]
		if status == "" {
			status = event
		}
		return state, status, true
	}

	state, ok := claudeEventToState[event]
	if !ok {
		return 0, "", false
	}
	status := claudeEventToStatus[event]
	if status == "" {
		status = event
	}
	return state, status, true
}

func normalizeProvider(provider string) string {
	switch strings.ToLower(strings.TrimSpace(provider)) {
	case "codex", "openai codex":
		return "Codex"
	case "claude", "claude code", "":
		return "Claude"
	default:
		return sanitizeField(provider, maxProviderLen)
	}
}

func markDisconnected(err error) {
	connMu.Lock()
	wasConnected := connected
	connected = false
	multiSupported = false
	connMu.Unlock()
	if err != nil && wasConnected {
		fmt.Printf("[ble] disconnected: %v\n", err)
	}
}

func writeCharacteristic(ch bluetooth.DeviceCharacteristic, data []byte) error {
	gattMu.Lock()
	defer gattMu.Unlock()

	var err error
	for attempt := 0; attempt < 3; attempt++ {
		if _, err = ch.WriteWithoutResponse(data); !isGattInProgress(err) {
			return err
		}
		time.Sleep(50 * time.Millisecond)
	}
	return err
}

func readCharacteristic(ch bluetooth.DeviceCharacteristic, data []byte) error {
	gattMu.Lock()
	defer gattMu.Unlock()

	var err error
	for attempt := 0; attempt < 3; attempt++ {
		if _, err = ch.Read(data); !isGattInProgress(err) {
			return err
		}
		time.Sleep(50 * time.Millisecond)
	}
	return err
}

func isGattInProgress(err error) bool {
	return err != nil && strings.Contains(strings.ToLower(err.Error()), "in progress")
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
	if err := writeCharacteristic(ch, []byte{v}); err != nil {
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
	if err := writeCharacteristic(ch, []byte(text)); err != nil {
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
	if err := writeCharacteristic(ch, []byte(payload)); err != nil {
		markDisconnected(err)
		return false
	}
	return true
}

func sendInstance(inst agentInstance) {
	payload := fmt.Sprintf("U\t%s\t%d\t%s\t%s\t%s",
		inst.ID,
		inst.State,
		sanitizeField(inst.Label, maxLabelLen),
		sanitizeField(inst.Status, maxStatusLen),
		sanitizeField(normalizeProvider(inst.Provider), maxProviderLen),
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
			if err := writeCharacteristic(nc, []byte(hostname)); err != nil {
				fmt.Printf("[ble] send hostname failed: %v\n", err)
				dev.Disconnect()
				continue
			}
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
			sendStats(readClaudeStats())
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
	provider := normalizeProvider(ev.Provider)
	processLabel := ""
	if ev.Process.PID > 0 {
		processLabel = fmt.Sprintf(" pid=%d", ev.Process.PID)
	}
	fmt.Printf("[unix] event: %s provider=%s cwd=%s%s\n", ev.Event, provider, ev.CWD, processLabel)

	state, status, ok := eventStateAndStatus(provider, ev.Event)
	if !ok {
		return
	}

	updated := time.Now()
	if ev.TimestampMS > 0 {
		updated = time.UnixMilli(ev.TimestampMS)
	}

	key := canonicalPath(ev.CWD)
	id := instanceID(instanceKey(provider, key, ev.SessionID))

	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok {
		inst = &agentInstance{ID: id, Key: key}
		instances[id] = inst
	}
	inst.UserLabel = strings.TrimSpace(ev.Label)
	inst.Provider = provider
	inst.SessionID = strings.TrimSpace(ev.SessionID)
	inst.Model = strings.TrimSpace(ev.Model)
	inst.Event = ev.Event
	inst.State = state
	inst.Status = status
	inst.Updated = updated
	if ev.Process.PID > 0 && strings.TrimSpace(ev.Process.StartTime) != "" {
		inst.Process = ev.Process
	}
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

	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok || focusedID != id || !inst.Updated.Equal(eventTime) {
		instancesMu.Unlock()
		return
	}
	idleState, _, ok := eventStateAndStatus(inst.Provider, "SessionEnd")
	if !ok || inst.State != stateSuccess {
		instancesMu.Unlock()
		return
	}
	statsInst := *inst
	instancesMu.Unlock()

	stats := readInstanceStats(statsInst)

	instancesMu.Lock()
	inst, ok = instances[id]
	if !ok || focusedID != id || !inst.Updated.Equal(eventTime) || inst.State != stateSuccess {
		instancesMu.Unlock()
		return
	}
	inst.State = idleState
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
			sendStats(readClaudeStats())
		}
	}
}

func healthLoop() {
	for range time.NewTicker(5 * time.Second).C {
		connMu.RLock()
		ok := connected
		ch := actionCh
		connMu.RUnlock()
		if !ok {
			continue
		}

		var buf [1]byte
		if err := readCharacteristic(ch, buf[:]); err != nil {
			markDisconnected(fmt.Errorf("health check failed: %w", err))
		}
	}
}

func pruneLoop() {
	for range time.NewTicker(10 * time.Second).C {
		deleted := pruneInstances(time.Now())
		sendDeletedInstances(deleted)
	}
}

func pruneInstances(now time.Time) []string {
	var deleted []string

	instancesMu.Lock()
	for id, inst := range instances {
		processExited := inst.Process.PID > 0 && !processAlive(inst.Process)
		expiredEnded := !inst.EndedAt.IsZero() && now.Sub(inst.EndedAt) > 30*time.Minute
		expiredStale := now.Sub(inst.Updated) > 2*time.Hour
		if processExited || expiredEnded || expiredStale {
			deleted = append(deleted, id)
			delete(instances, id)
			if processExited {
				fmt.Printf("[daemon] removed %s: process %d exited\n", inst.Label, inst.Process.PID)
			}
		}
	}
	if len(deleted) > 0 && instances[focusedID] == nil {
		focusedID = newestInstanceIDLocked()
	}
	deriveLabelsLocked()
	instancesMu.Unlock()

	return deleted
}

func sendDeletedInstances(deleted []string) {
	for _, id := range deleted {
		sendDelete(id)
	}
	if len(deleted) > 0 {
		sendSnapshot()
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

func instanceKey(provider, cwd, sessionID string) string {
	provider = normalizeProvider(provider)
	sessionID = strings.TrimSpace(sessionID)
	if sessionID == "" {
		return provider + "\x00" + cwd
	}
	return provider + "\x00" + cwd + "\x00" + sessionID
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

func runDaemon() error {
	fmt.Println("=== Agent Viewer Daemon ===")
	if err := adapter.Enable(); err != nil {
		return fmt.Errorf("enable bluetooth adapter: %w", err)
	}
	go unixServer()
	go statsSync()
	go healthLoop()
	go pruneLoop()
	connectLoop()
	return nil
}

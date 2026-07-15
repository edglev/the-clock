package main

import (
	"bufio"
	"context"
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
	actionUUID = "00000000-0000-a359-42f0-4467de900004"
	nameUUID   = "00000000-0000-a359-42f0-4467de900005"
	multiUUID  = "00000000-0000-a359-42f0-4467de900006"
	sockPath   = "/tmp/agent-viewer.sock"

	maxLabelLen    = 32
	maxStatusLen   = 32
	maxProviderLen = 12
	maxBranchLen   = 24
	maxMetricsLen  = 20
	maxModelLen    = 24
	maxEffortLen   = 12

	stateIdle     byte = 0
	stateThinking byte = 1
	stateWaiting  byte = 2
	stateSuccess  byte = 3

	successAutoIdleDelay = 2 * time.Second
	waitingAutoIdleDelay = 30 * time.Second
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
	adapter   = bluetooth.DefaultAdapter
	actionCh  bluetooth.DeviceCharacteristic
	nameCh    bluetooth.DeviceCharacteristic
	multiCh   bluetooth.DeviceCharacteristic
	connected bool
	connMu    sync.RWMutex
	gattMu    sync.Mutex

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
	Effort      string     `json:"effort,omitempty"`
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
	Effort    string
	Branch    string
	Event     string
	Status    string
	State     byte
	Updated   time.Time
	EndedAt   time.Time
	Process   processRef
}

type claudeStats struct {
	TotalInputTokens  int64                         `json:"totalInputTokens"`
	TotalOutputTokens int64                         `json:"totalOutputTokens"`
	TotalCost         float64                       `json:"totalCost"`
	ModelUsage        map[string]claudeModelUsage   `json:"modelUsage"`
	DailyModelTokens  []claudeDailyModelTokenBucket `json:"dailyModelTokens"`
}

type claudeModelUsage struct {
	InputTokens              int64   `json:"inputTokens"`
	OutputTokens             int64   `json:"outputTokens"`
	CacheReadInputTokens     int64   `json:"cacheReadInputTokens"`
	CacheCreationInputTokens int64   `json:"cacheCreationInputTokens"`
	CostUSD                  float64 `json:"costUSD"`
}

type claudeDailyModelTokenBucket struct {
	TokensByModel map[string]claudeModelUsage `json:"tokensByModel"`
}

type claudeMessageUsage struct {
	InputTokens              int64   `json:"input_tokens"`
	OutputTokens             int64   `json:"output_tokens"`
	CacheReadInputTokens     int64   `json:"cache_read_input_tokens"`
	CacheCreationInputTokens int64   `json:"cache_creation_input_tokens"`
	CostUSD                  float64 `json:"costUSD"`
}

type usageSummary struct {
	Tokens int64
	Cost   float64
}

type claudeProjectEntry struct {
	SessionID string          `json:"sessionId"`
	RequestID string          `json:"requestId"`
	Message   json.RawMessage `json:"message"`
}

func readClaudeStats() string {
	usage, ok := readClaudeAggregateUsage()
	if !ok {
		return "no stats"
	}
	return formatUsageStats(usage)
}

func readClaudeInstanceStats(inst agentInstance) string {
	if usage, ok := readClaudeProjectUsage(inst.SessionID, inst.Key); ok {
		return formatUsageStats(usage)
	}
	return readClaudeStats()
}

func readClaudeAggregateUsage() (usageSummary, bool) {
	home, err := os.UserHomeDir()
	if err != nil {
		return usageSummary{}, false
	}
	path := filepath.Join(home, ".claude", "stats-cache.json")
	f, err := os.Open(path)
	if err != nil {
		return usageSummary{}, false
	}
	defer f.Close()
	return parseClaudeStatsCache(f)
}

func readClaudeProjectUsage(sessionID, cwd string) (usageSummary, bool) {
	home, err := os.UserHomeDir()
	if err != nil {
		return usageSummary{}, false
	}
	projectDir := claudeProjectDir(home, cwd)
	if projectDir == "" {
		return usageSummary{}, false
	}

	if strings.TrimSpace(sessionID) != "" {
		path := filepath.Join(projectDir, strings.TrimSpace(sessionID)+".jsonl")
		if f, err := os.Open(path); err == nil {
			defer f.Close()
			return parseClaudeProjectUsage(f, sessionID)
		}
	}

	path := newestClaudeProjectFile(projectDir)
	if path == "" {
		return usageSummary{}, false
	}
	f, err := os.Open(path)
	if err != nil {
		return usageSummary{}, false
	}
	defer f.Close()
	return parseClaudeProjectUsage(f, "")
}

func parseClaudeStatsCache(r io.Reader) (usageSummary, bool) {
	var s claudeStats
	if json.NewDecoder(r).Decode(&s) != nil {
		return usageSummary{}, false
	}

	var usage usageSummary
	usage.Tokens = s.TotalInputTokens + s.TotalOutputTokens
	usage.Cost = s.TotalCost

	if len(s.ModelUsage) > 0 {
		usage = usageSummary{}
		addClaudeModelUsage(&usage, s.ModelUsage)
	}

	if usage.Tokens == 0 && len(s.DailyModelTokens) > 0 {
		for _, day := range s.DailyModelTokens {
			addClaudeModelUsage(&usage, day.TokensByModel)
		}
	}

	return usage, usage.Tokens > 0 || usage.Cost > 0
}

func addClaudeModelUsage(summary *usageSummary, usageByModel map[string]claudeModelUsage) {
	for _, usage := range usageByModel {
		summary.Tokens += usage.InputTokens +
			usage.OutputTokens +
			usage.CacheReadInputTokens +
			usage.CacheCreationInputTokens
		summary.Cost += usage.CostUSD
	}
}

func parseClaudeProjectUsage(r io.Reader, sessionID string) (usageSummary, bool) {
	var usage usageSummary
	seenRequests := map[string]bool{}
	sessionID = strings.TrimSpace(sessionID)

	reader := bufio.NewReader(r)
	for {
		line, err := reader.ReadBytes('\n')
		if len(strings.TrimSpace(string(line))) > 0 {
			var entry claudeProjectEntry
			if json.Unmarshal(line, &entry) == nil {
				if sessionID == "" || entry.SessionID == "" || entry.SessionID == sessionID {
					alreadyCounted := false
					if entry.RequestID != "" {
						if seenRequests[entry.RequestID] {
							alreadyCounted = true
						} else {
							seenRequests[entry.RequestID] = true
						}
					}

					if !alreadyCounted {
						var message struct {
							Usage claudeMessageUsage `json:"usage"`
						}
						if json.Unmarshal(entry.Message, &message) == nil {
							usage.Tokens += message.Usage.InputTokens +
								message.Usage.OutputTokens +
								message.Usage.CacheReadInputTokens +
								message.Usage.CacheCreationInputTokens
							usage.Cost += message.Usage.CostUSD
						}
					}
				}
			}
		}

		if err == io.EOF {
			break
		}
		if err != nil {
			break
		}
	}

	return usage, usage.Tokens > 0 || usage.Cost > 0
}

func claudeProjectDir(home, cwd string) string {
	cwd = strings.TrimSpace(cwd)
	if cwd == "" || cwd == "unknown" {
		return ""
	}
	clean := filepath.Clean(cwd)
	projectName := strings.ReplaceAll(clean, string(filepath.Separator), "-")
	return filepath.Join(home, ".claude", "projects", projectName)
}

func newestClaudeProjectFile(projectDir string) string {
	entries, err := os.ReadDir(projectDir)
	if err != nil {
		return ""
	}

	var newest string
	var newestTime time.Time
	for _, entry := range entries {
		if entry.IsDir() || !strings.HasSuffix(entry.Name(), ".jsonl") {
			continue
		}
		info, err := entry.Info()
		if err != nil {
			continue
		}
		if newest == "" || info.ModTime().After(newestTime) {
			newest = filepath.Join(projectDir, entry.Name())
			newestTime = info.ModTime()
		}
	}
	return newest
}

func formatUsageStats(usage usageSummary) string {
	if usage.Cost > 0 && usage.Tokens > 0 {
		return fmt.Sprintf("Cost $%.2f T %s", usage.Cost, formatTokenCount(usage.Tokens))
	}
	if usage.Cost > 0 {
		return fmt.Sprintf("Cost $%.2f", usage.Cost)
	}
	if usage.Tokens > 0 {
		return formatTokenStats(usage.Tokens)
	}
	return "no stats"
}

func readInstanceStats(inst agentInstance) string {
	if normalizeProvider(inst.Provider) == "Codex" {
		if tokens, ok := readCodexTokenCount(inst.SessionID, inst.Key); ok {
			return formatTokenStats(tokens)
		}
		return inst.Status
	}
	return readClaudeInstanceStats(inst)
}

func readInstanceMetrics(inst agentInstance) string {
	if normalizeProvider(inst.Provider) != "Codex" {
		return ""
	}
	tokens, ok := readCodexTokenCount(inst.SessionID, inst.Key)
	if !ok {
		return ""
	}
	return formatTokenMetrics(tokens)
}

func readCodexStats(sessionID, cwd string) string {
	tokens, ok := readCodexTokenCount(sessionID, cwd)
	if !ok {
		return ""
	}
	return formatTokenStats(tokens)
}

func readCodexTokenCount(sessionID, cwd string) (int64, bool) {
	path := codexStatePath()
	if path == "" {
		return 0, false
	}

	dbURL := url.URL{
		Scheme:   "file",
		Path:     path,
		RawQuery: "mode=ro&_query_only=true",
	}
	db, err := sql.Open("sqlite3", dbURL.String())
	if err != nil {
		return 0, false
	}
	defer db.Close()
	db.SetMaxOpenConns(1)

	return codexTokensForThread(db, sessionID, cwd)
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
	return "Tokens " + formatTokenCount(tokens)
}

func formatTokenMetrics(tokens int64) string {
	return formatTokenCount(tokens) + " tok"
}

func formatTokenCount(tokens int64) string {
	if tokens < 1000 {
		return fmt.Sprintf("%d", tokens)
	}
	if tokens < 1000000 {
		return formatScaledNumber(float64(tokens)/1000.0, "k")
	}
	return formatScaledNumber(float64(tokens)/1000000.0, "m")
}

func formatScaledNumber(value float64, suffix string) string {
	text := fmt.Sprintf("%.1f", value)
	text = strings.TrimSuffix(text, ".0")
	return text + suffix
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

func hookEventStateAndStatus(ev hookEvent) (byte, string, bool) {
	if normalizeProvider(ev.Provider) == "Codex" &&
		ev.Event == "PreToolUse" &&
		strings.TrimSpace(ev.ToolName) == "request_user_input" {
		return stateWaiting, "Question", true
	}
	return eventStateAndStatus(ev.Provider, ev.Event)
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

func sendMultiPayload(payload string) bool {
	connMu.RLock()
	ok := connected
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
	metrics := readInstanceMetrics(inst)
	payload := fmt.Sprintf("U\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s",
		inst.ID,
		inst.State,
		sanitizeField(inst.Label, maxLabelLen),
		sanitizeField(inst.Status, maxStatusLen),
		sanitizeField(normalizeProvider(inst.Provider), maxProviderLen),
		sanitizeOptionalField(inst.Branch, maxBranchLen),
		sanitizeOptionalField(metrics, maxMetricsLen),
		sanitizeOptionalField(inst.Model, maxModelLen),
		sanitizeOptionalField(inst.Effort, maxEffortLen),
	)
	sendMultiPayload(payload)
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

		chars, err := svc.DiscoverCharacteristics([]bluetooth.UUID{actUUID})
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

		chars, err = svc.DiscoverCharacteristics([]bluetooth.UUID{maUUID})
		if err != nil || len(chars) == 0 {
			fmt.Println("[ble] multi-agent char not found")
			dev.Disconnect()
			continue
		}
		mc := chars[0]
		fmt.Println("[ble] multi-agent char found")

		connMu.Lock()
		actionCh = ac
		nameCh = nc
		multiCh = mc
		connected = true
		connMu.Unlock()

		fmt.Println("[ble] connected")
		sendSnapshot()
		sendCurrentPrinterStatus()
		sendCurrentAMSStatus()
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

	state, status, ok := hookEventStateAndStatus(ev)
	if !ok {
		return
	}

	updated := time.Now()
	if ev.TimestampMS > 0 {
		updated = time.UnixMilli(ev.TimestampMS)
	}

	key := canonicalPath(ev.CWD)
	id := instanceID(instanceKey(provider, key, ev.Process))

	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok {
		inst = &agentInstance{ID: id, Key: key}
		instances[id] = inst
	}
	inst.UserLabel = strings.TrimSpace(ev.Label)
	inst.Provider = provider
	inst.SessionID = strings.TrimSpace(ev.SessionID)
	if model := strings.TrimSpace(ev.Model); model != "" {
		inst.Model = model
	}
	if effort := strings.TrimSpace(ev.Effort); effort != "" {
		inst.Effort = effort
	}
	inst.Branch = currentGitBranch(key)
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
	} else if provider == "Claude" && ev.Event == "Notification" {
		go autoIdleAfterWaiting(id, updated)
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
	time.Sleep(successAutoIdleDelay)
	autoIdleIfCurrent(id, eventTime, stateSuccess)
}

func autoIdleAfterWaiting(id string, eventTime time.Time) {
	time.Sleep(waitingAutoIdleDelay)
	autoIdleIfCurrent(id, eventTime, stateWaiting)
}

func autoIdleIfCurrent(id string, eventTime time.Time, requiredState byte) {
	instancesMu.Lock()
	inst, ok := instances[id]
	if !ok || !inst.Updated.Equal(eventTime) {
		instancesMu.Unlock()
		return
	}
	idleState, _, ok := eventStateAndStatus(inst.Provider, "SessionEnd")
	if !ok || inst.State != requiredState {
		instancesMu.Unlock()
		return
	}
	statsInst := *inst
	instancesMu.Unlock()

	stats := readInstanceStats(statsInst)

	instancesMu.Lock()
	inst, ok = instances[id]
	if !ok || !inst.Updated.Equal(eventTime) || inst.State != requiredState {
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

func autoIdleTimedOutWaiting(now time.Time) bool {
	var candidates []agentInstance

	instancesMu.Lock()
	for _, inst := range instances {
		if normalizeProvider(inst.Provider) != "Claude" || inst.State != stateWaiting {
			continue
		}
		if now.Sub(inst.Updated) < waitingAutoIdleDelay {
			continue
		}
		candidates = append(candidates, *inst)
	}
	instancesMu.Unlock()

	if len(candidates) == 0 {
		return false
	}

	statsByID := make(map[string]string, len(candidates))
	for _, inst := range candidates {
		statsByID[inst.ID] = readInstanceStats(inst)
	}

	changed := false
	instancesMu.Lock()
	for _, candidate := range candidates {
		inst, ok := instances[candidate.ID]
		if !ok || normalizeProvider(inst.Provider) != "Claude" || inst.State != stateWaiting {
			continue
		}
		if !inst.Updated.Equal(candidate.Updated) || now.Sub(inst.Updated) < waitingAutoIdleDelay {
			continue
		}
		inst.State = stateIdle
		inst.Status = statsByID[candidate.ID]
		inst.Updated = now
		changed = true
	}
	if changed {
		deriveLabelsLocked()
	}
	instancesMu.Unlock()

	return changed
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
		now := time.Now()
		deleted := pruneInstances(now)
		sendDeletedInstances(deleted)
		if autoIdleTimedOutWaiting(now) {
			sendSnapshot()
		}
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

func currentGitBranch(cwd string) string {
	if strings.TrimSpace(cwd) == "" || cwd == "unknown" {
		return ""
	}

	if branch := runGit(cwd, "branch", "--show-current"); branch != "" {
		return branch
	}
	if rev := runGit(cwd, "rev-parse", "--short", "HEAD"); rev != "" {
		return "detached " + rev
	}
	return ""
}

func runGit(cwd string, args ...string) string {
	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()

	cmdArgs := append([]string{"-C", cwd}, args...)
	out, err := exec.CommandContext(ctx, "git", cmdArgs...).Output()
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(out))
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

func instanceKey(provider, cwd string, process processRef) string {
	provider = normalizeProvider(provider)
	if process.PID > 0 && strings.TrimSpace(process.StartTime) != "" {
		return fmt.Sprintf("%s\x00process\x00%d\x00%s", provider, process.PID, process.StartTime)
	}
	return provider + "\x00cwd\x00" + cwd
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
	s = cleanField(s, maxLen)
	if s == "" {
		return "Agent"
	}
	return s
}

func sanitizeOptionalField(s string, maxLen int) string {
	return cleanField(s, maxLen)
}

func cleanField(s string, maxLen int) string {
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
		return ""
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
	go healthLoop()
	go pruneLoop()
	startPrinterStatusLoop()
	connectLoop()
	return nil
}

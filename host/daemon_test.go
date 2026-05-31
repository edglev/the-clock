package main

import (
	"database/sql"
	"errors"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestEventStateAndStatus(t *testing.T) {
	state, status, ok := eventStateAndStatus("codex", "PermissionRequest")
	if !ok || state != stateWaiting || status != "Approval" {
		t.Fatalf("Codex PermissionRequest = (%d, %q, %v)", state, status, ok)
	}

	state, status, ok = eventStateAndStatus("claude", "Notification")
	if !ok || state != stateWaiting || status != "Waiting" {
		t.Fatalf("Claude Notification = (%d, %q, %v)", state, status, ok)
	}

	if _, _, ok := eventStateAndStatus("codex", "PostToolUse"); ok {
		t.Fatal("unexpected PostToolUse mapping")
	}
}

func TestInstanceKeyUsesProcessBeforeSession(t *testing.T) {
	cwd := "/tmp/project"
	procA := processRef{PID: 123, StartTime: "555"}
	procB := processRef{PID: 124, StartTime: "777"}
	codexA := instanceID(instanceKey("codex", cwd, procA))
	codexSameProcess := instanceID(instanceKey("codex", cwd, procA))
	codexB := instanceID(instanceKey("codex", cwd, procB))
	claude := instanceID(instanceKey("claude", cwd, procA))

	if codexA != codexSameProcess {
		t.Fatal("same Codex process did not collapse to one instance id")
	}
	if codexA == codexB {
		t.Fatal("different Codex processes collapsed to one instance id")
	}
	if codexA == claude {
		t.Fatal("different providers collapsed to one instance id")
	}
}

func TestInstanceKeyFallsBackToProviderAndCWD(t *testing.T) {
	cwd := "/tmp/project"
	codexA := instanceID(instanceKey("codex", cwd, processRef{}))
	codexB := instanceID(instanceKey("codex", cwd, processRef{}))
	claude := instanceID(instanceKey("claude", cwd, processRef{}))

	if codexA != codexB {
		t.Fatal("same provider and cwd without process did not collapse")
	}
	if codexA == claude {
		t.Fatal("different providers collapsed to one fallback instance id")
	}
}

func TestCodexTokensForThread(t *testing.T) {
	db := openTestCodexDB(t)

	if _, err := db.Exec(`
		create table threads (
			id text primary key,
			cwd text,
			tokens_used integer,
			updated_at_ms integer
		)
	`); err != nil {
		t.Fatal(err)
	}

	rows := []struct {
		id      string
		cwd     string
		tokens  int64
		updated int64
	}{
		{"session-a", "/tmp/project", 1234, 100},
		{"session-b", "/tmp/project", 4567, 200},
		{"session-c", "/tmp/other", 9999, 300},
	}
	for _, row := range rows {
		if _, err := db.Exec(
			"insert into threads (id, cwd, tokens_used, updated_at_ms) values (?, ?, ?, ?)",
			row.id,
			row.cwd,
			row.tokens,
			row.updated,
		); err != nil {
			t.Fatal(err)
		}
	}

	tokens, ok := codexTokensForThread(db, "session-a", "/tmp/project")
	if !ok || tokens != 1234 {
		t.Fatalf("session lookup = (%d, %v)", tokens, ok)
	}

	tokens, ok = codexTokensForThread(db, "", "/tmp/project")
	if !ok || tokens != 4567 {
		t.Fatalf("cwd fallback = (%d, %v)", tokens, ok)
	}

	if stats := formatTokenStats(tokens); stats != "Tokens 4.6k" {
		t.Fatalf("formatted stats = %q", stats)
	}

	if metrics := formatTokenMetrics(423); metrics != "423 tok" {
		t.Fatalf("formatted small metrics = %q", metrics)
	}

	if metrics := formatTokenMetrics(tokens); metrics != "4.6k tok" {
		t.Fatalf("formatted metrics = %q", metrics)
	}
}

func TestFormatPrinterPayloadSanitizesFields(t *testing.T) {
	payload := formatPrinterPayload(printerStatus{
		State:           "printing",
		ProgressPercent: 42,
		EtaSeconds:      3600,
		Layer:           12,
		Layers:          120,
		NozzleC:         "220.5",
		BedC:            "60",
		ChamberC:        "35",
		Job:             "clock\tbezel\nvery-long-print-name-that-should-be-truncated.3mf",
		Material:        "PLA",
		Source:          "Cloud",
		Updated:         time.UnixMilli(1234567890),
	})

	fields := strings.Split(payload, "\t")
	if len(fields) != 13 {
		t.Fatalf("payload field count = %d: %q", len(fields), payload)
	}
	if fields[0] != "P" || fields[1] != "printing" || fields[2] != "42" {
		t.Fatalf("unexpected payload prefix: %q", payload)
	}
	if strings.Contains(fields[9], "\n") || strings.Contains(fields[9], "\t") {
		t.Fatalf("job was not sanitized: %q", fields[9])
	}
	if len([]rune(fields[9])) > maxPrinterJobLen {
		t.Fatalf("job was not truncated: %q", fields[9])
	}
}

func TestPrinterReconnectDelayBackoff(t *testing.T) {
	delay := nextPrinterReconnectDelay(0)
	if delay != printerReconnectMin {
		t.Fatalf("initial delay = %s", delay)
	}

	delay = printerReconnectMin
	for i := 0; i < 10; i++ {
		delay = nextPrinterReconnectDelay(delay)
	}
	if delay != printerReconnectMax {
		t.Fatalf("max delay = %s", delay)
	}
}

func TestBambuOfflineJob(t *testing.T) {
	if job := bambuOfflineJob(errors.New("Bambu config missing; run `agent-viewer bambu-login`")); job != "Run bambu-login" {
		t.Fatalf("config job = %q", job)
	}
	if job := bambuOfflineJob(errors.New("Bambu MQTT disconnected")); job != "Bambu Cloud disconnected" {
		t.Fatalf("disconnect job = %q", job)
	}
}

func TestSamePrinterStatusViewIgnoresUpdated(t *testing.T) {
	a := printerStatus{
		State:           "printing",
		ProgressPercent: 42,
		Job:             "clock.3mf",
		Source:          "Cloud",
		Updated:         time.Unix(1, 0),
	}
	b := a
	b.Updated = time.Unix(2, 0)

	if !samePrinterStatusView(a, b) {
		t.Fatal("same view differed only by Updated")
	}
	b.ProgressPercent = 43
	if samePrinterStatusView(a, b) {
		t.Fatal("different progress was treated as same view")
	}
}

func TestPrinterStatusFromBambuPrint(t *testing.T) {
	status := printerStatusFromBambuPrint(map[string]any{
		"gcode_state":       "RUNNING",
		"mc_percent":        float64(64),
		"mc_remaining_time": float64(41),
		"layer_num":         float64(77),
		"total_layer_num":   float64(120),
		"nozzle_temper":     float64(219.4),
		"bed_temper":        float64(60),
		"chamber_temper":    float64(36.2),
		"gcode_file":        "cache/clock-bezel.3mf",
		"vt_tray": map[string]any{
			"tray_type": "PLA",
		},
	})

	if status.State != "printing" {
		t.Fatalf("state = %q", status.State)
	}
	if status.ProgressPercent != 64 || status.EtaSeconds != 2460 {
		t.Fatalf("progress/eta = %d/%d", status.ProgressPercent, status.EtaSeconds)
	}
	if status.Layer != 77 || status.Layers != 120 {
		t.Fatalf("layers = %d/%d", status.Layer, status.Layers)
	}
	if status.NozzleC != "219.4" || status.BedC != "60" || status.ChamberC != "36.2" {
		t.Fatalf("temps = %q/%q/%q", status.NozzleC, status.BedC, status.ChamberC)
	}
	if status.Job != "clock-bezel.3mf" || status.Material != "PLA" {
		t.Fatalf("job/material = %q/%q", status.Job, status.Material)
	}
}

func TestPrinterStatusRunningWinsOverStaleErrors(t *testing.T) {
	status := printerStatusFromBambuPrint(map[string]any{
		"gcode_state":         "RUNNING",
		"mc_percent":          float64(42),
		"mc_print_error_code": "123",
	})
	if status.State != "printing" {
		t.Fatalf("state = %q", status.State)
	}
}

func TestPrinterStatusPausedErrorShowsFailed(t *testing.T) {
	status := printerStatusFromBambuPrint(map[string]any{
		"gcode_state":         "PAUSE",
		"mc_percent":          float64(42),
		"mc_print_error_code": "123",
	})
	if status.State != "error" {
		t.Fatalf("state = %q", status.State)
	}
}

func TestClearStaleBambuErrorFieldsOnHealthyStateUpdate(t *testing.T) {
	merged := map[string]any{
		"gcode_state":         "FAILED",
		"mc_percent":          float64(42),
		"print_error":         float64(117),
		"mc_print_error_code": "123",
		"hms":                 []any{map[string]any{"code": float64(1)}},
	}
	update := map[string]any{
		"gcode_state": "RUNNING",
		"mc_percent":  float64(43),
	}

	mergeJSONMap(merged, update)
	clearStaleBambuErrorFields(merged, update)
	status := printerStatusFromBambuPrint(merged)

	if status.State != "printing" {
		t.Fatalf("state = %q", status.State)
	}
	if _, ok := merged["print_error"]; ok {
		t.Fatal("print_error was not cleared")
	}
	if _, ok := merged["mc_print_error_code"]; ok {
		t.Fatal("mc_print_error_code was not cleared")
	}
	if _, ok := merged["hms"]; ok {
		t.Fatal("hms was not cleared")
	}
}

func openTestCodexDB(t *testing.T) *sql.DB {
	t.Helper()

	path := filepath.Join(t.TempDir(), "state.sqlite")
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		db.Close()
	})
	return db
}

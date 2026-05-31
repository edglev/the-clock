package main

import (
	"database/sql"
	"path/filepath"
	"testing"
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

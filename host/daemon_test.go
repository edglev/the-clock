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

func TestInstanceKeyIncludesProviderAndSession(t *testing.T) {
	cwd := "/tmp/project"
	codexA := instanceID(instanceKey("codex", cwd, "session-a"))
	codexB := instanceID(instanceKey("codex", cwd, "session-b"))
	claude := instanceID(instanceKey("claude", cwd, "session-a"))

	if codexA == codexB {
		t.Fatal("different Codex sessions collapsed to one instance id")
	}
	if codexA == claude {
		t.Fatal("different providers collapsed to one instance id")
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

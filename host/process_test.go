package main

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

func TestParseProcStat(t *testing.T) {
	stat := "1234 (codex helper) S 42 1 1 0 -1 4194560 100 0 0 0 3 2 0 0 20 0 7 0 987654321 0"

	proc, ok := parseProcStat(stat)
	if !ok {
		t.Fatal("parseProcStat returned false")
	}
	if proc.PID != 1234 || proc.PPID != 42 || proc.State != "S" || proc.Command != "codex helper" || proc.StartTime != "987654321" {
		t.Fatalf("unexpected proc info: %+v", proc)
	}
}

func TestMatchesAgentProcess(t *testing.T) {
	shellHook := processInfo{
		Command: "sh",
		Args:    []string{"sh", "-c", "/tmp/agent-viewer hook --provider codex --event PreToolUse"},
	}
	if matchesAgentProcess("codex", shellHook) {
		t.Fatal("shell hook wrapper matched as Codex agent")
	}

	codex := processInfo{Command: "codex", Args: []string{"/usr/local/bin/codex"}}
	if !matchesAgentProcess("codex", codex) {
		t.Fatal("Codex command was not matched")
	}

	codexNode := processInfo{
		Command: "node",
		Args:    []string{"node", "/usr/lib/node_modules/@openai/codex/bin/codex.js"},
	}
	if !matchesAgentProcess("codex", codexNode) {
		t.Fatal("Codex node command was not matched")
	}

	claudeNode := processInfo{
		Command: "node",
		Args:    []string{"node", "/usr/lib/node_modules/@anthropic-ai/claude-code/cli.js"},
	}
	if !matchesAgentProcess("claude", claudeNode) {
		t.Fatal("Claude Code node command was not matched")
	}
}

func TestProcessAliveChecksStartTime(t *testing.T) {
	restore := useTestProcRoot(t)
	defer restore()

	writeProc(t, 123, "123 (codex) S 1 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 555 0", "codex", "codex")

	if !processAlive(processRef{PID: 123, StartTime: "555"}) {
		t.Fatal("expected process to be alive")
	}
	if processAlive(processRef{PID: 123, StartTime: "999"}) {
		t.Fatal("process with reused pid/starttime mismatch reported alive")
	}
	if processAlive(processRef{PID: 999, StartTime: "555"}) {
		t.Fatal("missing process reported alive")
	}
}

func TestPruneInstancesRemovesExitedProcess(t *testing.T) {
	restoreProc := useTestProcRoot(t)
	defer restoreProc()
	restoreInstances := useTestInstances(t)
	defer restoreInstances()

	instances["agent-a"] = &agentInstance{
		ID:      "agent-a",
		Key:     "/tmp/project",
		Label:   "project",
		Updated: time.Now(),
		Process: processRef{PID: 123, StartTime: "555"},
	}

	deleted := pruneInstances(time.Now())
	if len(deleted) != 1 || deleted[0] != "agent-a" {
		t.Fatalf("deleted = %v, want [agent-a]", deleted)
	}
	if _, ok := instances["agent-a"]; ok {
		t.Fatal("dead process instance was not removed")
	}
}

func useTestProcRoot(t *testing.T) func() {
	t.Helper()
	old := procRoot
	procRoot = t.TempDir()
	return func() {
		procRoot = old
	}
}

func useTestInstances(t *testing.T) func() {
	t.Helper()
	oldInstances := instances
	oldFocusedID := focusedID
	instances = map[string]*agentInstance{}
	focusedID = ""
	return func() {
		instances = oldInstances
		focusedID = oldFocusedID
	}
}

func writeProc(t *testing.T, pid int, stat, comm, cmdline string) {
	t.Helper()
	dir := filepath.Join(procRoot, strconv.Itoa(pid))
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "stat"), []byte(stat), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "comm"), []byte(comm+"\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "cmdline"), []byte(cmdline+"\x00"), 0644); err != nil {
		t.Fatal(err)
	}
}

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"time"
)

const sockPath = "/tmp/agent-viewer.sock"

type hookEvent struct {
	Event       string `json:"event"`
	CWD         string `json:"cwd"`
	SessionID   string `json:"session_id,omitempty"`
	Label       string `json:"label,omitempty"`
	TimestampMS int64  `json:"timestamp_ms"`
}

func main() {
	event := flag.String("event", "", "Event name (PreToolUse, Notification, Stop, SessionStart, SessionEnd)")
	cwd := flag.String("cwd", "", "Worktree path; defaults to hook cwd or Claude hook payload cwd")
	sessionID := flag.String("session-id", "", "Optional Claude session id")
	label := flag.String("label", "", "Optional display label")
	flag.Parse()

	stdinFields := readStdinJSON()

	if *event == "" {
		*event = stringField(stdinFields, "hook_event_name")
	}
	if *cwd == "" {
		*cwd = stringField(stdinFields, "cwd")
	}
	if *sessionID == "" {
		*sessionID = stringField(stdinFields, "session_id")
	}
	if *cwd == "" {
		var err error
		*cwd, err = os.Getwd()
		if err != nil {
			*cwd = "unknown"
		}
	}

	if *event == "" {
		fmt.Fprintln(os.Stderr, "error: --event is required")
		os.Exit(1)
	}

	payload := hookEvent{
		Event:       *event,
		CWD:         *cwd,
		SessionID:   *sessionID,
		Label:       strings.TrimSpace(*label),
		TimestampMS: time.Now().UnixMilli(),
	}

	data, err := json.Marshal(payload)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: marshal failed: %v\n", err)
		os.Exit(0)
	}

	conn, err := net.Dial("unix", sockPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: connect failed: %v\n", err)
		os.Exit(0)
	}
	defer conn.Close()

	conn.Write(data)
}

func readStdinJSON() map[string]any {
	info, err := os.Stdin.Stat()
	if err != nil || info.Mode()&os.ModeCharDevice != 0 {
		return nil
	}
	data, err := io.ReadAll(io.LimitReader(os.Stdin, 8192))
	if err != nil || len(strings.TrimSpace(string(data))) == 0 {
		return nil
	}

	var fields map[string]any
	if json.Unmarshal(data, &fields) != nil {
		return nil
	}
	return fields
}

func stringField(fields map[string]any, key string) string {
	if fields == nil {
		return ""
	}
	if value, ok := fields[key].(string); ok {
		return value
	}
	return ""
}

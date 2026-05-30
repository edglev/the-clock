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

func runHook(args []string) error {
	fs := flag.NewFlagSet("hook", flag.ContinueOnError)
	event := fs.String("event", "", "Event name (PreToolUse, Notification, Stop, SessionStart, SessionEnd)")
	cwd := fs.String("cwd", "", "Worktree path; defaults to hook cwd or hook payload cwd")
	sessionID := fs.String("session-id", "", "Optional agent session id")
	label := fs.String("label", "", "Optional display label")
	provider := fs.String("provider", "", "Coding CLI provider (claude or codex); defaults to claude")
	model := fs.String("model", "", "Optional model label")
	if err := fs.Parse(args); err != nil {
		return err
	}

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
	if *provider == "" {
		*provider = stringField(stdinFields, "provider")
	}
	if *model == "" {
		*model = stringField(stdinFields, "model")
	}
	if *cwd == "" {
		var err error
		*cwd, err = os.Getwd()
		if err != nil {
			*cwd = "unknown"
		}
	}

	if *event == "" {
		return fmt.Errorf("hook: --event is required")
	}

	payload := hookEvent{
		Event:       *event,
		CWD:         *cwd,
		SessionID:   *sessionID,
		Label:       strings.TrimSpace(*label),
		Provider:    normalizeProvider(*provider),
		Model:       strings.TrimSpace(*model),
		ToolName:    stringField(stdinFields, "tool_name"),
		TurnID:      stringField(stdinFields, "turn_id"),
		TimestampMS: time.Now().UnixMilli(),
	}

	data, err := json.Marshal(payload)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: marshal failed: %v\n", err)
		return nil
	}

	conn, err := net.Dial("unix", sockPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: connect failed: %v\n", err)
		return nil
	}
	defer conn.Close()

	conn.Write(data)
	return nil
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

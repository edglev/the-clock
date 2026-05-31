package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
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
	effort := fs.String("effort", "", "Optional reasoning effort label")
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
	normalizedProvider := normalizeProvider(*provider)
	if *model == "" {
		*model = stringField(stdinFields, "model")
	}
	if *model == "" {
		*model = firstNestedStringField(stdinFields,
			[]string{"payload", "model"},
			[]string{"collaboration_mode", "settings", "model"},
			[]string{"payload", "collaboration_mode", "settings", "model"},
		)
	}
	if *model == "" && normalizedProvider == "Codex" {
		*model = codexConfigString("model")
	}
	if *effort == "" {
		*effort = firstStringField(stdinFields, "effort", "reasoning_effort", "thinking_level", "reasoning_level")
	}
	if *effort == "" {
		*effort = firstNestedStringField(stdinFields,
			[]string{"payload", "effort"},
			[]string{"payload", "reasoning_effort"},
			[]string{"settings", "reasoning_effort"},
			[]string{"collaboration_mode", "settings", "reasoning_effort"},
			[]string{"payload", "collaboration_mode", "settings", "reasoning_effort"},
		)
	}
	if *effort == "" && normalizedProvider == "Codex" {
		*effort = codexConfigString("model_reasoning_effort", "plan_mode_reasoning_effort")
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

	process := discoverAgentProcess(*provider)
	payload := hookEvent{
		Event:       *event,
		CWD:         *cwd,
		SessionID:   *sessionID,
		Label:       strings.TrimSpace(*label),
		Provider:    normalizedProvider,
		Model:       strings.TrimSpace(*model),
		Effort:      strings.TrimSpace(*effort),
		ToolName:    stringField(stdinFields, "tool_name"),
		TurnID:      stringField(stdinFields, "turn_id"),
		Process:     process,
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

func firstStringField(fields map[string]any, keys ...string) string {
	for _, key := range keys {
		if value := stringField(fields, key); strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}

func nestedStringField(fields map[string]any, path ...string) string {
	var value any = fields
	for _, key := range path {
		object, ok := value.(map[string]any)
		if !ok {
			return ""
		}
		value = object[key]
	}
	text, ok := value.(string)
	if !ok {
		return ""
	}
	return strings.TrimSpace(text)
}

func firstNestedStringField(fields map[string]any, paths ...[]string) string {
	for _, path := range paths {
		if value := nestedStringField(fields, path...); value != "" {
			return value
		}
	}
	return ""
}

func codexConfigString(keys ...string) string {
	path := os.Getenv("CODEX_HOME")
	if strings.TrimSpace(path) == "" {
		home, err := os.UserHomeDir()
		if err != nil {
			return ""
		}
		path = filepath.Join(home, ".codex")
	}

	data, err := os.ReadFile(filepath.Join(path, "config.toml"))
	if err != nil {
		return ""
	}
	return rootTomlString(data, keys...)
}

func rootTomlString(data []byte, keys ...string) string {
	wanted := map[string]bool{}
	for _, key := range keys {
		wanted[key] = true
	}

	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") {
			return ""
		}

		key, value, ok := strings.Cut(line, "=")
		if !ok || !wanted[strings.TrimSpace(key)] {
			continue
		}
		return parseTomlString(value)
	}
	return ""
}

func parseTomlString(value string) string {
	value = strings.TrimSpace(value)
	if strings.HasPrefix(value, "\"") {
		end := strings.LastIndex(value[1:], "\"")
		if end >= 0 {
			return value[1 : end+1]
		}
	}
	if strings.HasPrefix(value, "'") {
		end := strings.LastIndex(value[1:], "'")
		if end >= 0 {
			return value[1 : end+1]
		}
	}
	if before, _, ok := strings.Cut(value, "#"); ok {
		value = before
	}
	return strings.TrimSpace(value)
}

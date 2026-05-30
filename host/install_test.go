package main

import (
	"strings"
	"testing"
)

func TestInstallHookSpecsAddsProviderCommands(t *testing.T) {
	data := map[string]any{}

	installHookSpecs(data, "codex", "/tmp/agent-viewer", codexHookSpecs)

	hooks := data["hooks"].(map[string]any)
	groups := hooks["PreToolUse"].([]any)
	if len(groups) != 1 {
		t.Fatalf("PreToolUse groups = %d, want 1", len(groups))
	}

	group := groups[0].(map[string]any)
	if group["matcher"] != "*" {
		t.Fatalf("matcher = %v, want *", group["matcher"])
	}

	command := group["hooks"].([]any)[0].(map[string]any)["command"].(string)
	if !strings.Contains(command, "/tmp/agent-viewer hook --provider codex --event PreToolUse") {
		t.Fatalf("command = %q", command)
	}
}

func TestInstallHookSpecsReplacesOnlyMatchingProviderHooks(t *testing.T) {
	data := map[string]any{
		"hooks": map[string]any{
			"Stop": []any{
				map[string]any{
					"hooks": []any{
						map[string]any{
							"type":    "command",
							"command": "/old/agent-viewer hook --provider codex --event Stop",
						},
						map[string]any{
							"type":    "command",
							"command": "/custom/hook",
						},
					},
				},
				map[string]any{
					"hooks": []any{
						map[string]any{
							"type":    "command",
							"command": "/old/agent-viewer hook --provider claude --event Stop",
						},
					},
				},
			},
		},
	}

	installHookSpecs(data, "codex", "/new/agent-viewer", []hookSpec{{Event: "Stop"}})

	groups := data["hooks"].(map[string]any)["Stop"].([]any)
	commands := collectHookCommands(groups)

	for _, command := range commands {
		if strings.Contains(command, "/old/agent-viewer hook --provider codex") {
			t.Fatalf("old Codex command was not removed: %v", commands)
		}
	}
	if !containsCommand(commands, "/custom/hook") {
		t.Fatalf("custom hook was removed: %v", commands)
	}
	if !containsCommand(commands, "/old/agent-viewer hook --provider claude --event Stop") {
		t.Fatalf("Claude hook was removed while installing Codex hooks: %v", commands)
	}
	if !containsCommand(commands, "/new/agent-viewer hook --provider codex --event Stop") {
		t.Fatalf("new Codex hook missing: %v", commands)
	}
}

func collectHookCommands(groups []any) []string {
	var commands []string
	for _, groupValue := range groups {
		group, ok := groupValue.(map[string]any)
		if !ok {
			continue
		}
		hooks, ok := group["hooks"].([]any)
		if !ok {
			continue
		}
		for _, hookValue := range hooks {
			hook, ok := hookValue.(map[string]any)
			if !ok {
				continue
			}
			command, _ := hook["command"].(string)
			commands = append(commands, command)
		}
	}
	return commands
}

func containsCommand(commands []string, needle string) bool {
	for _, command := range commands {
		if command == needle {
			return true
		}
	}
	return false
}

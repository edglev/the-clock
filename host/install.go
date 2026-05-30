package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type hookSpec struct {
	Event   string
	Matcher *string
}

var (
	codexHookSpecs = []hookSpec{
		{Event: "SessionStart"},
		{Event: "UserPromptSubmit"},
		{Event: "PreToolUse", Matcher: stringPtr("*")},
		{Event: "PermissionRequest", Matcher: stringPtr("*")},
		{Event: "Stop"},
	}
	claudeHookSpecs = []hookSpec{
		{Event: "Notification", Matcher: stringPtr("")},
		{Event: "PreToolUse", Matcher: stringPtr("*")},
		{Event: "Stop", Matcher: stringPtr("")},
		{Event: "SessionStart", Matcher: stringPtr("")},
		{Event: "SessionEnd", Matcher: stringPtr("")},
	}
)

func runInstallHooks(provider string, args []string) error {
	fs := flag.NewFlagSet("install-"+provider+"-hooks", flag.ContinueOnError)
	configPath := fs.String("config", defaultHookConfigPath(provider), "Path to hooks/settings JSON")
	binPath := fs.String("bin", defaultBinaryPath(), "Path to agent-viewer binary")
	dryRun := fs.Bool("dry-run", false, "Print merged JSON without writing it")
	if err := fs.Parse(args); err != nil {
		return err
	}

	provider = strings.ToLower(provider)
	specs, err := hookSpecsForProvider(provider)
	if err != nil {
		return err
	}

	resolvedBin, err := filepath.Abs(*binPath)
	if err != nil {
		return fmt.Errorf("resolve binary path: %w", err)
	}
	if !*dryRun {
		if info, err := os.Stat(resolvedBin); err != nil || info.IsDir() {
			return fmt.Errorf("agent-viewer binary not found at %s; build it with: cd host && go build -o agent-viewer .", resolvedBin)
		}
	}

	path := expandHome(*configPath)
	data, err := readHookConfig(path)
	if err != nil {
		return err
	}
	installHookSpecs(data, provider, resolvedBin, specs)

	out, err := json.MarshalIndent(data, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal hooks JSON: %w", err)
	}
	out = append(out, '\n')

	if *dryRun {
		fmt.Print(string(out))
		return nil
	}

	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}
	if err := os.WriteFile(path, out, 0644); err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}

	fmt.Printf("Installed Agent Viewer %s hooks in %s\n", normalizeProvider(provider), path)
	if provider == "codex" {
		fmt.Println("Review and trust the new hooks from Codex with /hooks.")
	}
	return nil
}

func hookSpecsForProvider(provider string) ([]hookSpec, error) {
	switch provider {
	case "codex":
		return codexHookSpecs, nil
	case "claude":
		return claudeHookSpecs, nil
	default:
		return nil, fmt.Errorf("unsupported hook provider %q", provider)
	}
}

func defaultHookConfigPath(provider string) string {
	home, err := os.UserHomeDir()
	if err != nil {
		home = "~"
	}
	switch provider {
	case "codex":
		return filepath.Join(home, ".codex", "hooks.json")
	case "claude":
		return filepath.Join(home, ".claude", "settings.json")
	default:
		return filepath.Join(home, ".agent-viewer-hooks.json")
	}
}

func defaultBinaryPath() string {
	exe, err := os.Executable()
	if err != nil {
		return "agent-viewer"
	}
	if resolved, err := filepath.EvalSymlinks(exe); err == nil {
		exe = resolved
	}
	return exe
}

func readHookConfig(path string) (map[string]any, error) {
	data := map[string]any{}
	raw, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return data, nil
		}
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	if len(strings.TrimSpace(string(raw))) == 0 {
		return data, nil
	}
	if err := json.Unmarshal(raw, &data); err != nil {
		return nil, fmt.Errorf("%s is not valid JSON: %w", path, err)
	}
	return data, nil
}

func installHookSpecs(data map[string]any, provider, binPath string, specs []hookSpec) {
	hooks := objectField(data, "hooks")
	for _, spec := range specs {
		groups := arrayField(hooks, spec.Event)
		groups = removeExistingAgentViewerHooks(groups, provider)
		groups = append(groups, hookGroup(provider, binPath, spec))
		hooks[spec.Event] = groups
	}
	data["hooks"] = hooks
}

func hookGroup(provider, binPath string, spec hookSpec) map[string]any {
	group := map[string]any{
		"hooks": []any{
			map[string]any{
				"type":    "command",
				"command": fmt.Sprintf("%s hook --provider %s --event %s", shellQuote(binPath), provider, spec.Event),
			},
		},
	}
	if spec.Matcher != nil {
		group["matcher"] = *spec.Matcher
	}
	return group
}

func removeExistingAgentViewerHooks(groups []any, provider string) []any {
	cleaned := make([]any, 0, len(groups))
	for _, groupValue := range groups {
		group, ok := groupValue.(map[string]any)
		if !ok {
			cleaned = append(cleaned, groupValue)
			continue
		}

		hooks, ok := group["hooks"].([]any)
		if !ok {
			cleaned = append(cleaned, groupValue)
			continue
		}

		remaining := make([]any, 0, len(hooks))
		for _, hookValue := range hooks {
			hook, ok := hookValue.(map[string]any)
			if !ok {
				remaining = append(remaining, hookValue)
				continue
			}
			command, _ := hook["command"].(string)
			if isAgentViewerHookCommand(command, provider) {
				continue
			}
			remaining = append(remaining, hookValue)
		}

		if len(remaining) > 0 {
			nextGroup := copyObject(group)
			nextGroup["hooks"] = remaining
			cleaned = append(cleaned, nextGroup)
		}
	}
	return cleaned
}

func isAgentViewerHookCommand(command, provider string) bool {
	command = strings.ToLower(command)
	provider = strings.ToLower(provider)
	if !strings.Contains(command, "--provider "+provider) {
		return false
	}
	return strings.Contains(command, "agent-viewer")
}

func objectField(data map[string]any, key string) map[string]any {
	if value, ok := data[key].(map[string]any); ok {
		return value
	}
	next := map[string]any{}
	data[key] = next
	return next
}

func arrayField(data map[string]any, key string) []any {
	if value, ok := data[key].([]any); ok {
		return value
	}
	return nil
}

func copyObject(in map[string]any) map[string]any {
	out := make(map[string]any, len(in))
	for key, value := range in {
		out[key] = value
	}
	return out
}

func expandHome(path string) string {
	if path == "~" {
		home, err := os.UserHomeDir()
		if err == nil {
			return home
		}
	}
	if strings.HasPrefix(path, "~/") {
		home, err := os.UserHomeDir()
		if err == nil {
			return filepath.Join(home, path[2:])
		}
	}
	return path
}

func shellQuote(s string) string {
	if s == "" {
		return "''"
	}
	if !strings.ContainsAny(s, " \t\n'\"\\$`!#&;|*?()[]{}<>") {
		return s
	}
	return "'" + strings.ReplaceAll(s, "'", "'\"'\"'") + "'"
}

func stringPtr(s string) *string {
	return &s
}

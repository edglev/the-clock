package main

import "testing"

func TestNormalizeProvider(t *testing.T) {
	tests := map[string]string{
		"":             "Claude",
		"claude":       "Claude",
		"Claude Code":  "Claude",
		"codex":        "Codex",
		"OpenAI Codex": "Codex",
		"Custom":       "Custom",
	}

	for input, want := range tests {
		if got := normalizeProvider(input); got != want {
			t.Fatalf("normalizeProvider(%q) = %q, want %q", input, got, want)
		}
	}
}

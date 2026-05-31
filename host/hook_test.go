package main

import "testing"

func TestFirstNestedStringField(t *testing.T) {
	fields := map[string]any{
		"payload": map[string]any{
			"collaboration_mode": map[string]any{
				"settings": map[string]any{
					"reasoning_effort": "xhigh",
				},
			},
		},
	}

	got := firstNestedStringField(fields,
		[]string{"effort"},
		[]string{"payload", "collaboration_mode", "settings", "reasoning_effort"},
	)
	if got != "xhigh" {
		t.Fatalf("nested effort = %q", got)
	}
}

func TestRootTomlString(t *testing.T) {
	data := []byte(`
model = "gpt-5.5"
model_reasoning_effort = "xhigh"

[projects."/tmp/project"]
model_reasoning_effort = "low"
`)

	if got := rootTomlString(data, "model"); got != "gpt-5.5" {
		t.Fatalf("model = %q", got)
	}
	if got := rootTomlString(data, "model_reasoning_effort", "plan_mode_reasoning_effort"); got != "xhigh" {
		t.Fatalf("reasoning effort = %q", got)
	}
	if got := rootTomlString(data, "missing"); got != "" {
		t.Fatalf("missing key = %q", got)
	}
}

package main

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestDefaultGitLabURL(t *testing.T) {
	if defaultGitLabURL != "https://gitlab.com" {
		t.Fatalf("default GitLab URL = %q", defaultGitLabURL)
	}
}

func TestGitLabProvisioningPayloadDoesNotContainWiFi(t *testing.T) {
	data, err := json.Marshal(deviceCloudConfig{
		GitLabURL:   defaultGitLabURL,
		GitLabToken: "pat",
	})
	if err != nil {
		t.Fatal(err)
	}
	payload := string(data)
	if strings.Contains(payload, "ssid") || strings.Contains(payload, "wifi_password") ||
		strings.Contains(payload, "access_token") {
		t.Fatalf("GitLab payload contains unrelated credentials: %s", payload)
	}
	if !strings.Contains(payload, `"gitlab_pat":"pat"`) {
		t.Fatalf("GitLab PAT missing from payload: %s", payload)
	}
}

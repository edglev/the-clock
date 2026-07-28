package main

import (
	"flag"
	"fmt"
	"strings"
)

const defaultGitLabURL = "https://gitlab.com"

func runGitLabSetup(args []string) error {
	fs := flag.NewFlagSet("gitlab-setup", flag.ContinueOnError)
	baseURL := fs.String("url", defaultGitLabURL, "GitLab base URL")
	token := fs.String("token", "", "GitLab personal access token with read_api")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if strings.TrimSpace(*token) == "" {
		secret, err := promptSecret("GitLab PAT (read_api): ")
		if err != nil {
			return err
		}
		*token = secret
	}
	cfg := deviceCloudConfig{
		GitLabURL:   strings.TrimRight(strings.TrimSpace(*baseURL), "/"),
		GitLabToken: strings.TrimSpace(*token),
	}
	if cfg.GitLabURL == "" || cfg.GitLabToken == "" {
		return fmt.Errorf("GitLab URL and PAT are required")
	}
	if err := sendDeviceCloudConfig(cfg); err != nil {
		return err
	}
	fmt.Println("Sent GitLab PAT to the device over encrypted BLE")
	return nil
}

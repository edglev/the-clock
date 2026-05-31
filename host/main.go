package main

import (
	"fmt"
	"os"
)

func main() {
	if err := run(os.Args); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) < 2 {
		return usage(args[0])
	}

	switch args[1] {
	case "daemon":
		return runDaemon()
	case "hook":
		return runHook(args[2:])
	case "bambu-login":
		return runBambuLogin(args[2:])
	case "install-codex-hooks":
		return runInstallHooks("codex", args[2:])
	case "install-claude-hooks":
		return runInstallHooks("claude", args[2:])
	case "help", "-h", "--help":
		return usage(args[0])
	default:
		return fmt.Errorf("unknown command %q\n\n%s", args[1], usageText(args[0]))
	}
}

func usage(bin string) error {
	fmt.Fprint(os.Stderr, usageText(bin))
	return nil
}

func usageText(bin string) string {
	return fmt.Sprintf(`Agent Viewer host utility

Usage:
	  %s daemon
	  %s bambu-login [--region global|china] [--email EMAIL] [--device SERIAL]
	  %s hook --provider claude|codex --event EVENT
	  %s install-codex-hooks [--config PATH] [--bin PATH] [--dry-run]
	  %s install-claude-hooks [--config PATH] [--bin PATH] [--dry-run]

	`, bin, bin, bin, bin, bin)
}

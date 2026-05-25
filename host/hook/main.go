package main

import (
	"flag"
	"fmt"
	"net"
	"os"
)

const sockPath = "/tmp/agent-viewer.sock"

func main() {
	event := flag.String("event", "", "Event name (PreToolUse, Notification, Stop, SessionStart, SessionEnd)")
	flag.Parse()

	if *event == "" {
		fmt.Fprintln(os.Stderr, "error: --event is required")
		os.Exit(1)
	}

	conn, err := net.Dial("unix", sockPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: connect failed: %v\n", err)
		os.Exit(0)
	}
	defer conn.Close()

	conn.Write([]byte(*event))
}

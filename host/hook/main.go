package main

import (
	"flag"
	"fmt"
	"net"
	"os"
)

const udpAddr = "127.0.0.1:50005"

func main() {
	event := flag.String("event", "", "Event name (PreToolUse, Notification, Stop, SessionStart, SessionEnd)")
	flag.Parse()

	if *event == "" {
		fmt.Fprintln(os.Stderr, "error: --event is required")
		os.Exit(1)
	}

	addr, err := net.ResolveUDPAddr("udp", udpAddr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: resolve failed: %v\n", err)
		os.Exit(0)
	}

	conn, err := net.DialUDP("udp", nil, addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "warning: dial failed: %v\n", err)
		os.Exit(0)
	}
	defer conn.Close()

	conn.Write([]byte(*event))
}

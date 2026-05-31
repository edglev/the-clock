package main

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

var procRoot = "/proc"

type processRef struct {
	PID       int    `json:"pid,omitempty"`
	StartTime string `json:"start_time,omitempty"`
	Command   string `json:"command,omitempty"`
}

type processInfo struct {
	PID       int
	PPID      int
	State     string
	StartTime string
	Command   string
	Args      []string
}

func discoverAgentProcess(provider string) processRef {
	provider = normalizeProvider(provider)
	self, ok := readProcessInfo(os.Getpid())
	if !ok || self.PPID <= 0 {
		return processRef{}
	}

	pid := self.PPID
	for depth := 0; depth < 32 && pid > 0; depth++ {
		proc, ok := readProcessInfo(pid)
		if !ok {
			return processRef{}
		}
		if matchesAgentProcess(provider, proc) {
			return processRef{
				PID:       proc.PID,
				StartTime: proc.StartTime,
				Command:   proc.Command,
			}
		}
		pid = proc.PPID
	}
	return processRef{}
}

func processAlive(ref processRef) bool {
	if ref.PID <= 0 || strings.TrimSpace(ref.StartTime) == "" {
		return true
	}
	proc, ok := readProcessInfo(ref.PID)
	if !ok {
		return false
	}
	if proc.StartTime != ref.StartTime {
		return false
	}
	return proc.State != "Z" && proc.State != "X"
}

func readProcessInfo(pid int) (processInfo, bool) {
	stat, err := os.ReadFile(filepath.Join(procRoot, strconv.Itoa(pid), "stat"))
	if err != nil {
		return processInfo{}, false
	}
	proc, ok := parseProcStat(string(stat))
	if !ok {
		return processInfo{}, false
	}
	proc.PID = pid

	if comm, err := os.ReadFile(filepath.Join(procRoot, strconv.Itoa(pid), "comm")); err == nil {
		proc.Command = strings.TrimSpace(string(comm))
	}
	if rawArgs, err := os.ReadFile(filepath.Join(procRoot, strconv.Itoa(pid), "cmdline")); err == nil {
		proc.Args = splitCmdline(rawArgs)
	}
	if len(proc.Args) == 0 {
		proc.Args = []string{proc.Command}
	}
	return proc, true
}

func parseProcStat(stat string) (processInfo, bool) {
	stat = strings.TrimSpace(stat)
	closeIdx := strings.LastIndex(stat, ")")
	openIdx := strings.Index(stat, "(")
	if openIdx < 0 || closeIdx <= openIdx || closeIdx+2 >= len(stat) {
		return processInfo{}, false
	}

	pid, err := strconv.Atoi(strings.TrimSpace(stat[:openIdx]))
	if err != nil {
		return processInfo{}, false
	}

	fields := strings.Fields(stat[closeIdx+1:])
	if len(fields) < 20 {
		return processInfo{}, false
	}
	ppid, err := strconv.Atoi(fields[1])
	if err != nil {
		return processInfo{}, false
	}

	return processInfo{
		PID:       pid,
		PPID:      ppid,
		State:     fields[0],
		Command:   stat[openIdx+1 : closeIdx],
		StartTime: fields[19],
	}, true
}

func splitCmdline(raw []byte) []string {
	parts := strings.Split(strings.TrimRight(string(raw), "\x00"), "\x00")
	args := parts[:0]
	for _, part := range parts {
		if part != "" {
			args = append(args, part)
		}
	}
	return args
}

func matchesAgentProcess(provider string, proc processInfo) bool {
	command := strings.ToLower(proc.Command)
	exe := commandBase(proc.Args)

	if command == "agent-viewer" || exe == "agent-viewer" {
		return false
	}

	switch normalizeProvider(provider) {
	case "Codex":
		if command == "codex" || exe == "codex" {
			return true
		}
		return command == "node" && argsContain(proc.Args, "codex")
	case "Claude":
		if command == "claude" || exe == "claude" {
			return true
		}
		return command == "node" && argsContain(proc.Args, "claude-code")
	default:
		return false
	}
}

func commandBase(args []string) string {
	if len(args) == 0 {
		return ""
	}
	return strings.ToLower(filepath.Base(args[0]))
}

func argsContain(args []string, needle string) bool {
	needle = strings.ToLower(needle)
	for _, arg := range args {
		if strings.Contains(strings.ToLower(arg), needle) {
			return true
		}
	}
	return false
}

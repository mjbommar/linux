// SPDX-License-Identifier: Apache-2.0
//
// syzkaller backend for User-Mode Linux fork-server pools.
//
// All real work lives in the in-tree Rust binary `umlctl`
// (tools/uml/uml-launcher).  This file is glue: shell out to
// `umlctl pool take`, `umlctl exec`, `umlctl port-forward`, and
// `umlctl pool destroy --name`; parse the JSON envelopes; stream
// stdout/stderr/console frames through OutputMerger.
//
// Reference implementation that is intended to be dropped into
// upstream syzkaller at vm/uml/uml.go with the blank import added
// to vm/vm.go. See README.md in this directory.
//
// This file deliberately depends only on github.com/google/syzkaller
// packages - there are no direct dependencies on the Rust binary
// beyond the wire shapes documented below.

package uml

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/google/syzkaller/pkg/report"
	"github.com/google/syzkaller/vm/vmimpl"
)

// Config is the per-pool config block, JSON-decoded from Env.Config.
type Config struct {
	Pool       string `json:"pool"`        // "default"
	UmlctlBin  string `json:"umlctl"`      // "/usr/local/bin/umlctl"
	RuntimeDir string `json:"runtime_dir"` // optional umlctl --runtime-dir
	AutoServe bool   `json:"auto_serve"`  // start `pool serve` if absent
	Kernel     string `json:"kernel"`      // path to fork-capable vmlinux
	Count      int    `json:"count"`       // matches syzkaller's vm count
	MemMB      int    `json:"mem_mb"`      // matches umlctl --mem
	TapPrefix  string `json:"tap_prefix"`  // "tap-uml-"
	Subnet     string `json:"subnet"`      // "10.7.0.0/24"
	HostfsMap  string `json:"hostfs_map"`  // guest path the host root mounts at; default "/host"
}

type pool struct {
	env *vmimpl.Env
	cfg *Config
}

type instance struct {
	pool        *pool
	index       int
	pid         int
	instance    string
	workdir     string
	mac, tap    string
	ipv4, gw    string
	consoleLog  string
	forwardPort int
	closed      bool
	mu          sync.Mutex
}

func init() {
	vmimpl.Register("uml", vmimpl.Type{
		Ctor:        ctor,
		Overcommit:  true,
		Preemptible: true,
	})
}

func ctor(env *vmimpl.Env) (vmimpl.Pool, error) {
	cfg := &Config{
		Pool:      "default",
		UmlctlBin: "umlctl",
		TapPrefix: "tap-uml-",
		Subnet:    "10.7.0.0/24",
		HostfsMap: "/host",
	}
	if len(env.Config) > 0 {
		if err := json.Unmarshal(env.Config, cfg); err != nil {
			return nil, fmt.Errorf("uml: parse config: %w", err)
		}
	}
	if cfg.Count <= 0 {
		cfg.Count = 1
	}
	if cfg.AutoServe {
		// Best-effort; if a daemon is already listening, this exits
		// cleanly (the daemon's bind-stale-socket path swallows it).
		args := []string{"pool", "serve", "--name", cfg.Pool, "--background"}
		if cfg.Kernel != "" {
			args = append(args, "--kernel", cfg.Kernel)
		}
		if cfg.MemMB > 0 {
			args = append(args, "--mem", fmt.Sprintf("%dM", cfg.MemMB))
		}
		_ = exec.Command(cfg.UmlctlBin, umlctlArgs(cfg, args...)...).Run()
	}
	return &pool{env: env, cfg: cfg}, nil
}

func (p *pool) Count() int { return p.cfg.Count }

func umlctlArgs(cfg *Config, args ...string) []string {
	if cfg.RuntimeDir == "" {
		return args
	}
	out := []string{"--runtime-dir", cfg.RuntimeDir}
	return append(out, args...)
}

// allocIPv4Pair returns the per-instance CIDR + gateway.  Strategy:
// take the /24 from cfg.Subnet, give index+10 to the guest, .1 to the
// host gateway.  Trivial; bigger subnets / non-/24 are operator-config.
func (p *pool) allocIPv4Pair(index int) (cidr, gateway string) {
	subnet := p.cfg.Subnet
	if subnet == "" {
		subnet = "10.7.0.0/24"
	}
	_, ipnet, err := net.ParseCIDR(subnet)
	if err != nil {
		return "10.7.0.42/24", "10.7.0.1"
	}
	base := ipnet.IP.To4()
	if base == nil {
		return "10.7.0.42/24", "10.7.0.1"
	}
	ones, _ := ipnet.Mask.Size()
	guestIP := make(net.IP, 4)
	copy(guestIP, base)
	guestIP[3] = byte(10 + (index & 0xff))
	gwIP := make(net.IP, 4)
	copy(gwIP, base)
	gwIP[3] = 1
	return fmt.Sprintf("%s/%d", guestIP.String(), ones), gwIP.String()
}

func allocMAC(index int) string {
	return fmt.Sprintf("52:54:00:00:%02x:%02x", (index>>8)&0xff, index&0xff)
}

func (p *pool) Create(ctx context.Context, workdir string, index int) (vmimpl.Instance, error) {
	name := fmt.Sprintf("uml-%d", index)
	tap := p.cfg.TapPrefix + name
	cidr, gw := p.allocIPv4Pair(index)
	mac := allocMAC(index)

	cmd := exec.CommandContext(ctx, p.cfg.UmlctlBin,
		umlctlArgs(p.cfg,
			"pool", "take", "--name", p.cfg.Pool, "--json",
			"--instance", name, "--mac", mac, "--tap", tap,
			"--ipv4", cidr, "--gateway", gw)...)
	out, err := cmd.Output()
	if err != nil {
		return nil, &vmimpl.BootError{
			Title:  "umlctl pool take failed",
			Output: out,
		}
	}
	var sr struct {
		Pid          int    `json:"pid"`
		Instance     string `json:"instance"`
		Mac          string `json:"mac"`
		Tap          string `json:"tap"`
		Ipv4Cidr     string `json:"ipv4_cidr"`
		Ipv4Gateway  string `json:"ipv4_gateway"`
		MconsolePath string `json:"mconsole_path"`
	}
	if err := json.Unmarshal(out, &sr); err != nil {
		return nil, fmt.Errorf("uml: parse take reply: %w (raw=%s)", err, string(out))
	}
	return &instance{
		pool:       p,
		index:      index,
		pid:        sr.Pid,
		instance:   sr.Instance,
		workdir:    workdir,
		mac:        sr.Mac,
		tap:        sr.Tap,
		ipv4:       sr.Ipv4Cidr,
		gw:         sr.Ipv4Gateway,
		consoleLog: filepath.Join(workdir, name+".console"),
	}, nil
}

// Copy: UML's hostfs root is mapped at p.cfg.HostfsMap (default
// "/host") inside the guest, so the guest already sees the host file
// at <hostfs>/<abs(hostSrc)>.  No scp/copy needed.
func (inst *instance) Copy(hostSrc string) (string, error) {
	abs, err := filepath.Abs(hostSrc)
	if err != nil {
		return "", err
	}
	hostfs := inst.pool.cfg.HostfsMap
	if hostfs == "" {
		hostfs = "/host"
	}
	return filepath.Join(hostfs, abs), nil
}

// Forward: in TAP-direct mode the host IS the gateway IP, so the
// guest can dial it directly.  We shell out to `umlctl port-forward`
// to get the canonical address string the daemon would advertise.
func (inst *instance) Forward(port int) (string, error) {
	inst.forwardPort = port
	out, err := exec.Command(inst.pool.cfg.UmlctlBin, umlctlArgs(inst.pool.cfg,
		"port-forward", "--name", inst.pool.cfg.Pool,
		"--pid", fmt.Sprint(inst.pid),
		"--host-port", fmt.Sprint(port), "--json")...).Output()
	if err != nil {
		// Fallback: synthesize the address from the gateway we
		// already know.  This is the same logic the verb runs.
		host := strings.TrimSuffix(strings.SplitN(inst.gw, "/", 2)[0], "/")
		return fmt.Sprintf("%s:%d", host, port), nil
	}
	var r struct {
		GuestAddress string `json:"guest_address"`
	}
	if err := json.Unmarshal(out, &r); err != nil {
		host := strings.TrimSuffix(strings.SplitN(inst.gw, "/", 2)[0], "/")
		return fmt.Sprintf("%s:%d", host, port), nil
	}
	return r.GuestAddress, nil
}

// Run executes `command` inside the pool member.  We shell out to
// `umlctl exec --json` and decode the NDJSON frame stream into the
// shape vmimpl wants: a chan of typed Chunks plus a terminating
// chan error.
func (inst *instance) Run(ctx context.Context, timeout time.Duration,
	stop <-chan bool, command string) (<-chan []byte, <-chan error, error) {
	args := []string{
		"exec",
		"--name", inst.pool.cfg.Pool,
		"--pid", fmt.Sprint(inst.pid),
		"--json",
	}
	if timeout > 0 {
		args = append(args, "--timeout", fmt.Sprint(int(timeout.Seconds())))
	}
	args = append(args, "--", "/bin/sh", "-c", command)

	cmd := exec.CommandContext(ctx, inst.pool.cfg.UmlctlBin, umlctlArgs(inst.pool.cfg, args...)...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, nil, err
	}

	outCh := make(chan []byte, 64)
	errCh := make(chan error, 1)
	go func() {
		defer close(outCh)
		dec := json.NewDecoder(stdout)
		var lastExit int
		var sawExit bool
		for {
			var frame struct {
				Type     string `json:"type"`
				Data     string `json:"data"`
				Code     int    `json:"code"`
				Signal   int    `json:"signal"`
				TimedOut bool   `json:"timed_out"`
			}
			if err := dec.Decode(&frame); err != nil {
				if err == io.EOF {
					break
				}
				errCh <- fmt.Errorf("uml: decode exec frame: %w", err)
				_ = cmd.Wait()
				return
			}
			switch frame.Type {
			case "stdout", "stderr", "console":
				select {
				case outCh <- []byte(frame.Data):
				case <-ctx.Done():
					errCh <- ctx.Err()
					_ = cmd.Wait()
					return
				}
			case "exit":
				lastExit = frame.Code
				sawExit = true
				if frame.TimedOut {
					errCh <- fmt.Errorf("uml: in-guest command timed out")
					_ = cmd.Wait()
					return
				}
			}
		}
		_ = cmd.Wait()
		if sawExit && lastExit != 0 {
			errCh <- fmt.Errorf("uml: in-guest exit %d", lastExit)
			return
		}
		errCh <- nil
	}()

	// Honor the syzkaller-side stop channel.  vmimpl backends are
	// expected to kill the in-guest command if stop fires before
	// completion.
	go func() {
		select {
		case <-stop:
			_ = cmd.Process.Kill()
		case <-ctx.Done():
		}
	}()

	return outCh, errCh, nil
}

func (inst *instance) Diagnose(rep *report.Report) ([]byte, bool) {
	data, _ := exec.Command(inst.pool.cfg.UmlctlBin, umlctlArgs(inst.pool.cfg,
		"pool", "status", "--name", inst.pool.cfg.Pool, "--json")...).Output()
	return data, false
}

func (inst *instance) Info() ([]byte, error) {
	return exec.Command(inst.pool.cfg.UmlctlBin, umlctlArgs(inst.pool.cfg,
		"pool", "status", "--name", inst.pool.cfg.Pool, "--json")...).Output()
}

func (inst *instance) Close() error {
	inst.mu.Lock()
	defer inst.mu.Unlock()
	if inst.closed {
		return nil
	}
	inst.closed = true
	return exec.Command(inst.pool.cfg.UmlctlBin, umlctlArgs(inst.pool.cfg,
		"pool", "destroy",
		"--name", inst.pool.cfg.Pool,
		fmt.Sprint(inst.pid))...).Run()
}

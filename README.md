# Multi-Container Runtime

A lightweight Linux container runtime written in C, featuring a long-running supervisor daemon, kernel-space memory monitoring via a Linux Kernel Module (LKM), a concurrent bounded-buffer logging pipeline, and a full CLI interface for managing container lifecycles.

---

## Table of Contents

- [Project Overview](#project-overview)
- [Architecture](#architecture)
- [How Everything Works Together](#how-everything-works-together)
- [Component Deep Dive](#component-deep-dive)
- [Requirements](#requirements)
- [Build Instructions](#build-instructions)
- [Running the Runtime](#running-the-runtime)
- [CLI Reference](#cli-reference)
- [Memory Limits and Kernel Monitor](#memory-limits-and-kernel-monitor)
- [Logging Pipeline](#logging-pipeline)
- [Scheduling Experiments](#scheduling-experiments)
- [Cleanup and Teardown](#cleanup-and-teardown)
- [Engineering Analysis](#engineering-analysis)

---

## Project Overview

This project implements a minimal but functional container runtime on Linux — similar in spirit to what Docker does internally, but stripped down to its essential OS mechanisms. The goal is to understand and exercise core operating system concepts: process isolation via namespaces, inter-process communication via pipes and UNIX sockets, concurrent producer-consumer logging, kernel module development, and Linux scheduling.

**The runtime can:**
- Launch multiple isolated containers concurrently, each with their own PID, UTS, and mount namespaces
- Enforce per-container soft and hard memory limits from kernel space
- Capture container stdout/stderr through a concurrent logging pipeline
- Accept CLI commands from any terminal while the supervisor runs in another
- Gracefully stop containers or force-kill them if they ignore termination signals
- Track per-container metadata (state, PID, start time, memory limits, exit status)

---

## Architecture

The system has two integrated layers:

```
┌─────────────────────────────────────────────────────────┐
│                     User Space                          │
│                                                         │
│   CLI Client          Supervisor Daemon                 │
│  (engine start)  ──►  (engine supervisor)               │
│  (engine ps)     ◄──  UNIX Domain Socket                │
│  (engine stop)        /tmp/mini_runtime.sock            │
│                            │                            │
│                    ┌───────┴──────────┐                 │
│                    │  Container Mgmt  │                 │
│                    │  clone() + ns    │                 │
│                    │  chroot + /proc  │                 │
│                    └───────┬──────────┘                 │
│                            │ pipes                      │
│                    ┌───────▼──────────┐                 │
│                    │ Bounded Buffer   │                 │
│                    │ Producer Threads │                 │
│                    │ Logger Thread    │                 │
│                    │ → logs/*.log     │                 │
│                    └──────────────────┘                 │
│                            │ ioctl                      │
├────────────────────────────┼────────────────────────────┤
│                    Kernel Space        │                 │
│                                        ▼                │
│                    monitor.ko → /dev/container_monitor  │
│                    Linked list of tracked PIDs          │
│                    Periodic RSS checks (1s timer)       │
│                    Soft warn / Hard kill enforcement    │
└─────────────────────────────────────────────────────────┘
```

The runtime is a **single binary** (`engine`) used in two distinct modes:

1. **Supervisor mode** — a long-running daemon that owns all containers, the logging pipeline, and the control socket
2. **Client mode** — short-lived invocations (`start`, `stop`, `ps`, etc.) that connect to the running supervisor, send a command, and exit

---

## How Everything Works Together

### Container Launch Flow

When you run `engine start alpha ./rootfs-alpha /bin/sh`:

1. The CLI client connects to `/tmp/mini_runtime.sock` and sends a `control_request_t` struct
2. The supervisor receives it and calls `launch_container()`
3. `clone()` is called with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD` — this creates a child process with isolated namespaces
4. Inside the child (`child_fn`): stdout/stderr are redirected to the write end of a pipe, `chroot()` sets the container's root to its assigned rootfs, `/proc` is mounted, and `execv("/bin/sh", ...)` runs the command
5. Back in the supervisor: a **producer thread** is spawned to drain the pipe's read end into the bounded buffer; the container record is registered with the kernel monitor via `ioctl`; a response is sent back to the CLI client

### IPC — Two Separate Paths

**Path A (Logging):** Each container's stdout/stderr flows through a pipe into a producer thread. The producer pushes `log_item_t` chunks into a shared bounded buffer. A single consumer (logger thread) pops items and appends them to per-container log files under `logs/`.

**Path B (Control):** CLI clients connect over a UNIX domain socket. The supervisor reads a `control_request_t`, dispatches to the appropriate handler (`handle_start`, `handle_stop`, etc.), and writes back a `control_response_t`. For `ps`, a larger `ps_response_t` is used to avoid truncation.

### Container Exit Flow

When a container exits, `SIGCHLD` is delivered to the supervisor. The `sigchld_handler` calls `waitpid()` to reap the child, updates container state (`exited`, `stopped`, or `killed`), unregisters the PID from the kernel monitor, detaches the producer thread (so the OS reclaims it), and notifies any `run` clients waiting for that container's exit status.

---

## Component Deep Dive

### `engine.c` — Supervisor and CLI

| Subsystem | Key Implementation |
|---|---|
| Container launch | `clone()` with PID/UTS/mount namespace flags |
| Filesystem isolation | `chroot(cfg->rootfs)` + `mount("proc", "/proc", ...)` |
| Control IPC | `AF_UNIX SOCK_STREAM` socket at `/tmp/mini_runtime.sock` |
| Metadata | `container_record_t` linked list, protected by `metadata_lock` mutex |
| SIGCHLD reaping | Signal handler with `waitpid(-1, WNOHANG)` loop — no zombies |
| Stop escalation | SIGTERM → 5s grace → SIGKILL via background thread |
| `run` command | Client blocks on socket read until supervisor writes final response on container exit |
| Duplicate rootfs check | `realpath()` comparison before launch |
| Log paths | Absolute paths derived from `getcwd()` at supervisor start |

### `monitor.c` — Kernel Module

| Subsystem | Key Implementation |
|---|---|
| Device | Character device at `/dev/container_monitor` |
| Tracked entries | `monitored_entry` structs in a `LIST_HEAD` linked list |
| Concurrency | `DEFINE_MUTEX` — mutex chosen over spinlock because `get_task_mm()`/`mmput()` can sleep |
| Periodic check | `timer_list` fires every 1 second |
| RSS measurement | `get_task_mm()` → `get_mm_rss()` × `PAGE_SIZE` |
| Soft limit | Logs `KERN_WARNING` once per crossing; resets when RSS drops back below |
| Hard limit | Sends `SIGKILL` via `send_sig()`, removes entry |
| Stale entries | `get_rss_bytes()` returns -1 for dead PIDs → entry freed |
| Registration | `MONITOR_REGISTER` ioctl from supervisor after `clone()` |
| Unregistration | `MONITOR_UNREGISTER` ioctl on container exit |
| Module unload | `del_timer_sync()` + full list teardown in `monitor_exit()` |

### Bounded Buffer — `bounded_buffer_t`

A classic producer-consumer ring buffer with capacity 16, protected by a mutex and two condition variables (`not_empty`, `not_full`).

- **Producers** (one per container): read pipe, push `log_item_t` into buffer; block when full
- **Consumer** (one logger thread): pop items, open log file by path, append, close
- **Shutdown**: `bounded_buffer_begin_shutdown()` broadcasts on both condvars so blocked threads wake and drain; consumer exits when buffer is empty and `shutting_down` is set
- **No dropped logs**: producer blocks (never discards) while buffer is full; shutdown sequence waits for full drain before joining the logger thread

---

## Requirements

- **OS:** Ubuntu 22.04 or 24.04 in a VM (QEMU/KVM recommended)
- **Secure Boot:** Must be OFF (kernel module loading requires unsigned module support)
- **No WSL**

Install dependencies inside the Ubuntu VM:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) gcc-12
```

> **Note:** `gcc-12` is required because the kernel headers on Ubuntu 22.04/24.04 with kernel 6.x were built with gcc-12. Without it, `make` will fail when building `monitor.ko`.

---

## Build Instructions

```bash
cd boilerplate
make
```

This produces:
- `engine` — the runtime binary (supervisor + CLI)
- `monitor.ko` — the kernel module
- `memory_hog` — memory pressure workload (statically linked)
- `cpu_hog` — CPU-bound workload (statically linked)
- `io_pulse` — I/O-bound workload (statically linked)

To build only user-space targets (no kernel headers needed, safe for CI):

```bash
make ci
```

To clean all build artifacts:

```bash
make clean
```

### Environment Preflight Check

Before first use, verify your VM is configured correctly:

```bash
chmod +x environment-check.sh
sudo ./environment-check.sh
```

This checks Ubuntu version, WSL absence, VM detection, Secure Boot state, kernel headers, and does a test `insmod`/`rmmod` of `monitor.ko`.

---

## Running the Runtime

### Step 1: Prepare the Alpine Root Filesystem

The containers use Alpine Linux as their root filesystem. Download and extract it once, then copy per container:

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# One writable copy per container — never share rootfs between running containers
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```


<img width="1896" height="422" alt="1_alpine" src="https://github.com/user-attachments/assets/70cdf5d1-b8e8-45f5-9ebb-04c62ac67b0a" />


> Do not commit `rootfs-base/` or `rootfs-*/` to the repository — they are listed in `.gitignore`.

To run workload binaries inside a container, copy them into the rootfs before launch:

```bash
cp ./memory_hog ./rootfs-alpha/
cp ./cpu_hog    ./rootfs-alpha/
cp ./io_pulse   ./rootfs-alpha/
```

<img width="1480" height="394" alt="2_copy_to_rootfs" src="https://github.com/user-attachments/assets/06f247ea-db4e-45ce-bad0-6b218573af02" />


### Step 2: Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

<img width="1326" height="589" alt="3_load_and_verify" src="https://github.com/user-attachments/assets/9c5abcf5-fc01-408c-9809-92f518f42dbe" />


Expected output: `crw------- 1 root root <major>, 0 ... /dev/container_monitor`

Check kernel logs to confirm:

```bash
dmesg | grep container_monitor
# [container_monitor] Module loaded. Device: /dev/container_monitor
```

<img width="1286" height="629" alt="4_check_kernel_logs" src="https://github.com/user-attachments/assets/646479a6-540a-4666-810b-1e461170d72f" />


### Step 3: Start the Supervisor (Terminal 1)

```bash
sudo ./engine supervisor ./rootfs-base
```

<img width="1358" height="603" alt="5_start_hypervisor" src="https://github.com/user-attachments/assets/3d50a07c-42e2-4ff1-bd65-3b9d568372b6" />


The supervisor runs in the foreground and prints:

```
[supervisor] Ready. rootfs=./rootfs-base socket=/tmp/mini_runtime.sock logs=<cwd>/logs
```

It creates a `logs/` directory in the current working directory where per-container log files are stored.

### Step 4: Use the CLI (Terminal 2)

All CLI commands connect to the running supervisor over the UNIX socket.

```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# List all tracked containers
sudo ./engine ps

# View a container's log output
sudo ./engine logs alpha

# Stop a container gracefully (SIGTERM → SIGKILL after 5s)
sudo ./engine stop alpha
```

<img width="988" height="504" alt="5_start_container" src="https://github.com/user-attachments/assets/878e19ea-c74e-4d32-bd52-deb51a7582ac" />

---


<img width="1741" height="301" alt="6_list_all_tracked_containers" src="https://github.com/user-attachments/assets/b39b7645-b55c-4c42-9547-167430bfb3da" />

---

<img width="1275" height="199" alt="7_container_log" src="https://github.com/user-attachments/assets/b6120a8c-589e-483b-b5e9-5f4981dfcf09" />

---

<img width="1012" height="147" alt="8_container_stop" src="https://github.com/user-attachments/assets/9745bdc0-a1d4-439c-ad0c-06bffcf7fbee" />

---

## CLI Reference

```
engine supervisor <base-rootfs>
    Start the supervisor daemon. Stays running until SIGINT/SIGTERM.

engine start <id> <container-rootfs> <command> [options]
    Launch a container in the background. Returns after supervisor records metadata.

engine run <id> <container-rootfs> <command> [options]
    Launch a container and block until it exits. Returns the container's exit code
    (or 128 + signal number if killed by a signal).
    SIGINT/SIGTERM to the run client forwards a stop to the supervisor and
    continues waiting for the final exit status.

engine ps
    List all tracked containers with: ID, host PID, state, start time,
    soft/hard memory limits, and log file path.

engine logs <id>
    Print the full log file for the specified container.

engine stop <id>
    Send SIGTERM to the container. If it does not exit within 5 seconds,
    send SIGKILL automatically.
```

**Optional flags for `start` and `run`:**

| Flag | Default | Description |
|---|---|---|
| `--soft-mib N` | 40 | Soft memory limit in MiB. Kernel logs a warning when exceeded. |
| `--hard-mib N` | 64 | Hard memory limit in MiB. Kernel kills the container when exceeded. |
| `--nice N` | 0 | Nice value for the container process (-20 to 19). Lower = higher priority. |

**Container states:**

| State | Meaning |
|---|---|
| `running` | Container is alive |
| `stopped` | Exited after a `stop` command was issued |
| `killed` | Killed by SIGKILL (hard limit or forced kill) |
| `exited` | Exited on its own |

---

## Memory Limits and Kernel Monitor

The kernel module tracks each container's Resident Set Size (RSS) and enforces two-tier memory limits.

**Soft limit:** When a container's RSS first crosses the soft limit, a warning is written to the kernel log. This is non-destructive — the container continues running. The warning resets if RSS drops back below the soft limit, so future crossings are also reported.

**Hard limit:** When RSS meets or exceeds the hard limit, the kernel module sends `SIGKILL` directly to the container process and removes it from the tracked list.

To observe memory events in real time:

```bash
# Watch kernel log for monitor events
sudo dmesg -w | grep container_monitor
```

Example output:
```
[container_monitor] Registering container=alpha pid=1234 soft=50331648 hard=83886080
[container_monitor] SOFT LIMIT container=alpha pid=1234 rss=52428800 limit=50331648
[container_monitor] HARD LIMIT container=alpha pid=1234 rss=85196800 limit=83886080
```

The supervisor distinguishes termination causes in `ps` output:
- `stopped` — `stop` command was issued (`stop_requested` flag set before the signal)
- `killed` — SIGKILL received without a prior stop request (hard limit kill)
- `exited` — clean voluntary exit

**To test memory enforcement:**

```bash
cp ./memory_hog ./rootfs-alpha/
sudo ./engine start alpha ./rootfs-alpha /memory_hog --soft-mib 20 --hard-mib 32
sudo dmesg -w | grep container_monitor
```

`memory_hog` allocates 8 MiB per second and touches each page, so it will hit the soft limit in ~3 seconds and the hard limit shortly after.

---

## Logging Pipeline

Each container's stdout and stderr are captured through a pipe into the supervisor's logging pipeline. Log files are written to `logs/<container-id>.log` relative to the directory where the supervisor was started (absolute path is used internally to survive `chdir`).

**To follow a container's output live:**

```bash
tail -f logs/alpha.log
```

**To read the full log:**

```bash
sudo ./engine logs alpha
```

The pipeline continues capturing until the container exits and the producer thread drains the pipe. On supervisor shutdown, the logger thread is joined only after the bounded buffer is fully drained, so no log entries are lost.

---

## Scheduling Experiments

The runtime supports nice-value based priority experiments using the `--nice` flag and the included workload binaries.

### CPU-bound experiment: priority comparison

```bash
cp ./cpu_hog ./rootfs-alpha/
cp ./cpu_hog ./rootfs-beta/

# High priority container (nice -10)
sudo ./engine start hiprio ./rootfs-alpha "/cpu_hog 30" --nice -10

# Low priority container (nice 10)
sudo ./engine start loprio ./rootfs-beta  "/cpu_hog 30" --nice 10

# Compare log timestamps to observe CPU time distribution
tail -f logs/hiprio.log &
tail -f logs/loprio.log &
```

### CPU-bound vs I/O-bound

```bash
cp ./cpu_hog  ./rootfs-alpha/
cp ./io_pulse ./rootfs-beta/

sudo ./engine start cpu_work ./rootfs-alpha "/cpu_hog 20"
sudo ./engine start io_work  ./rootfs-beta  "/io_pulse 20 100"

# cpu_hog burns CPU; io_pulse interleaves writes and sleeps
# Observe that the I/O-bound container remains responsive even
# when the CPU-bound container is monopolizing the CPU
```

**Workload reference:**

| Binary | Behavior | Arguments |
|---|---|---|
| `cpu_hog` | Busy-loop computation | `[seconds]` (default 10) |
| `io_pulse` | Write bursts + sleep | `[iterations] [sleep_ms]` |
| `memory_hog` | Allocates and touches memory | `[chunk_mb] [sleep_ms]` |

---

## Cleanup and Teardown

**Stop all containers and shut down the supervisor:**

```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Then Ctrl+C the supervisor terminal (or send SIGTERM)
```

On SIGINT/SIGTERM, the supervisor:
1. Sends SIGTERM to all running containers
2. Waits up to 5 seconds
3. Sends SIGKILL to any still-running containers
4. Drains the bounded buffer and joins the logger thread
5. Joins or detaches all producer threads
6. Frees all container records and closes all file descriptors
7. Removes `/tmp/mini_runtime.sock`

**Unload the kernel module:**

```bash
sudo rmmod monitor
dmesg | tail -5
# [container_monitor] Module unloaded.
```

`monitor_exit()` calls `del_timer_sync()` to ensure the timer callback is not running, then frees all remaining `monitored_entry` nodes.

**Verify no zombies:**

```bash
ps aux | grep -E 'Z|defunct'
```

**Clean build artifacts:**

```bash
make clean
```

---

## Engineering Analysis

### 1. Isolation Mechanisms

Each container is created with `clone()` using three namespace flags:

- `CLONE_NEWPID` — the container gets its own PID namespace. The first process inside is PID 1 from its perspective; the host sees it as a normal PID. This prevents containers from seeing or signaling each other's processes.
- `CLONE_NEWUTS` — the container gets its own hostname. `sethostname()` inside `child_fn` sets it to the container ID without affecting the host.
- `CLONE_NEWNS` — the container gets its own mount namespace. Mounts inside the container (specifically `/proc`) are invisible to the host and other containers.

`chroot()` then restricts the container's filesystem view to its assigned rootfs directory. The host kernel is still shared — the same kernel services all containers; only the namespace wrappers around PIDs, mounts, and UTS differ.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are child processes, and on Linux only the parent (or `init`) can `waitpid()` for them. Without a persistent parent, exited containers become zombies. The supervisor installs a `SIGCHLD` handler with `SA_NOCLDSTOP` that calls `waitpid(-1, WNOHANG)` in a loop — this reaps all exited children immediately without blocking the main loop.

### 3. IPC, Threads, and Synchronization

**Pipes (Path A):** Each container gets a `pipe()`. The write end is `dup2()`'d to the container's stdout/stderr; the supervisor holds the read end in a producer thread. This is unidirectional, buffered by the kernel, and naturally signals EOF when the container exits (write end closes).

**UNIX socket (Path B):** The control channel uses `AF_UNIX SOCK_STREAM`. It supports bidirectional, framed communication suitable for request-response semantics. A FIFO would work for one direction only; shared memory would require more complex framing.

**Bounded buffer:** Protected by a `pthread_mutex_t` + two `pthread_cond_t` (not_full, not_empty). A spinlock would waste CPU spinning while the buffer is full or empty. A mutex correctly blocks the thread. Without the mutex, two producer threads could write to the same tail slot simultaneously; without the condvars, threads would spin-check the count.

**Metadata list:** Protected by a separate `metadata_lock` mutex from the bounded buffer, so log I/O does not block container launch or `ps` queries.

### 4. Memory Management and Enforcement

RSS (Resident Set Size) measures pages currently in physical RAM, not virtual address space. A process can `malloc()` a large region but its RSS only grows as pages are actually touched. RSS does not count shared library pages attributed to other processes, nor does it count swap.

Soft and hard limits serve different policies: soft is advisory (warn the operator without disrupting the workload); hard is mandatory (kill before the system OOMs). Enforcement belongs in the kernel because a user-space poller can be preempted or killed itself; the kernel timer fires reliably regardless of user-space state, and `send_sig()` from kernel context cannot be intercepted by the target process.

### 5. Scheduling Behavior

Linux uses CFS (Completely Fair Scheduler) with nice values mapping to weight factors. A process with nice -10 gets roughly 9× the CPU share of a process with nice +10 when both are runnable. I/O-bound processes voluntarily sleep (blocking on `write()`/`fsync()`/`usleep()`), releasing the CPU and accumulating "vruntime credit" that lets them preempt CPU-bound tasks when they wake. This is why `io_pulse` remains responsive even when `cpu_hog` is at the same nice value — the scheduler rewards processes that voluntarily yield.


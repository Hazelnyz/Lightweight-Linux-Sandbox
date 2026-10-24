# minibox

A mini container runtime written in C, using Linux namespaces, cgroups v2,
pivot_root and a BusyBox root filesystem.

Project title: Lightweight Linux Sandbox with Resource Isolation & Monitoring
(A Mini Container Runtime Using Linux Namespaces and Cgroups)

## How it works

```
 sudo ./minibox run [options] <cmd>
            |
            v
 +----------------------+    pipe (sync)     +--------------------------+
 |   PARENT (minibox)   | -----------------> |   CHILD (becomes PID 1)  |
 |                      |  "cgroup is ready" |                          |
 | 1. create cgroup     |                    | 1. wait on the pipe      |
 | 2. set limits        |                    | 2. mount proc/sys/tmp/dev|
 | 3. add child to it   |                    | 3. pivot_root to rootfs  |
 | 4. poll every 100 ms |                    | 4. exec the command      |
 | 5. write CSV+report  |                    +--------------------------+
 +----------------------+                    new PID, UTS, mount, IPC
            |                                and network namespaces
            v
 /sys/fs/cgroup/minibox/<name>/
   memory.max   pids.max   cpu.max   (limits)
   memory.current   pids.current   cpu.stat   (monitoring)
```

- **Namespaces** give the container its own view: its own PID numbers,
  hostname, mounts, IPC and network.
- **Cgroups** limit how much memory, CPU and how many processes it can use.
- **pivot_root** swaps the root filesystem to `rootfs/`, so the container
  cannot see the host files.
- The child waits on a pipe so it never runs before the parent has set up
  the cgroup limits.

## Files

- `minibox.c` - the runtime
- `stress.c` - test program (modes: cpu, mem <MB>, fork <N>)
- `rootfs/` - BusyBox root filesystem
- `scripts/demo.sh` - runs six limited vs unlimited experiments
- `tools/make_report.py` - builds `results/dashboard.html` from the results
- `Makefile` - build, test, demo, report, clean

## Build

```
make build
```

This compiles `minibox` and the static `stress` program, and copies
`stress` into `rootfs/bin/`.

## Usage

```
sudo ./minibox run [options] <cmd>
```

| Option | Meaning |
|---|---|
| `--mem 64M` | memory limit |
| `--cpus 0.5` | CPU limit (0.5 = half a CPU) |
| `--pids 20` | maximum number of processes |
| `--weight N` | CPU weight |
| `--io "MAJ:MIN wbps=N"` | disk write speed limit |
| `--hostname h` | hostname inside the container |
| `--name n` | cgroup name and output file name |
| `--userns` | user namespace (experimental) |
| `--monitor` | print live usage lines every second |

Example:

```
sudo ./minibox run --hostname box1 --name t1 /bin/sh
```

## Run the demo

```
make demo     # six runs, limited vs unlimited, saved in results/
make report   # builds results/dashboard.html
```

Each run also writes `minibox-<name>.csv` with the columns
`t_ms, mem_bytes, pids, cpu_usec`.

The report printed after each run shows: exit reason, wall time, CPU time
used, peak memory, OOM kills and fork denied (pids.max hit). The OOM kills
number counts kill events from `memory.events`, not separate processes.

## Notes for WSL2

- Needs cgroups v2 (check with `stat -fc %T /sys/fs/cgroup`, it should print
  `cgroup2fs`).
- Must be run with `sudo`.
- Mounting `/sys` inside the container may only print a warning on WSL2.
  The run still works.
- `--userns` was tested on WSL2 (see the next section).

## User namespace mode (--userns)

With `--userns`, root inside the container is an unprivileged user on the
host. By default container ids 0..65535 map to host ids 100000..165535.
Change this with `--uid-base` and `--uid-count`. A base of 0 is refused,
because it would map container root to host root.

The rootfs must be owned by the mapped id, and every folder above it must
be enterable by other users. A rootfs inside a private home folder will not
work, so use a copy outside it:

```
sudo mkdir -p /var/lib/minibox
sudo cp -a rootfs /var/lib/minibox/rootfs-userns
sudo chown -R -h 100000:100000 /var/lib/minibox/rootfs-userns
sudo ./minibox run --userns --rootfs /var/lib/minibox/rootfs-userns /bin/sh
```

Checked on WSL2: inside the container `id` shows uid 0, PID 1, no extra
groups, and files can be written. On the host the container process runs
as uid 100000.

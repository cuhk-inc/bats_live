# bats_live

Live video transport on top of the BATS codec in `../bats`. One frame is one generation. The source sends every batch for that frame at once. A relay forwards a full batch immediately and flushes a partial batch after a few milliseconds. The destination emits the frame if it has decoded by the playout time, and drops the frame if it has not. The next frame continues either way.

The program calls `bats_encode`, `bats_recode`, and `bats_decode`. It does not change the code. It sets the send and playout schedule.

## Build

From this directory:

```bat
build.bat
```

`build.bat` uses `../bats/tools/tcc`. `make` uses gcc.

```bash
make
./bats_live --role selftest
```

## Frame format

The source reads a sequence of frames from `--input`, or from stdin when `--input` is omitted. Each frame is a 4-byte big-endian length followed by that many payload bytes. An encoder writes this stream. A decoder reads the destination stream.

The default output record adds the generation and the presentation time, both 4-byte big-endian, in front of the length:

```text
generation | pts_ms | length | payload
```

A gap in `generation` is a dropped frame. `--plain-out` writes the same records the source reads: length, then payload. `--frame-bytes N` reads fixed-size raw frames with no length prefix.

## Running

The path is source, then zero or more relays, then destination. The source names the next hop with `--to`. Each relay listens and names its next hop. The destination only listens.

```bash
bats_live --role dest --listen 9002 --delay-ms 80 --output out.bin
bats_live --role relay --listen 9001 --to 127.0.0.1:9002 --flush-ms 2
bats_live --role relay --listen 9000 --to 127.0.0.1:9001 --flush-ms 2
bats_live --role source --to 127.0.0.1:9000 --input frames.bin --fps 30 --t 1200
```

With no relay, point `--to` at the destination.

`T` is the payload size, from `--t`, default 1200. A packet is the 40-byte header plus `T` bytes, and that datagram has to fit in the path MTU. For one frame, `K` is `ceil(frame length / T)`.

`--delay-ms` is how long the destination waits after the first packet of a frame. It emits a decoded frame at that time. A frame that is still unsolved is dropped. The wait has to cover sending every batch of the frame, the flush at each relay, and one decode.

The base send size is about `K + K/2` packets plus one batch. `--overhead-pct` (default 25) adds that percentage on top of the base, which is the allowance for loss. Raise it when packets are lost. Lower it when the playout wait is too long and the path is clean.

`--flush-ms` (default 2) is how long a relay holds a batch that has not collected 8 packets. `--idle-sec` (default 0) keeps the relay and the destination running. A positive value makes them exit after that many seconds without a packet. For a file demo, set it higher than `--delay-ms`.

The source and the destination use the same `--seed` and the same degree distribution. The default distribution is `../bats/psi_line.h`. Pass the same `--psi` file on both ends to replace it. A relay does not load Ψ.

`--loss-pct` drops that percentage of packets before they are sent. It is for experiments.

## Options

| Option | Default | Role | Meaning |
| --- | --- | --- | --- |
| `--to HOST:PORT` | | source, relay | Next hop |
| `--listen PORT` | | relay, dest | UDP port |
| `--bind ADDR` | `0.0.0.0` | relay, dest | Bind address |
| `--input PATH` | stdin | source | Frame stream. `-` is stdin |
| `--output PATH` | stdout | dest | Decoded frames. `-` is stdout |
| `--fps N` | `0` | source | Pace in frames per second. `0` sends each frame as it arrives |
| `--t N` | `1200` | source | Payload bytes |
| `--overhead-pct N` | `25` | source | Extra packets on top of the base send size, in percent |
| `--flush-ms N` | `2` | relay | Flush a partial batch after this idle time |
| `--delay-ms N` | `80` | dest | Playout wait after the first packet of a frame |
| `--coalesce-ms N` | `1` | dest | Decode after the frame has been idle this long |
| `--window N` | `64` | relay, dest | Generations kept at once |
| `--max-k N` | `1024` | source | Largest `K` for one frame |
| `--frame-bytes N` | `0` | source | Fixed frame size, with no length prefix |
| `--seed N` | `0xB175` | source, dest | `code_seed` |
| `--recode-seed N` | `0xB175E1` | relay | Initial `recode_state` for this hop |
| `--flow N` | `0` | source, dest | Flow number |
| `--loss-pct N` | `0` | source, relay | Percent of packets dropped before send |
| `--idle-sec N` | `0` | relay, dest | Exit after this many idle seconds. `0` runs until stopped |
| `--psi PATH` | `psi_line.h` | source, dest | Degree distribution file |
| `--plain-out` | off | dest | Write length and payload only |
| `--quiet` | off | all | Suppress per-frame logs |

## Video coding

A drop removes the whole frame. A later frame that predicts from it stays broken until the next frame that can be decoded on its own. Use a short keyframe interval, or frames that decode independently.

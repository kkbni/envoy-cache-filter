# Envoy Ring-Buffer Cache with Request Coalescing

*Design write-up - CDN77 take-home assignment*
> **Note:** This project is based on the [Envoy Proxy](https://www.envoyproxy.io/) source code. The original upstream documentation can be found in [DOC_README.md](DOC_README.md).

## Table of Contents
- [Summary](#summary)
- [Cache Design](#cache-design)
- [From Thread-Local to Shared Cache](#from-thread-local-to-shared-cache)
- [Request Coalescing (Leader and Follower)](#request-coalescing-leader-and-follower)
- [Bugs I Hit Along the Way](#bugs-i-hit-along-the-way)
- [Watermarks and Backpressure](#watermarks-and-backpressure)
- [Limitations and Trade-offs](#limitations-and-trade-offs)
- [What I Would Do Differently in Production](#what-i-would-do-differently-in-production)
- [Post-Submission Fixes](#post-submission-fixes)
- [Building and Running](#building-and-running)
- [How I Tested It](#how-i-tested-it)

## Summary

This repo adds a custom HTTP filter to Envoy (built from source at `v1.38.3`) that caches HTTP responses in memory and coalesces concurrent requests for the same resource, so a burst of simultaneous requests for the same URL only reaches the origin server once instead of once per request.

The cache key is the request's `Host` header plus its path. Each cache entry is backed by a fixed-size ring buffer, with the size configurable through the filter's config. I built this in two passes:

1. A first version using Envoy's thread-local storage - the simplest thing that compiles, but it turns out not to actually solve the problem the assignment is about. Testing exposed this quickly, and the reason why is worth explaining.
2. A second version using a single cache shared across worker threads, protected by a mutex, with a Leader/Follower model to do the actual coalescing.

Along the way I hit two real concurrency bugs while load-testing with parallel requests. They ended up being the most useful part of the whole exercise, so they're written up in detail below.

Watermark buffers (Envoy's backpressure mechanism) aren't respected for coalesced Followers, given the scope of the MVP, but I've written up what they're for, why coalescing conflicts with them, and what I'd change per HTTP version to fix it properly.

## Cache Design

The cache lives in an HTTP filter, `source/extensions/filters/http/ring_buffer_cache`, that sits in Envoy's filter chain and looks at both the request (to compute the cache key) and the response (to populate the cache). It's registered like any other extension, included into `source/extensions/extensions_build_config.bzl` so it gets compiled into `envoy-static`.

- **Cache key:** the request's `Host` header plus its path. The assignment allows a configurable key instead, but I kept it fixed for this version. Making the set of headers folded into the key configurable (closer to how `Vary` works) would be a small addition on top of the same config, and is the first place I'd look if this needed to grow.
- **Storage:** every key gets its own fixed-size ring buffer. The size is a field in the filter's `config.proto`, set once in `envoy.yaml`. This bounds the memory used per cached object regardless of how large the real response is.
- **Number of keys:** unbounded - the cache can grow to hold as many distinct URLs as show up; only the size of each individual entry is capped. I come back to why that's a real limitation outside of a take-home in the trade-offs section below.
- **Storage medium:** RAM only. Nothing is written to disk, and the cache doesn't survive a restart.

**A note on file layout:** Envoy's built-in extensions usually keep their `.proto` config under `api/`, registered via an `extensions_metadata.yaml`, a `CODEOWNERS` entry, and a listing in `contrib_build_config.bzl`. I started down that path and backed out of it - that approach is meant for extensions going upstream into the real project, which this isn't. Everything for this filter (`config.proto`, `BUILD`, the `.h`/`.cc` files) is placed together under `source/extensions/filters/http/ring_buffer_cache`, which is simpler and doesn't pretend this is more than what it is.

## From Thread-Local to Shared Cache

My first version used Envoy's thread-local storage API:

```cpp
ThreadLocal::SlotPtr tls_slot = context.serverFactoryContext().threadLocal().allocateSlot();
```

This felt like the simplest thing that could possibly work: Envoy runs one worker thread per CPU core, and thread-local storage means every worker thread gets its own private map of ring buffers. No locking, no shared state, nothing to get wrong.

Testing it exposed the problem almost immediately. Curling the same URL twice in a row was a cache hit, waiting even a couple of seconds between the two requests and it missed every time. The reason is that two `curl`s fired back-to-back reuse the same TCP connection, and Envoy always services a given connection on the same worker thread, so the second request lands on the same thread and sees the first request's cached copy. Once that connection times out and closes, the next request opens a new one, and Envoy's connection balancing can hand that new connection to a different worker thread, whose local cache has never seen this URL.

That's not what we were aiming for. The thundering herd scenario is many different clients, on many different connections, hitting the same URL at the same moment - exactly the kind of traffic that gets spread across every worker thread by design. A thread-local cache doesn't coalesce any of that, it just happens to look correct if you only ever test it with one client reusing one connection. Under real concurrent load, you'd still get the request hitting the origin once per thread - up to N times simultaneously, which is the exact problem I was supposed to be solving.

## Request Coalescing (Leader and Follower)

The fix was to stop using thread-local storage and move to a single cache shared by all worker threads, protected by an `absl::Mutex`, with a small state machine per key:

- **Not in map** - nothing cached, no request for this key.
- `FETCHING` - a request for this key is currently being served by the origin.
- `READY` - the full response is sitting in the ring buffer and can be served from memory.

The first request for a key becomes the **Leader** - it takes the lock to flip the entry to `FETCHING`, releases it, and is forwarded to the origin as normal. Any request that shows up while the entry is `FETCHING` becomes a **Follower**: instead of going upstream, it pauses its own filter chain (`StopIteration`) and adds itself to a waitlist for that same key.

As the Leader receives the response from the origin, it doesn't wait for the whole thing to finish before telling the Followers about it - it broadcasts each piece (headers, then each chunk of the body) to everyone on the waitlist as it arrives. That was an intentional choice to satisfy the requirement that coalesced requests keep receiving data as soon as it's available, so a Follower doesn't sit there long enough to trigger Envoy's idle timeouts.

Once the Leader finishes and the entry moves to `READY`, any new request for that key is served straight out of the ring buffer - no Leader/Follower involved at all at that point.

- **Handling Client Disconnects:** If a Follower's client drops the connection mid-wait, we need to ensure the Leader doesn't try to post data to a destroyed stream. Each Follower captures a `std::shared_ptr<bool> is_active` into its posted callback, which is set to `false` in the filter's destructor. Because a connection's callbacks run on that specific connection's worker thread, this safely prevents use-after-free errors without requiring a lock.

## Bugs I Hit Along the Way

These are the bugs I spent the most time on, and honestly the most interesting part of the assignment. Most of them only showed up once I started testing with real concurrent load instead of one request at a time.

### The Mid-Stream Race Condition

The first time I load-tested this with four parallel `curl`s:

```bash
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
wait
```

it deadlocked. Three of the four background jobs never printed `Done`.

The dummy origin (Python's `http.server`) responds fast enough that the Leader can receive and broadcast the response headers before any Follower has even reached the waitlist. So by the time Followers 2-4 actually do join, the headers broadcast has already reached an empty waitlist, they only catch the body broadcast that comes right after. Envoy's internal HTTP state machine treats a Follower receiving body data before headers as a protocol violation, and freezes the stream to avoid corrupting it. That's exactly the deadlock I was seeing.

The fix: when a Follower registers, it immediately checks what the Leader has already produced for that key. If headers (or partial body) are already sitting there, the Follower gets sent those right away instead of only waiting for the next broadcast. That way the order is always headers then body, no matter how late a Follower joins relative to the Leader.

### The Buffer-Consumption Bug

While fixing the race condition I found a second, unrelated bug: I was passing the cached buffer straight into `encodeData()`. Envoy's buffers are designed to be drained by that call, so the first reader of a cached entry would empty it out corrupting the cache for the next request that tried to read it. The fix was to copy the buffer's contents before handing them to `encodeData()`, so reading a cache entry never changes it.

### The Unbounded Cache Oversight
My initial implementation successfully read the `ring_buffer_size` from the Envoy config, but I mistakenly backed the storage with a standard Envoy `Buffer::OwnedImpl` that just grew infinitely. It was an unbounded cache, meaning a request for a massive file would eventually exhaust the proxy's memory.

The fix was to replace the Envoy buffer with a custom `RingBuffer` class using a fixed-capacity `std::vector<char>` and modulo arithmetic. If a response exceeds the configured capacity, the buffer wraps around and overwrites the oldest bytes. To prevent future clients from receiving a corrupted, partially overwritten file, the entry is flagged as `overflowed`. The active Followers still receive the live broadcast safely in memory, but the entry is never marked `READY`, ensuring subsequent requests fall back to the origin rather than reading broken data.

## Watermarks and Backpressure

**What watermark buffers are for.** They're Envoy's flow-control mechanism - the thing that stops Envoy from running out of memory when data arrives faster than it can be sent back out. Imagine proxying a large file from a fast origin to a client on a slow connection: if Envoy just kept reading from the origin as fast as it could, it would keep buffering more and more of the file in RAM while the slow client catches up, until it runs out of memory. A high watermark on the internal buffer (e.g. 1 MB) tells Envoy to stop reading from the origin once the buffer fills up to that point and a low watermark (e.g. 500 KB) tells it to resume once the buffer has drained back down below that. Pause the fast side, resume it once there's room - that's the whole mechanism.

**Why coalescing complicates this.** Normal backpressure is 1-to-1: one client, one buffer, pause or resume the one origin fetch that's feeding it. Coalescing turns this into 1-to-N: one Leader fetch feeding N Followers who can each be reading at very different speeds. Say 99 Followers are on fast connections and 1 is on a slow one: respecting that slow Follower's watermark would mean pausing the Leader's fetch from the origin, which pauses data for the 99 fast Followers too, just because of one slow client. Ignoring the watermark, which is what this implementation currently does, avoids punishing the fast Followers, but means Envoy keeps buffering data in RAM for the slow Follower without stopping - exactly the memory blowup watermarks exist to prevent. Neither option is actually correct on its own, but here's how I'd fix it for each protocol:

**HTTP/1.1** maps one request to one TCP connection, and flow control only exists at the connection level - you can't pause data to one Follower without pausing the whole connection it's on. So every Follower's buffer occupancy needs to be tracked individually, using Envoy's watermark callbacks. When a specific Follower crosses its high watermark, it gets pulled out of the in-memory broadcast group. Instead of continuing to hold its data in RAM, it starts reading from a temporary file on disk that the Leader is also writing to in the background. Fast Followers keep reading straight from RAM, the slow one falls back to disk at its own pace, and the Leader's fetch from the origin is never paused.

**HTTP/2 and HTTP/3** both multiplex many requests over a single connection and do flow control per-stream, so Envoy has fine-grained control compared to HTTP/1.1. You'd still track each Follower's buffer occupancy individually, but instead of the disk fallback, we could reset just that one stream once it crosses its high watermark - that Follower gets a 503 (Service Unavailable) and its buffer is freed immediately, while the Leader and every other Follower on the same multiplexed connection carry on.

## Limitations and Trade-offs

**What this solves:**
- Repeated concurrent requests for the same URL under load hit the origin once, not once per request - the actual thundering herd scenario.
- Per-object memory is bounded by the configurable ring buffer size, regardless of the real response size.
- Followers get data streamed to them live as the Leader receives it, so they don't sit idle long enough to trigger Envoy's idle timers

**What it doesn't handle, and why that matters outside of a take-home:**
- **One global mutex.** Every worker thread contends for the same lock to touch the cache map. Fine at low-to-moderate traffic. At real CDN request rates with many distinct keys, this lock is the first thing that would show up in a profile.
- **Single-process cache.** This only coalesces requests landing on one Envoy process. A real deployment runs many Envoy instances - a miss on one node has no idea a neighboring node already fetched the same object a moment earlier.
- **No cache expiration.** There are no time limits on cached items, and the filter doesn't read origin instructions like the `Vary` header. Once something is cached under a Host+Path key, it stays until it's overwritten in the ring buffer.
- **No cap on the total number of keys.** The assignment explicitly allows this, but an unbounded number of ring buffers means unbounded total memory in a real deployment.
- **Watermarks aren't respected for Followers**, as covered [above](#watermarks-and-backpressure).
- **Only tested against a single trivial local origin.** No real load testing at concurrency levels anywhere close to what a CDN edge node would actually see.

## What I Would Do Differently in Production

1. **Get rid of the single global mutex.** The simplest fix is lock striping: split the cache map into N partitions (e.g. by `hash(key) % N`), each with its own mutex, so that unrelated keys stop contending with each other. The more thorough fix (and the one that fits how Envoy is built) is to stop using a mutex at all: designate one thread as the only owner of the cache map, and have worker threads talk to it by posting messages (`dispatcher.post()`) instead of taking a lock. The owner thread processes lookups one at a time with no contention and posts results back to the worker that asked. Envoy's whole architecture is built around not blocking worker threads, so this is the more "native" direction - the mutex version is just much easier to get right first, which is why I started there.
2. **Implement proper cache expiration and validation.** Right now anything under a given Host+Path is cached unconditionally and stays until overwritten. A production cache needs to respect origin instructions (like the `Vary` header), expire files based on TTL, and provide a way to manually clear out files.
3. **Think about cross-node behavior.** A CDN edge is usually more than one Envoy instance. To cache effectively across a whole cluster, the nodes either need to talk to a separate, shared caching database (which adds a network delay), or the load balancer needs to be smart enough to always route requests for the same URL to the exact same server node. I haven't implemented either of these. It's the most obvious next problem once a single node works correctly.
4. **Make the cache observable.** Right now the only way I confirmed this was working was watching `curl` output and the dummy origin's own request log. In production, I'd expose hit/miss counters and current memory usage through Envoy's stats system (the same admin interface I used to check `http.ingress_http.downstream_rq_completed` while testing) so cache behavior is visible without a manual test every time.
5. **Handle watermarks properly**, with each protocol-specific approach [above](#watermarks-and-backpressure).

## Post-Submission Fixes

While preparing to talk through this project, I re-read my own filter more critically and found two things worth fixing. I'm calling them out explicitly rather than folding them in quietly, since being upfront about what a second pass caught seemed more useful than pretending the first version was already complete.

- **`ring_buffer_size: 0` crashed the proxy.** `config.proto` documents the field as `>= 1`, but nothing enforced that. Proto3 leaves an unset `uint32` at `0`, and that value went straight into `RingBuffer`'s capacity with no check. Since `RingBuffer::write()` does `write_pos_ % capacity_`, a capacity of `0` is a division by zero on the very first byte written to the cache - a crash. Fixed by rejecting `ring_buffer_size == 0` in `createFilterFactoryFromProto`, so a bad config fails to load with an error instead of crashing a worker thread on the first real request.
- **The demo `envoy.yaml` never actually tested a `Ready` state cache hit.** It shipped with `ring_buffer_size: 10` (10 bytes) against `/WORKSPACE`, which is 888 bytes. Every write overflowed the buffer immediately, so the entry never reached `Ready`. I bumped it to `65536` (64 KiB), comfortably larger than the demo target, so [How I Tested It](#how-i-tested-it) now actually exercises the `Ready` path.

## Building and Running

Tested on Ubuntu under WSL2. Base build dependencies:

```bash
sudo apt update
sudo apt install -y build-essential clang lld git curl unzip zip python3 python3-pip
```

Bazel comes from [Bazelisk](https://github.com/bazelbuild/bazelisk) (downloaded and placed on `$PATH` as `bazel`), which reads Envoy's pinned Bazel version and fetches it automatically.

A couple of extra packages that aren't obvious until you actually hit the errors for them:

```bash
# some newer Ubuntu releases no longer ship this in the default repos
wget http://archive.ubuntu.com/ubuntu/pool/main/n/ncurses/libtinfo5_6.1-1ubuntu1.18.04.1_amd64.deb
sudo apt install ./libtinfo5_6.1-1ubuntu1.18.04.1_amd64.deb

# needed once the hermetic Clang toolchain below is forced on
sudo apt-get install -y autoconf libtool patch virtualenv
```

By default, Bazel can silently fall back to the host system's linker instead of Envoy's own bundled Clang/lld toolchain, which on a host with a newer glibc produces a confusing `undefined reference` error deep inside a third-party dependency build (`libevent`, via `rules_foreign_cc`). Forcing the hermetic toolchain explicitly avoids it:

```bash
echo "build --config=clang" >> user.bazelrc
```

I sanity-checked a plain Envoy build against the bundled demo config before writing any filter code, just to confirm the toolchain worked at all:

```bash
bazel build //source/exe:envoy-static
$(bazel info bazel-genfiles)/source/exe/envoy-static --config-path configs/envoy-demo.yaml
```

This is a genuinely long build. I capped Bazel at 6 jobs (`build --jobs=6` in `~/.bazelrc`) to keep the rest of the machine usable, and the first build still took around 8 hours.

*(Optional: `sudo ln -s $(realpath $(bazel info bazel-genfiles)/source/exe/envoy-static) /usr/local/bin/envoy` puts the built binary on `$PATH` so you don't need the full `bazel-bin` path every time.)*

Once the filter itself is in place and wired into `source/extensions/extensions_build_config.bzl`, rebuild the same target and run it against the repo's `envoy.yaml`, which has the cache filter configured on a route:

```bash
bazel build //source/exe:envoy-static
./bazel-bin/source/exe/envoy-static -c envoy.yaml
```

The admin interface is on `http://localhost:9901`: `http.ingress_http.downstream_rq_completed`, visible under `/stats`, is a quick way to confirm requests are actually flowing through the proxy.

If you also try to build or index everything under `/contrib` (for example via a full IDE project sync), you may separately hit a `qatlib` build failure unrelated to this filter - fixed with `sudo apt-get install -y autoconf automake libtool autoconf-archive nasm libnuma-dev pkg-config`.

## How I Tested It

With Envoy running against `envoy.yaml` (cache filter enabled, pointed at a local dummy origin):

```bash
python3 -m http.server 8080 &
./bazel-bin/source/exe/envoy-static -c envoy.yaml
```

**Basic caching:**

```bash
curl -v http://localhost:10000/WORKSPACE
```

**Coalescing under concurrent load** - fire several requests for the same URL at once:

```bash
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
curl -s http://localhost:10000/WORKSPACE > /dev/null & \
wait
```

All four should complete. To actually confirm coalescing happened, rather than four independent requests that all just happened to be fast, watch the dummy origin's own terminal: `http.server` logs one line per request it actually receives, so you should see a single `GET /WORKSPACE` logged even though four `curl`s fired.

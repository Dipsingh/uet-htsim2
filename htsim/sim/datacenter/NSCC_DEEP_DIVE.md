# NSCC Deep Technical Dive

A comprehensive analysis of the UET Network-aware Sender Congestion Control (NSCC) algorithm, with comparisons to TCP Cubic and DCQCN.

Every formula in this document is **discovered, not stated**. We start with what we need, try the simplest thing, see what goes wrong, and fix it — so each equation feels inevitable.

> **Simulation Results Available**: This document includes figures from actual htsim simulations validating the algorithm concepts. See [§11 Simulation Validation](#11-simulation-validation) for the complete results and [How to Reproduce](#how-to-reproduce) for instructions.

> **Scope & Sources**
>
> This document draws on three distinct layers — and it's important to know which is which:
>
> | Layer | What it covers | Source |
> |---|---|---|
> | **UET NSCC (spec-level)** | Four-case decision logic, signal philosophy, Quick Adapt intent | [UET paper (arXiv:2508.08906)](https://arxiv.org/html/2508.08906v1) |
> | **SMaRTT / SMaRTT-REPS (research)** | Specific control-law formulas, WTD mechanism, REPS load balancing, parameter scaling | [SMaRTT-REPS paper (arXiv:2404.01630)](https://ar5iv.labs.arxiv.org/html/2404.01630v3) |
> | **This simulator (implementation)** | Exact parameter values, batching/fulfill mechanics, EWMA filter cases, code-level details | `uec.cpp` in this repository |
>
> Where the implementation diverges from the published descriptions, we call it out explicitly. Most of the "discovered equations" narrative follows the implementation, with notes on how the spec or research papers frame the same concepts differently.

> **References**
>
> | Tag | Full Citation |
> |-----|---------------|
> | **[UET]** | Ultra Ethernet Transport Protocol, arXiv:2508.08906 — [html](https://arxiv.org/html/2508.08906v1) |
> | **[SMaRTT-REPS]** | SMaRTT-REPS: Sender-based Marked Rapidly-adapting Trimmed & Timed Transport with Recycling Entropies for Path Selection, arXiv:2404.01630 — [html](https://ar5iv.labs.arxiv.org/html/2404.01630v3) |
> | **[DCTCP]** | Data Center TCP (DCTCP), RFC 8257, IETF, 2017 |
> | **[TCP Cubic]** | CUBIC for Fast and Long-Distance Networks, RFC 9438, IETF, 2024 |
> | **[DCQCN]** | Y. Zhu et al., "Congestion Control for Large-Scale RDMA Deployments," ACM SIGCOMM 2015 |
> | **[TIMELY]** | R. Mittal et al., "TIMELY: RTT-based Congestion Control for the Datacenter," ACM SIGCOMM 2015 |
> | **[Swift]** | G. Kumar et al., "Swift: Delay is Simple and Effective for Congestion Control in the Datacenter," ACM SIGCOMM 2020 |

---

## 1. Introduction & Mental Model

> **Learning Goals**
>
> After reading this document you should be able to:
>
> 1. **Explain** why two orthogonal signals (delay + ECN) are necessary in a multi-path spraying fabric, and how they produce four distinct congestion quadrants.
> 2. **Derive** the proportional increase and decrease formulas from first principles — constant increase overshoots, quadratic is too timid, linear headroom is "just right."
> 3. **Trace** a single ACK through the full NSCC pipeline: delay extraction → quadrant classification → window update → multipath feedback.
> 4. **Compare** NSCC with TCP Cubic, DCQCN, DCTCP, and Swift using the *Knob + Signal + Assumptions* framework.
> 5. **Predict** NSCC's behavior in canonical scenarios (incast, ECMP collision, ramp-up) and identify which subsystem (QA, NOOP, MD) dominates.

### The Spraying Problem

Before we look at any formulas, let's understand the *one problem* that makes NSCC necessary. Traditional congestion control assumes a single path between sender and receiver:

```
Single-path:  Sender ──────────────── Receiver
              (one signal = one truth)
```

Every packet sees the same queue, the same delay, and the same congestion. If delay goes up, that *is* the network state. Simple.

But modern datacenter fabrics spray packets across many equal-cost paths:

```
Multi-path:   Sender ─── Path A (empty) ─── Receiver
                    ├── Path B (congested) ──┤
                    └── Path C (empty) ──────┘
              (three signals = conflicting truths)
```

Now ask yourself: **if Path B is congested but A and C are fine, should you reduce your sending rate?**

If you *do* reduce, you're punishing the flow for congestion on one out of three paths — wasting 2/3 of available capacity. If you *don't* reduce, you're ignoring real congestion. Neither answer is obviously right.

This is the fundamental tension NSCC resolves. Its answer: **use two signals, not one.** Delay tells you *how much* congestion there is on average. ECN tells you *whether a specific path* is congested. The combination lets you distinguish "one path is hot" from "the whole network is overloaded."

### Where NSCC Fits

NSCC is a **delay + ECN hybrid** that uses two orthogonal signals to classify network state into four quadrants, each triggering a distinct response. This gives it proportional control: it doesn't just "increase or decrease" but modulates *how aggressively* it does each.

**The Bandwidth-Delay Product (BDP).** The fundamental physical quantity governing any window-based CC is:

```
BDP = B × R₀
```

where **B** is the bottleneck bandwidth (bytes/s) and **R₀** is the base round-trip time (the propagation delay with zero queuing). BDP tells you how many bytes can be "in flight" before the first ACK returns. NSCC's actuator is a **window of outstanding bytes** — the congestion window (`cwnd`) — and the algorithm's job is to keep `cwnd` near `BDP`:

| Regime | Meaning | Consequence |
|--------|---------|-------------|
| `W ≈ BDP` | Pipe is full, queues are empty | Ideal operating point |
| `W > BDP` | Excess bytes queue at switches | Delay rises, ECN marks appear |
| `W < BDP` | Pipe is under-utilized | Wasted capacity |

For a concrete example: at 400 Gbps with R₀ = 12 µs, `BDP = 50 GB/s × 12 µs = 600 KB`. That's roughly 150 MTU-sized packets worth of bytes that must be in flight to keep the pipe full. Every formula in this document — increase, decrease, Quick Adapt threshold — ultimately refers back to this single number.

| | **TCP Cubic** | **DCQCN** | **NSCC** |
|---|---|---|---|
| **Signal** | Loss / ECN (binary) | ECN via CNPs | Delay magnitude + ECN |
| **Actuator** | Congestion window | Sending rate | Congestion window |
| **Target fabric** | Single-path, deep buffers | Lossless RDMA (PFC) | Multi-path spraying, shallow buffers |

---

## 2. NSCC Core Algorithm: The Four Quadrants

### Discovering the Decision Matrix

Let's design a congestion control algorithm from scratch for a spraying network. We'll start with the simplest possible approach and see where it breaks.

**Attempt 1: Use delay only.**

We measure queuing delay on every ACK. If delay is above a target, decrease the window. If below, increase. This works well on a single path — but with spraying, here's what happens:

```
  16 paths total. Path 5 is congested (delay = 20µs).
  All other paths: delay ≈ 2µs.

  Average delay across recent ACKs:
    (15 × 2µs + 1 × 20µs) / 16 = 3.1µs

  If our target is 6µs → average is BELOW target → INCREASE!

  But what if we get 3 packets from Path 5 in a row?
    Average shifts to ~8µs → ABOVE target → DECREASE!

  Result: oscillation driven by sampling noise, not real congestion.
```

The problem: delay alone mixes per-path congestion with global congestion. A single hot path can randomly push the average above or below the target depending on which ACKs arrive recently.

**Attempt 2: Add a second signal — ECN.**

ECN marks tell us something delay doesn't: *a specific switch on a specific path* decided it was congested. If we get an ECN mark, we know *that path* is congested. If we don't, the path was fine — even if the delay was high (maybe the packet just queued briefly at a non-congested switch).

Now we have two independent signals, each binary. That gives us **four combinations**.

But before we work through them, notice something crucial about *when* each signal arrives:

- **ECN is a leading indicator**: a switch marks a packet *at the moment of congestion*. If queues start building, you see ECN marks within one link-hop of the congestion point — fast, but only 1 bit of information (congested or not).
- **RTT is a trailing indicator**: you only learn the delay *after* the packet has traversed the entire path, been processed, and the ACK has returned. RTT can remain elevated even after queues have started draining and ECN marks have stopped. It's multi-bit (tells you *how much* congestion), but it lags.

This "leading vs trailing" asymmetry is the key to interpreting the four quadrants. When the two signals agree, the situation is clear. When they disagree, the disagreement itself tells you something — specifically, it tells you whether congestion is *building* or *clearing*.

> **Note: both signals become binary for the quadrant decision.** ECN is inherently binary (marked or not). RTT/delay is *continuous*, but NSCC **bins** it into low/high using the `target_Qdelay` threshold. So the four-quadrant matrix operates on two binary inputs. The continuous *magnitude* of delay re-enters later — inside `multiplicative_decrease()` (§3), where `avg_delay` determines *how much* to cut. This "binary for direction, continuous for magnitude" split is central to the design; §4 explains it in full.

**Let's work through each one:**

1. **Low delay, no ECN** — "What should we do?" The network is uncongested. Obviously: **increase**. And increase *proportionally* to how far below the target we are — the emptier the network, the more room we have.

2. **High delay, ECN** — "What should we do?" The network is congested *and* marking us. Obviously: **decrease**. And decrease proportionally to how far above the target the delay is — the worse the congestion, the harder we back off.

3. **Low delay, ECN** — "This is the interesting one." The leading indicator (ECN) says congestion is building, but the trailing indicator (RTT) hasn't caught up yet — delay is still low. In a spraying network, this most likely means *one specific path* is congested while the rest are fine. The right answer: **don't change the window** — just avoid that path next time. This is NSCC's key innovation: the NOOP quadrant.

4. **High delay, no ECN** — "What does this mean?" The trailing indicator (RTT) is elevated, but the leading indicator (ECN) has gone quiet. The UET paper interprets this as **"congestion is going down"**: queues were high recently (RTT still reflects that), but switches have stopped marking (the congestion is clearing). The right answer: **increase gently** — the situation is improving, so nudge the window up conservatively rather than aggressively.

Here's the resulting decision space:

```
        RTT (trailing)
           ↑
           │  FAIR            MULTIPLICATIVE
           │  INCREASE        DECREASE
           │  (congestion     (both signals
           │   clearing)       agree: bad)
    target ┤╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶╶
           │  PROPORTIONAL
           │  INCREASE        NOOP
           │  (both signals   (congestion
           │   agree: good)    building)
           └──────────────────────────────→ ECN (leading)
                              (skip=true)
```

We didn't *invent* four quadrants — we *discovered* them by asking what the right response is for each combination of two orthogonal signals.

### The Actual Implementation

| | **No ECN** (`skip=false`) | **ECN** (`skip=true`) |
|---|---|---|
| **Delay < Target** (RTT low) | **Proportional Increase** | **NOOP** (defer to load balancer) |
| **Delay ≥ Target** (RTT high) | **Fair Increase** (gentle) | **Multiplicative Decrease** |

Source: `uec.cpp:1310-1335` (`updateCwndOnAck_NSCC`)

The logic maps directly:

```
if quick_adapt fires → return early
if (!ecn && delay >= target) → fair_increase
if (!ecn && delay < target)  → proportional_increase
if (ecn  && delay >= target) → multiplicative_decrease
if (ecn  && delay < target)  → NOOP
```

**Important detail:** The `delay` variable in the quadrant decision is the **raw per-packet** queuing delay (`raw_rtt - base_rtt`, `uec.cpp:973`), not the filtered average. This means the quadrant is chosen based on what *this specific packet* experienced, not a smoothed history. The filtered average only appears later, inside `multiplicative_decrease()` (`uec.cpp:1243`), where it determines the *magnitude* of the cut. Section 4 explains why this split matters.

**NOOP depends on load balancing.** The NOOP quadrant only pays off if a load-balancing module uses the ECN-marked ACKs as a hint to change path entropies — for example, REPS's policy of moving to a new entropy on ECN-marked feedback, or Bitmap's penalty mechanism (§6). Without active path steering (e.g., with static ECMP), NOOP would ignore congestion that nothing else fixes. SMaRTT-REPS makes this coupling explicit by introducing a "Wait to Decrease" (WTD) mechanism that specifically defers CC decreases to give the load balancer time to reroute around ECMP collisions.

**Design choice vs spec:** The UET NSCC spec says "does not react" in this case. The SMaRTT research algorithm instead applies a gentle **Fair Decrease** (`cwnd -= (cwnd/bdp) × fd × pkt_size`) — a small proportional cut that improves fairness even when load balancing can't fully resolve the issue. This implementation follows the UET NSCC spec (pure NOOP), relying on the multipath engine to handle per-path congestion.

**Threshold terminology:** The UET spec [UET] describes the delay threshold as the "expected unloaded RTT" — i.e., the delay you'd see with zero queuing. This implementation uses a separate configurable parameter `target_Qdelay` (§4), which represents the *acceptable queuing delay* on top of base RTT. The two can differ: `target_Qdelay` may be set to 0.75 × RTT, which is not the same as the unloaded RTT itself. When reading the spec alongside the code, keep this distinction in mind — "delay above threshold" in the spec maps to `delay ≥ target_Qdelay` in the implementation, where `delay = raw_rtt − base_rtt`.

---

## 3. Window Management Deep Dive

### How the Pieces Fit Together

Before diving into individual functions, here's how a single ACK flows through the window management pipeline:

```
  ACK arrives with (delay, ecn)
        │
  ┌─────┴──────┐
  │ Quadrant    │─── selects one of: fast_inc / prop_inc / fair_inc / mult_dec / noop
  │ Decision    │    (uses raw per-packet delay)
  └─────┬──────┘
        │
  ┌─────┴──────┐
  │ Accumulate  │─── increase functions add to _inc_bytes (not to cwnd!)
  │ _inc_bytes  │    decrease applies to cwnd immediately
  └─────┬──────┘
        │
  ┌─────┴──────┐
  │ Fulfill     │─── every ~8 MTU or ~1 RTT:  cwnd += _inc_bytes / cwnd
  │ Adjustment  │    normalizes by cwnd → fairness
  └─────┬──────┘
        │
  ┌─────┴──────┐
  │ Bounds      │─── clamp to [min_cwnd, 1.5 × BDP]
  └────────────┘
```

Notice the asymmetry: **increases are accumulated and applied in batch** (for stability), but **decreases are applied immediately** (for responsiveness). This is a deliberate design choice — you want to react to congestion quickly but grow cautiously. Each piece is explained in detail below.

### BDP and Maximum Window

The bandwidth-delay product is calculated from the base RTT and NIC link speed:

```
_bdp = timeAsUs(_base_rtt) * _nic.linkspeed() / 8000000    // uec.h:184
_maxwnd = 1.5 * _bdp                                        // uec.cpp:166
```

The 1.5× headroom above BDP allows the window to absorb transient queuing without immediately hitting the ceiling.

> **Implementation note:** This simulator uses `maxwnd = 1.5 × BDP`. The SMaRTT-REPS paper uses `1.25 × BDP` ("higher than 1 to manage transient bursts"). The choice affects how much transient overshoot the system tolerates — 1.5× is more permissive, 1.25× is tighter. Both use `min_cwnd = 1 MTU`.

### Designing the Increase Function

We know we need to increase the window when delay is below the target. But *what shape* should the increase function have?

**Attempt 1: Constant increase** — add the same number of bytes regardless of delay.

```
  increase
  rate ↑
       │████████████████████  ← constant: same increase everywhere
       │
       │
       └──────────────────→ delay
       0              target

  Problem: when delay is at 90% of target, we're ALMOST congested
  but still pushing just as hard as when the network is empty.
  Result: overshoot. We blow past the target every time.
```

**Attempt 2: Quadratic decrease** — increase falls off as delay² approaches target.

```
  increase
  rate ↑
       │╲
       │ ╲
       │   ╲
       │     ╲              ← quadratic: too cautious near zero
       │       ╲╲
       │          ╲╲╲╲╲___
       └──────────────────→ delay
       0              target

  Problem: too cautious near zero delay. When the network is
  nearly empty, we're barely increasing — wastes capacity.
```

**Attempt 3: Linear ramp** — increase proportional to `(target - delay)`.

```
  increase
  rate ↑
       │╲
       │  ╲
       │    ╲          ← slope = alpha
       │      ╲
       │        ╲
       │          ╲
       └───────────┴──→ delay
       0         target

  "The closer to target, the gentler we push.
   The emptier the network, the harder we push."
```

This is the Goldilocks choice. The linear ramp gives us:
- Maximum aggressiveness when the network is empty (delay ≈ 0)
- Gentle approach as we near the target (no overshoot)
- Zero increase exactly at the target (smooth transition to fair_increase)

This is exactly what NSCC implements in its proportional increase.

### Three Tiers of Increase

NSCC has four increase mechanisms with distinct aggressiveness:

```
fast_increase >> proportional_increase > fair_increase > eta
```

**Fast Increase** (`uec.cpp:1221-1238`): Fires when delay < 1µs (essentially zero queuing). After seeing one full cwnd worth of near-zero-delay ACKs, directly adds to cwnd:

```cpp
_cwnd += newly_acked_bytes * _fi_scale;     // _fi_scale = 0.25 * scaling_factor_a
```

This is applied *immediately* to `_cwnd`, bypassing the fulfill accumulator. It's the "network is empty, fill it fast" mode.

**Proportional Increase** (`uec.cpp:1208-1219`): Accumulates into `_inc_bytes`:

```cpp
_inc_bytes += _alpha * newly_acked_bytes * (_target_Qdelay - delay);
```

Where `_alpha = 4.0 * MSS * scaling_factor_a * scaling_factor_b / _target_Qdelay` (`uec.cpp:124`). The `(target - delay)` term makes increase proportional to available headroom.

What does alpha actually control? It converts `(target - delay)` — a time quantity in picoseconds — into bytes to add. The MSS factor gives it packet-scale units. The scaling factors (`a × b`) ensure the increase rate scales with the network's BDP (see §7). The division by `target_Qdelay` normalizes the time quantity — without it, a network with a 9µs target would increase 9× more per ACK than one with a 1µs target, even if both have the same BDP. In effect, alpha answers: "for each byte ACKed and each picosecond of headroom, how many bytes should we add to the accumulator?"

**Fair Increase** (`uec.cpp:1202-1206`): The most conservative accumulation:

```cpp
_inc_bytes += _fi * newly_acked_bytes;      // _fi = 5 * MSS * scaling_factor_a
```

**Eta** (`uec.cpp:1269`): A minimum additive increase applied once per adjustment period regardless of other actions:

```cpp
_cwnd += _eta;                              // _eta = 0.15 * MSS * scaling_factor_a
```

### Designing the Decrease Function

Now for the other side: when we need to decrease, **how much should we cut?**

**Attempt 1: Fixed multiplicative decrease** (TCP-style: always cut by 30%).

The problem: if delay is barely above the target, we're overreacting. If delay is 10x the target, we're underreacting. A fixed cut ignores the *severity* of congestion.

**The NSCC approach: proportional cut.** The fraction of the window we *keep* is:

```
  kept = 1 - γ × (avg_delay - target) / avg_delay
```

Let's plot what this looks like as delay increases:

```
  window
  kept (%)
  100│         ← at target (no cut)
     │╲
   73│  ╲ ·····  d = 1.5t (cut 27%)
     │    ╲
   60│ · · ·╲···  d = 2t (cut 40%)
     │        ╲
   50│─ ─ ─ ─ ─╲─ ─ ─  FLOOR (max 50% cut)
     │           ╲___________
     └─────────────────────→ delay/target
     1    1.5   2    3    5
```

The cut depth scales with how far above the target we are. At exactly the target, no cut. At 2× the target, a 40% cut. And we never cut more than 50% — because overshooting the decrease is just as bad as undershooting.

Why divide by `d` (actual delay) instead of `t` (target)? Because `(d-t)/d` is the **fraction of delay that is excess** — it ranges from 0 (at target) to 1 (as delay → ∞). This means `γ × (d-t)/d` asymptotically approaches γ but never exceeds it. If we divided by `t` instead, the cut fraction `γ × (d-t)/t` would grow without bound — at d = 10t, it would be `0.8 × 9 = 7.2`, which is meaningless (you can't cut 720% of a window). The `/d` normalization naturally bounds the cut to [0, γ], and the `max(..., 0.5)` floor provides an additional safety margin ensuring the window never loses more than half its value in one step.

**"What if?" — Exploring the gamma parameter:**

The parameter `γ` (gamma) controls aggressiveness. Let's see what happens as it varies:

```
  At delay = 2 × target:

  γ=0.5: kept = 1 - 0.5×(2t-t)/2t = 1 - 0.25 = 75%  (gentle)
  γ=0.8: kept = 1 - 0.8×(2t-t)/2t = 1 - 0.40 = 60%  ← NSCC's choice
  γ=1.0: kept = 1 - 1.0×(2t-t)/2t = 1 - 0.50 = 50%  (aggressive, hits floor)

  At delay = 1.5 × target:

  γ=0.5: kept = 1 - 0.5×(0.5t)/1.5t = 1 - 0.17 = 83%
  γ=0.8: kept = 1 - 0.8×(0.5t)/1.5t = 1 - 0.27 = 73%  ← NSCC's choice
  γ=1.0: kept = 1 - 1.0×(0.5t)/1.5t = 1 - 0.33 = 67%
```

Why γ=0.8? It's aggressive enough to respond quickly to real congestion but leaves enough headroom below the 50% floor that moderate congestion doesn't slam into the hard limit.

### Multiplicative Decrease

```cpp
// uec.cpp:1240-1255
avg_delay = get_avg_delay();
if (avg_delay > _target_Qdelay) {
    if (now - _last_dec_time > _base_rtt) {
        _cwnd *= max(1 - _gamma * (avg_delay - _target_Qdelay) / avg_delay, 0.5);
    }
}
```

Key properties:
- **Rate-limited**: Only fires once per base RTT (`_last_dec_time` guard)
- **Proportional**: Cut depth depends on `(avg_delay - target) / avg_delay`
- **Bounded**: The `max(..., 0.5)` ensures we never cut more than 50%
- **gamma = 0.8**: The aggressiveness parameter (`uec.cpp:129`)

> **Spec vs implementation divergence:** The UET NSCC paper describes the decrease as "aggressively for each incoming packet" (per-ACK), and SMaRTT constrains per-ACK decreases to "at most the acknowledged packet's size." This implementation instead rate-limits to **once per RTT** and caps the total cut at 50%. The behavioral difference is significant: per-ACK bounded decreases can collapse cwnd extremely fast under persistent marks (very "datacenter-ish" — sharp but bounded), whereas RTT-rate-limited halving is slower and more Reno-like. Both achieve proportional response; they differ in how quickly the window can shrink during sustained congestion.

### The Fulfill Adjustment Batching Mechanism

Conceptually, NSCC/SMaRTT updates on every ACK reception. In practice, implementations may batch updates — and this turns out to work well. The SMaRTT-REPS paper observes that "reacting only once every 50 packets does not significantly impact SMaRTT performance, which is within 5% of the per-packet reaction scenario." Reaction frequency is a tuning axis, not a fixed requirement.

This implementation uses a **two-phase approach**: accumulate signals from many ACKs, then apply one normalized adjustment periodically. This avoids oscillation from noisy per-ACK signals and ensures fair convergence between flows.

#### Phase 1: Accumulation (every ACK)

When an ACK triggers an increase action (e.g., `proportional_increase` or `fair_increase`), the result is added to the `_inc_bytes` accumulator—`_cwnd` is not modified:

```cpp
// proportional_increase — uec.cpp:1217
_inc_bytes += _alpha * newly_acked_bytes * (_target_Qdelay - delay);

// fair_increase — uec.cpp:1204
_inc_bytes += _fi * newly_acked_bytes;
```

After many ACKs, `_inc_bytes` can grow to a large raw number (because `_alpha` and `_fi` have scaling factors baked in). This is intentional—the normalization happens in Phase 2.

#### Phase 2: Application (periodic batch)

The batch fires when either of two conditions is met (`uec.cpp:1341`):

- **Byte trigger**: `_received_bytes > _adjust_bytes_threshold` (default: 8 MTUs ≈ 32KB)
- **Time trigger**: `now - _last_adjust_time > _adjust_period_threshold` (default: 1 network RTT)

The dual trigger ensures both high-throughput flows (hit the byte threshold quickly) and low-throughput flows (hit the time threshold instead) get regular updates.

When the batch fires, the accumulated value is **normalized by dividing by the current cwnd**:

```cpp
// uec.cpp:1260
_cwnd += _inc_bytes / _cwnd;
```

#### Why Divide by _cwnd? — Convergence to Fairness

This division is the key to fairness. To see why, watch two flows converge to a fair share:

```
  cwnd
  (KB)
   80│  Flow A ╲                          _inc_bytes = 600,000
     │           ╲                        600,000 / 80,000 = +7.5 bytes (tiny step)
     │             ╲─────────── converge
   40│─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─  fair share
     │             ╱───────────
     │           ╱                        600,000 / 10,000 = +60 bytes (big step)
   10│  Flow B ╱                          _inc_bytes = 600,000
     └─────────────────────────→ time (RTTs)
     0    5   10   15   20

  "Same inc_bytes, but ÷ big_cwnd = small step
                   and ÷ small_cwnd = big step"
```

Consider two flows with the same `_inc_bytes = 600,000` after a batch:

| Current `_cwnd` | `_inc_bytes / _cwnd` | Relative increase |
|---|---|---|
| 10,000 bytes (small flow) | +60 bytes | +0.6% |
| 100,000 bytes (large flow) | +6 bytes | +0.006% |

A flow with a small window gets a proportionally larger bump than a flow with a large window. This is mathematically equivalent to TCP's classic AIMD increase pattern (`+MSS²/cwnd` per ACK), but applied in batch form. It ensures convergence to fairness: large flows grow slowly, small flows grow quickly, and they meet in the middle.

#### Timeline Example

```
ACK 1:  _inc_bytes += 30000     (_cwnd unchanged)
ACK 2:  _inc_bytes += 28000     (_cwnd unchanged)
ACK 3:  _inc_bytes += 31000     (_cwnd unchanged)
...
ACK 8:  _received_bytes crosses 8*MTU threshold
        → fulfill_adjustment() fires:
           _cwnd += 240000 / _cwnd    (e.g., +4 bytes if cwnd=60000)
           _cwnd += _eta              (small bonus: 0.15 * MSS * scaling)
           _inc_bytes = 0             (reset accumulator)
           _received_bytes = 0        (reset trigger)
```

NSCC listens to many ACKs, aggregates their signals, and then makes one calm, normalized adjustment every ~8 packets or ~1 RTT, whichever comes first.

### Window Bounds

```cpp
// uec.cpp:1137-1143
if (_cwnd < _min_cwnd) _cwnd = _min_cwnd;   // _min_cwnd = 1 MTU
if (_cwnd > _maxwnd)   _cwnd = _maxwnd;      // _maxwnd = 1.5 * BDP
```

#### Simulation: Fairness Convergence

The figure below shows Jain's Fairness Index and per-flow throughput distributions from actual htsim runs with 8 to 128 flows. The fulfill normalization (`inc_bytes / cwnd`) drives near-perfect fairness regardless of flow count.

![Fairness Convergence](figures/sim_fairness.png)

---

## 4. Delay Measurement & Filtering

### Designing a Trustworthy Thermometer

NSCC's decisions depend on accurate delay measurement. But in a spraying network, each ACK's delay comes from a *different path*. Some paths are congested, most aren't. How do you build a delay signal that reflects *overall* network state rather than per-path noise?

The answer: a very slow-moving EWMA (Exponentially Weighted Moving Average) filter.

### Base RTT: The Zero Point of the Thermometer

Every thermometer needs a zero point. For NSCC, that zero point is **base RTT** — the round-trip time a packet would experience if every queue in its path were completely empty. All congestion measurement derives from this reference:

```
  measured_rtt = base_rtt + queuing_delay
                 ────────   ──────────────
                 "zero"      "the signal"
```

If base RTT is wrong, *every* delay measurement is wrong, and every quadrant decision is based on a lie. Get it too high, and the algorithm underestimates congestion (too aggressive). Get it too low, and it overestimates congestion (too timid). This makes base RTT arguably the most important single value in the entire algorithm.

#### How Base RTT Is Initially Computed

Base RTT is **not a user-configured parameter** — it's computed automatically from the topology. The `calculate_rtt()` function in `main_uec.cpp:55-63` sums three physical components:

```cpp
simtime_picosec rtt = 2 * t_cfg->get_diameter_latency()                                        // (1)
    + (Packet::data_packet_size() * 8 / speedAsGbps(host_linkspeed) * t_cfg->get_diameter() * 1000)  // (2)
    + (UecBasePacket::get_ack_size() * 8 / speedAsGbps(host_linkspeed) * t_cfg->get_diameter() * 1000); // (3)
```

| Component | What it is | Why it matters |
|-----------|-----------|----------------|
| (1) `2 × diameter_latency` | Round-trip propagation delay across every hop | The physical speed-of-light-in-fiber floor |
| (2) Data packet serialization | Time to push a full MTU onto the wire at each hop | Significant at lower link speeds (e.g., 10G) |
| (3) ACK serialization | Time to push the small ACK back across each hop | Usually small but not zero |

This gives `network_max_unloaded_rtt` — a **worst-case** estimate that assumes packets traverse the full diameter of the fat-tree (e.g., up to the core switches and back down). For a 3-tier fat-tree at 100 Gbps with 1µs per-hop latency, this is typically around 9-12µs.

#### Two Modes: Global vs Per-Flow

Not all flows travel the same distance. A flow between two hosts in the same rack traverses 2 hops; a flow crossing the entire fabric traverses 6. Using the worst-case diameter RTT for the same-rack flow would inflate its base RTT by 3×, causing it to systematically undercount queuing delay.

NSCC supports two modes, controlled by the `-enable_accurate_base_rtt` flag (`main_uec.cpp:880-893`):

```
  Mode 1: Global (default)
  ─────────────────────────────────────────────────
  All flows use network_max_unloaded_rtt (diameter-spanning RTT).
  Conservative: overestimates base RTT for nearby flows.

  Mode 2: Per-flow (-enable_accurate_base_rtt)
  ─────────────────────────────────────────────────
  Each flow uses get_two_point_diameter_latency(src, dst).
  Accurate: matches the actual hop count for each pair.
```

In per-flow mode, the RTT is computed per connection (`main_uec.cpp:824-826`):

```cpp
simtime_picosec transmission_delay = (Packet::data_packet_size() * 8 / speedAsGbps(linkspeed)
                                      * topo_cfg->get_diameter() * 1000)
                                   + (UecBasePacket::get_ack_size() * 8 / speedAsGbps(linkspeed)
                                      * topo_cfg->get_diameter() * 1000);
simtime_picosec base_rtt_bw_two_points = 2 * topo_cfg->get_two_point_diameter_latency(src, dest)
                                       + transmission_delay;
```

**When to use which:** The global mode is safe for uniform topologies where all flows span similar distances (e.g., permutation traffic across a symmetric fat-tree). Per-flow mode matters when flows have heterogeneous path lengths — for instance, a mix of intra-rack and inter-rack traffic, or oversubscribed topologies where rack-local flows should be treated differently.

#### Per-Flow Initialization

When a flow is created, base RTT seeds three derived values via `initNscc()` (`uec.cpp:162-184`):

```cpp
void UecSrc::initNscc(mem_b cwnd, simtime_picosec peer_rtt) {
    _base_rtt = peer_rtt;                                  // The zero point
    _base_bdp = timeAsSec(_base_rtt) * (_nic.linkspeed() / 8);  // Bytes that "fill the pipe"
    _bdp = _base_bdp;

    setMaxWnd(1.5 * _bdp);              // Never send more than 1.5× BDP
    setConfiguredMaxWnd(1.5 * _bdp);    // Remember the original cap
    ...
}
```

The chain of derivation:

```
  base_rtt  ──→  base_bdp = rtt × linkspeed / 8  ──→  maxwnd = 1.5 × bdp
  (time)         (bytes in the pipe)                   (send window cap)
```

The 1.5× factor on maxwnd allows the window to slightly overshoot BDP. This headroom is intentional: it lets the sender keep the pipe full even during fulfill adjustment boundaries, where the window might briefly lag behind the ideal value.

#### Dynamic Self-Refinement

The initial estimate from topology geometry is a *conservative starting point*. During simulation, base RTT is **refined downward** — never increased — as the flow observes actual RTT samples (`uec.cpp:1414-1425`):

```cpp
void UecSrc::update_base_rtt(simtime_picosec raw_rtt) {
    if (_base_rtt > raw_rtt) {
        _base_rtt = raw_rtt;
        _bdp = timeAsUs(raw_rtt) * _nic.linkspeed() / 8000000;
        _maxwnd = 1.5 * _bdp;
    }
}
```

This is called in two places:

| Trigger | Where | Condition |
|---------|-------|-----------|
| Every ACK (except RTS) | `uec.cpp:968-969` | Always |
| Every NACK | `uec.cpp:1593-1594` | Only if `update_base_rtt_on_nack` is true (default: yes) |

The "only decrease" rule means base RTT converges toward the *true minimum RTT* for the flow's actual path. If the initial topology estimate was 12µs but the first few packets measure 9.3µs on an uncongested path, base RTT tightens to 9.3µs and BDP/maxwnd are recalculated immediately.

**Why only decrease?** Consider the alternative — if base RTT could also *increase*, then a burst of congestion would raise the baseline, making the algorithm think "this higher RTT is normal." It would then underestimate queuing delay for all subsequent packets, becoming less responsive to congestion precisely when it should be more responsive. The monotonic-decrease design ensures base RTT always reflects the *best* conditions the flow has ever seen, which is the closest proxy for the true propagation delay.

**The NACK update flag:** NACKs (trimmed packets) carry RTT information too, since the receiver stamps them with arrival time. By default, NSCC uses NACK RTTs to refine base RTT. The `-disable_base_rtt_update_on_nack` flag turns this off, which can be useful if trimmed packets experience unusual forwarding delays that would artificially lower the base RTT estimate.

#### What Base RTT Drives

Base RTT is not just used for delay measurement — it propagates into many subsystems:

```
  base_rtt
    │
    ├──→ Queuing delay = raw_rtt - base_rtt          (uec.cpp:974)
    │      └──→ Quadrant decision (§2)
    │      └──→ Proportional increase magnitude (§3)
    │
    ├──→ BDP = base_rtt × linkspeed / 8              (uec.cpp:1417)
    │      └──→ maxwnd = 1.5 × BDP                   (uec.cpp:1418)
    │            └──→ Window cap for this flow
    │
    ├──→ QA evaluation window = base_rtt + target_Qdelay  (uec.cpp:1198)
    │      └──→ How long Quick Adapt waits before re-checking
    │
    ├──→ Probe scheduling = N × base_rtt             (uec.cpp:1080-1082)
    │      └──→ How often idle flows send keepalive probes
    │
    └──→ EWMA discount = base_rtt × 0.25             (uec.cpp:1432)
           └──→ What non-ECN high-delay samples are replaced with
               in the delay filter (see "Three-Case Asymmetry" below)
```

This dependency tree explains why an inaccurate base RTT has cascading effects: it doesn't just shift the delay signal, it also miscalibrates the maximum window, QA timing, and even the delay filter's trust model.

#### Design Trade-offs and Limitations

The base RTT design embeds several deliberate trade-offs:

**Conservative by default.** The global mode overestimates base RTT for short-path flows, which means their computed queuing delay (`raw_rtt - base_rtt`) can be *negative* when the path is truly uncongested. The code handles this (`uec.cpp:975-976`): if `raw_rtt < base_rtt`, it falls back to the smoothed average delay instead. This is safe but suboptimal — the flow temporarily loses its ability to distinguish "uncongested" from "mildly congested."

**No upward adjustment.** If a routing change sends the flow through a longer path (more hops), base RTT won't increase. The flow will now compute inflated queuing delay (`new_longer_rtt - old_shorter_base_rtt`), making it more conservative than necessary. In practice, datacenter topologies are symmetric and route changes are rare, so this is an acceptable trade-off for the stability gained.

**Tight coupling to BDP.** When base RTT decreases, maxwnd decreases too (since `maxwnd = 1.5 × bdp` and `bdp ∝ base_rtt`). This means an unusually low RTT sample could shrink the send window. The monotonic-minimum approach trusts that the lowest observed RTT is closest to truth — but in rare cases (e.g., a measurement artifact from a nearly-empty network at startup), it could lock in an artificially low ceiling.

### Queuing Delay Extraction

With base RTT established as the zero point, extracting the congestion signal is a simple subtraction (`uec.cpp:974`):

```cpp
delay = raw_rtt - _base_rtt;
```

This `delay` value — the estimated queuing delay — is what flows into the four-quadrant decision matrix. It represents how much *extra* time this packet spent sitting in switch queues beyond the irreducible propagation delay.

### Target Delay: The Decision Boundary

Base RTT gives us the zero point; the queuing delay gives us the signal. But how much queuing delay is "too much"? That's what **target delay** (`_target_Qdelay`) answers — it's the threshold that divides the four-quadrant matrix into "low delay" (bottom row) and "high delay" (top row):

```
                    No ECN (skip=false)       ECN (skip=true)
                 ┌───────────────────────┬───────────────────────┐
  delay ≥ target │  Q0: Fair Increase     │  Q2: Mult. Decrease   │
                 ├───────────────────────┼───────────────────────┤
  delay < target │  Q1: Proportional Inc. │  Q3: NOOP             │
                 └───────────────────────┴───────────────────────┘
                                         ▲
                            target_Qdelay is this dividing line
```

Target delay controls the fundamental operating point of the algorithm: **how much queue build-up is the sender willing to tolerate before switching from aggressive increase to conservative increase?**

- **Set it too low:** The sender interprets even minor, transient queue build-up as "at the target." It spends most of its time in Q0 (fair increase — the slowest increase mode) and rarely gets the benefit of Q1's proportional acceleration. Throughput suffers, especially during ramp-up.

- **Set it too high:** The sender tolerates deep queues before reacting. It stays in Q1 (proportional increase) longer, filling buffers aggressively. Tail latency increases because packets sit in queues that the algorithm considers "acceptable."

This is the classic **throughput vs. latency trade-off**, and target delay is the knob that tunes it.

#### How Target Delay Is Set

Target delay follows a three-level priority system (`uec.cpp:105-113`):

```cpp
if (target_Qdelay > 0) {
    _target_Qdelay = target_Qdelay;          // Priority 1: Explicit CLI flag
} else {
    if (_network_trimming_enabled) {
        _target_Qdelay = _network_rtt * 0.75; // Priority 2: 75% of RTT (with trimming)
    } else {
        _target_Qdelay = _network_rtt;         // Priority 3: Full RTT (without trimming)
    }
}
```

| Priority | Source | Value | When used |
|----------|--------|-------|-----------|
| 1 | `-target_q_delay <µs>` CLI flag | User-specified (in microseconds) | When explicitly provided |
| 2 | Auto (trimming enabled) | `network_rtt × 0.75` | Default when `-disable_trim` is NOT set |
| 3 | Auto (trimming disabled) | `network_rtt × 1.0` | Default when `-disable_trim` IS set |

For a typical 100 Gbps, 3-tier fat-tree with ~12µs unloaded RTT and trimming enabled:

```
  target_Qdelay = 12µs × 0.75 = 9µs
```

This means the algorithm considers anything under 9µs of queuing delay to be "low delay" (Q1 or Q3 territory), and anything at or above 9µs to be "high delay" (Q0 or Q2 territory).

#### Why 0.75 × RTT with Trimming?

The 0.75 factor is a design choice that reflects how trimming changes the congestion landscape:

**With trimming (the common case):** When a queue overflows, excess packets are *trimmed* (truncated to headers) rather than dropped. This means the sender learns about congestion quickly through trim notifications (NACKs) without losing data. Because trimming provides fast, reliable feedback, the algorithm can afford a tighter target — it doesn't need large queue buffers as a safety margin. The 0.75× RTT target keeps queues shallow while still giving enough headroom for the proportional increase mechanism (Q1) to do useful work.

**Without trimming:** Packets that overflow a queue are simply dropped. Loss detection is slower (requires timeouts or duplicate ACKs), so the sender needs more queue headroom to avoid underutilization during recovery. A full-RTT target allows deeper queues, giving the network more buffer to absorb bursts before packets are lost.

#### What Target Delay Drives

Target delay doesn't just split the quadrant matrix — it propagates into the algorithm's internal scaling:

```
  target_Qdelay
    │
    ├──→ Quadrant boundary: delay ≥ target → Q0/Q2, delay < target → Q1/Q3
    │                                                          (uec.cpp:1338-1358)
    │
    ├──→ Proportional increase magnitude:
    │      inc_bytes += alpha × acked_bytes × (target - delay)
    │      The "gap to target" controls how fast the window grows    (uec.cpp:1224)
    │
    ├──→ Multiplicative decrease trigger:
    │      Only cut if avg_delay > target_Qdelay                   (uec.cpp:1252)
    │      Cut magnitude: cwnd *= max(1 - γ×(avg-target)/avg, 0.5) (uec.cpp:1255)
    │
    ├──→ QA threshold = 4 × target_Qdelay                         (uec.cpp:120)
    │      Quick Adapt fires when delay exceeds this extreme threshold
    │
    ├──→ QA evaluation window = base_rtt + target_Qdelay           (uec.cpp:1198)
    │      How long Quick Adapt waits before re-checking
    │
    ├──→ Scaling factor_b = target_Qdelay / reference_rtt          (uec.cpp:123)
    │      Normalizes all rate parameters to the delay operating point
    │
    ├──→ Alpha (proportional rate) = 4 × MSS × a × b / target     (uec.cpp:125)
    │      Inversely proportional: tighter target → lower alpha → gentler increase
    │
    ├──→ Probe scheduling = base_rtt + target_Qdelay               (uec.cpp:1080)
    │      Idle flow keepalive interval
    │
    └──→ EWMA filter Case 1 trigger: no-ECN && delay > target      (uec.cpp:1431)
           Discounts non-ECN high-delay samples in the delay filter
```

#### The Proportional Increase Connection

The deepest interaction between target delay and the window dynamics is in proportional increase (Q1). When `delay < target`, the increase magnitude is:

```cpp
// uec.cpp:1224
_inc_bytes += _alpha * newly_acked_bytes * (_target_Qdelay - delay);
```

The term `(target - delay)` acts as a **linear ramp**: when delay is near zero (empty network), the gap is large and the window grows fast. As delay approaches the target, the gap shrinks and growth tapers off. At exactly the target, growth is zero — the sender has reached the operating point and naturally stops increasing.

```
  increase
  magnitude
       ↑
       │ ╲
       │   ╲
       │     ╲          ← linear ramp: inc ∝ (target - delay)
       │       ╲
       │         ╲
       │           ╲
       └─────────────╲──→ queuing delay
       0           target
                    ▲
              growth = 0 here
              (natural equilibrium)
```

This means target delay isn't just a threshold — it's the **equilibrium point** of the proportional control loop. The algorithm naturally converges to a state where queuing delay hovers around the target value, because growth decreases as delay approaches it and turns negative (via Q0/Q2) when it exceeds it.

#### Interaction with Alpha

Notice that `_alpha` itself depends on target delay (`uec.cpp:125`):

```cpp
_alpha = 4.0 * _mss * _scaling_factor_a * _scaling_factor_b / _target_Qdelay;
```

Since `_scaling_factor_b = _target_Qdelay / _reference_network_rtt`, this simplifies to:

```
alpha = 4 × MSS × (network_bdp / ref_bdp) / ref_rtt
```

The `_target_Qdelay` in the numerator (via `scaling_factor_b`) and denominator cancel out. This means **alpha is actually independent of target delay** after scaling — a deliberate design choice. Changing target delay shifts *where* the equilibrium sits (more or less queuing) but doesn't change *how fast* the algorithm converges to it. The proportional ramp shape is preserved; only the x-axis endpoint moves.

#### Practical Guidance

For most deployments, the auto-computed default (0.75 × RTT with trimming) is a good starting point. When tuning:

| Goal | Adjustment | Trade-off |
|------|-----------|-----------|
| Lower tail latency | Decrease target (e.g., 3-5µs) | Slower ramp-up, slightly lower throughput utilization |
| Higher throughput | Increase target (e.g., 9-12µs) | Deeper queues, higher p99 latency |
| Match buffer depth | Set target ≈ (queue_size / linkspeed) × 0.5 | Aligns the operating point with physical buffering |

The simulation results in §11 (Target Delay Sensitivity) sweep target delay from 3µs to 9µs and show this trade-off empirically.

### EWMA Responsiveness: Why So Slow?

NSCC uses `_delay_alpha = 0.0125` — that's 1/80. This is an extremely slow filter. To see why, compare step responses:

```
  avg_delay
       ↑    actual delay (step from 0 to 6µs)
  6µs  │    ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─
       │                      ╱── α=0.0125 (NSCC)
       │               ╱─────╱   ~80 samples to 63%
  3µs  │         ╱────╱
       │    ╱───╱
       │ ╱─╱  ╱── α=0.125 (10x faster)
       │╱ ╱       ~8 samples to 63%
       └──────────────────────────────→ samples
       0  10  20  40  60  80  100

  "NSCC chose the slowest filter. Why?"
```

With 16 paths and per-packet spraying, any single delay sample might come from the *one* congested path out of 16. A fast filter (α=0.125) would react to this single sample, pushing the average up and potentially triggering a decrease — even though 15/16 paths are fine. A few ACKs later, the average drops back down, triggering an increase. The result: wild oscillation between decrease and increase on every few ACKs.

The slow filter (α=0.0125) only moves meaningfully when *sustained* congestion shifts the overall average. It takes ~80 samples (roughly 5 fulfill adjustment periods at 8 MTUs each) for the filter to reach 63% of a new steady-state value. This means transient per-path congestion is smoothed out, and only persistent network-wide congestion drives the average up.

### Two Delays, Two Jobs

There's a subtle but critical architectural detail hidden in the code: NSCC uses delay at **two different timescales** within the same ACK processing path.

```
  processAck() timeline for a single ACK:
  ─────────────────────────────────────────────
  1. update_delay()  → feeds raw sample into EWMA → updates _avg_delay
  2. delay = raw_rtt - base_rtt  → raw per-packet delay       (uec.cpp:973)
  3. multipath feedback (processEv)
  4. Quadrant decision uses raw `delay`     ← FAST: reacts to THIS packet
  5. If mult_dec: cut uses `_avg_delay`     ← SLOW: smoothed over ~80 samples
                                                               (uec.cpp:1243)
```

The **quadrant selection** (step 4) uses the **raw per-packet** queuing delay — what *this specific packet* experienced. But the **multiplicative decrease magnitude** (step 5) uses the **filtered average** delay from `get_avg_delay()`.

This split is intentional. You want to *enter* the decrease quadrant quickly — if this specific packet saw ECN + high delay, that's a real signal worth acting on immediately. But you want the *magnitude* of the cut to reflect sustained conditions, not one unlucky sample. A single packet that bounced off a transiently full queue might have 3× target delay, but if the filtered average is only 1.2× target, the actual cut will be gentle.

The result is a **fast trigger + slow actuator** pattern: the system is responsive to emerging congestion (entering multiplicative decrease within one ACK) but resistant to overreaction (the cut size is damped by the slow EWMA filter). This is arguably the single most important architectural insight for understanding how NSCC avoids oscillation while staying responsive.

### EWMA Filter with Three Special Cases

The delay filter (`uec.cpp:1402-1423`) uses `_delay_alpha = 0.0125` with three distinct behaviors:

```
Case 1: No ECN and delay > target
    → avg = 0.0125 * (base_rtt * 0.25) + 0.9875 * avg

Case 2: delay > 5 * base_rtt (extreme outlier)
    → avg = 0.0125 * delay + 0.9875 * avg

Case 3: Normal (default path)
    → avg = 0.0125 * delay + 0.9875 * avg
```

### The Three-Case Asymmetry: A Trust Diagram

Why treat these cases differently? It comes down to how much we *trust* each sample:

```
  Sample type              → What filter sees    → Why?
  ─────────────────────────────────────────────────────────────────
  ECN + high delay         → actual delay        → "Trusted: switch confirmed
                                                     congestion"

  No ECN + high delay      → base_rtt × 0.25     → "Discounted: probably just
                                                     one bad path out of many"

  Extreme outlier (>5×)    → actual delay         → "Trusted: something is
                                                     seriously wrong"
```

**Why the asymmetry in Case 1:** When we see high delay *without* ECN, the sample is discounted to `base_rtt * 0.25` instead of the actual delay. This prevents non-ECN high-delay samples (which may be from a single congested path in a multi-path fabric) from inflating the average that drives multiplicative decrease. The filter trusts ECN-marked delay more than unmarked delay.

**Why Case 2 overrides Case 1:** If the delay is extreme (more than 5× the base RTT), something is seriously wrong regardless of ECN status — possibly a failing link or major routing change. The filter uses the actual delay to respond quickly to genuine emergencies.

---

## 5. Quick Adapt: The Emergency Brake

### Problem Statement

Quick Adapt detects when a flow is severely underperforming—its window is much larger than what the network can actually deliver. This happens during sudden congestion shifts, incast events, or routing changes.

### Why Normal Decrease Isn't Enough

Multiplicative decrease cuts at most 50% per RTT. In a 64-flow incast, the fair share might be 1/64 of the pipe. Even with 50% cuts every RTT, it takes log₂(64) ≈ 6 RTTs to reach fair share — and during those 6 RTTs, the excess packets cause severe queuing and loss for everyone.

```
         WITHOUT Quick Adapt              WITH Quick Adapt
  cwnd                                cwnd
  192KB │╲                            192KB │╲
        │  ╲ slow decrease                  │  ╲
        │    ╲  via mult_dec                │   ╳ ← QA fires at 18µs!
   96KB │     ╲                             │   │
        │       ╲                           │   │
        │         ╲  still too big   1.5KB  │   └─── reset to achieved_bytes
        │           ╲                       │        (stale pipeline ignored)
        └────────────────→ time             └────────────────→ time
        0   50   100µs                      0   18   36µs

  "Without QA: 5-10 RTTs to converge    With QA: 1 RTT to converge"
```

Quick Adapt doesn't *decrease* the window — it **resets** it to what was actually achieved in the last measurement period. This is fundamentally faster than iterative multiplicative decrease.

### Mechanism (`uec.cpp:1145-1200`)

Quick Adapt operates on a periodic window of `_base_rtt + _target_Qdelay`:

```
Every (base_rtt + target_Qdelay) interval:
  if (trigger_qa OR loss OR delay > qa_threshold)
    AND (achieved_bytes < maxwnd >> qa_gate):
      cwnd = max(achieved_bytes, min_cwnd)
```

**Key variables:**
- `_achieved_bytes`: Bytes actually delivered during the current QA window. Incremented on every ACK (`uec.cpp:916`).
- `_qa_gate`: Right-shift amount (default 3, so threshold = maxwnd/8). Set via `initNsccParams` (`uec.cpp:114-118`).
- `_qa_threshold`: Delay threshold = `4 * _target_Qdelay` (`uec.cpp:119`).
- `_trigger_qa`: Set to true on NACKs (`uec.cpp:1375`).

### Exploring the qa_gate Parameter

The `qa_gate` parameter controls how aggressively Quick Adapt triggers. It sets the threshold as a right-shift of maxwnd:

```
  qa_gate=0: threshold = maxwnd/1    → QA fires when achieved < 100% of maxwnd
                                       (almost always fires — too aggressive)
  qa_gate=1: threshold = maxwnd/2    → fires when achieved < 50%
  qa_gate=2: threshold = maxwnd/4    → fires when achieved < 25%
  qa_gate=3: threshold = maxwnd/8    → fires when achieved < 12.5%  ← default
  qa_gate=4: threshold = maxwnd/16   → fires when achieved < 6.25%
                                       (too conservative — misses incasts)
```

The default qa_gate=3 means Quick Adapt only fires when the flow is achieving less than 12.5% of its maximum window. This is a strong signal that something is seriously wrong — not just mild congestion but a major capacity reduction.

### The "Ignore Stale Feedback" Mechanism

After QA fires, packets already in flight will return with stale feedback. QA handles this by setting:

```cpp
_bytes_to_ignore = _in_flight;    // uec.cpp:1185
_bytes_ignored = 0;               // uec.cpp:1186
```

Subsequent ACKs increment `_bytes_ignored` (`uec.cpp:918`) and while `_bytes_ignored < _bytes_to_ignore`, the regular CC update is suppressed (`uec.cpp:1157-1158`). This prevents the stale pipeline from further reducing the window after QA has already corrected it.

### What Happens After QA Fires?

After QA resets cwnd to `achieved_bytes`, the flow re-enters normal CC operation. The stale-feedback mechanism (`_bytes_to_ignore`) suppresses CC updates for the old pipeline. Once those drain, the flow is typically back in the proportional_increase quadrant (since the network has less congestion now that the flow backed off) and ramps up normally. QA is a one-shot reset, not a new mode — it puts you back at the start of the normal feedback loop.

### What Prevents QA from Being Too Aggressive?

QA has a two-part guard: (1) something bad must be happening (`trigger_qa`, loss, or delay > 4× target), **AND** (2) the flow must be severely underperforming (`achieved_bytes < maxwnd >> qa_gate`, i.e., less than 12.5% of maxwnd with default qa_gate=3). Both conditions must be true simultaneously. A flow experiencing moderate congestion — high delay but still delivering 50% of maxwnd — will never trigger QA. It'll be handled by normal multiplicative decrease instead. QA only fires when the flow is virtually stalled, making it a true emergency mechanism rather than an aggressive optimizer.

#### Simulation: QA Gate Sensitivity

The figure below shows completion rate and p99 FCT under 32-to-1 incast for different `qa_gate` values. The default `qa_gate=3` provides the best balance.

![QA Gate Sensitivity](figures/sim_qa_gate.png)

![Incast Degree Scaling](figures/sim_incast_degree.png)

---

## 6. Packet Spraying & Load Balancing

NSCC operates natively with per-packet spraying. The multipath engine (`uec_mp.h`) selects an entropy value for each packet that determines its physical path through the fabric.

### How Path Selection Works: The Bitmap Example

The most illustrative strategy is Bitmap, which maintains a penalty score per path:

```
  Cycle 1:  Path scores [0, 0, 0, 0, 0, 0, 0, 0]  → all paths available
            Send on: 1, 2, 3, 4, 5, 6, 7, 8

  Feedback: Path 3 gets ECN, Path 7 gets NACK

  Cycle 2:  Path scores [0, 0, 1, 0, 0, 0, 1, 0]  → skip paths 3, 7
            Send on: 1, 2, 4, 5, 6, 8, 1, 2

  Feedback: Path 3 penalty decays, Path 7 gets another NACK

  Cycle 3:  Path scores [0, 0, 0, 0, 0, 0, 2, 0]  → skip only path 7
            Send on: 1, 2, 3, 4, 5, 6, 8, 1
```

Paths with penalties are skipped during selection. Penalties decay over time, so transiently congested paths are reintroduced. This is *the other half* of the NOOP quadrant — the CC doesn't change the window, but the multipath engine steers traffic away from the hot path.

### Four Strategies

**Oblivious** (`UecMpOblivious`): Round-robin through paths with random XOR permutation. No feedback—purely deterministic rotation. Simplest strategy, works well when all paths are symmetric.

**Bitmap** (`UecMpBitmap`): Maintains a penalty score per path. Paths receiving ECN, NACK, or timeout feedback get their penalty incremented. Paths with penalties are skipped during selection. Penalties are bounded by `_max_penalty`.

**REPS** (`UecMpReps`): Recycling good paths. Uses a circular buffer to track path quality. Good paths (those with clean ACKs) are recycled for reuse. Bad paths are removed from the rotation. Supports trimming-aware mode.

**Mixed** (`UecMpMixed`): Hybrid of Bitmap and REPS Legacy. Combines penalty-based avoidance with path recycling.

### Feedback Integration

On every ACK/NACK, the multipath engine receives feedback (`uec.cpp:1038`):

```cpp
_mp->processEv(pkt.ev(), pkt.ecn_echo() ? PATH_ECN : PATH_GOOD);
```

NACK processing similarly reports `PATH_NACK` or `PATH_TIMEOUT`. This closes the loop: CC adjusts the window, multipath adjusts the *paths*.

### Closing the Loop: NOOP + Multipath in Action

The NOOP quadrant only makes sense when you see how it interacts with the multipath engine. Here's a concrete sequence showing the full loop:

```
  1. Packet sent on Path 5, gets ECN-marked at a congested switch
  2. ACK arrives: ecn=true, delay=3µs (low — other paths are fine)
  3. Quadrant decision: ECN + low delay → NOOP (cwnd unchanged)
  4. Multipath feedback: processEv(PATH_ECN) → Path 5 gets penalty +1
  5. Next packet: Path 5 is skipped → sent on Path 6 instead
  6. Path 6 ACK: no ECN, low delay → proportional_increase → cwnd grows
```

Net effect: the flow **maintained its sending rate** AND **avoided the congested path**. No throughput was lost. This is why NOOP exists — it separates the "what to do about the window" decision from the "what to do about the path" decision. Any other quadrant response (decrease, or even a conservative increase) would penalize the flow's global sending rate for congestion that only affected one path out of many.

### Steer First, Throttle Second: The Design Philosophy

The deepest insight in NSCC's multipath integration is the **ordering of responses**: path steering (load balancing) is the first line of defense; window reduction (congestion control) is the fallback. This "steer first, throttle second" philosophy is what makes the NOOP quadrant viable and what the SMaRTT-REPS "Wait to Decrease" (WTD) mechanism formalizes.

**The 5-step mental timeline:**

```
  Step 1 — Collision:     Two flows collide on Path 3 (ECMP hash collision).
                          Switch queue builds; ECN marks appear.

  Step 2 — ECN arrives:   Sender sees ECN + low delay → Quadrant = Q3 (NOOP).
                          cwnd is NOT reduced.

  Step 3 — LB reroutes:   Multipath engine (Bitmap/REPS) penalizes Path 3.
                          Next packets are sent on Paths 4, 5, etc.

  Step 4 — Resolution:    Clean ACKs return from alternate paths.
                          Flow enters Q1 (proportional increase) → cwnd grows.
                          Path 3 penalty decays after marks stop.

  Step 5 — Fallback:      If marks PERSIST (LB couldn't fix it — e.g., global
                          congestion), delay rises above target.
                          Flow transitions to Q2 (multiplicative decrease).
```

**Net effect:** for ECMP collisions (Steps 1–4), the flow maintained its sending rate *and* avoided the congested path — zero throughput lost. For global congestion (Step 5), the system falls through to CC-level rate reduction. The ordering means you never cut your window for a problem that path steering can solve.

**Wait to Decrease (WTD).** SMaRTT-REPS [SMaRTT-REPS] formalizes this ordering with a mechanism called WTD: upon receiving ECN + low delay, the sender *defers* any CC-level decrease for a brief window (approximately one RTT), giving the load balancer time to reroute. WTD is **not** "ignore ECN" — it's "defer the cwnd reaction long enough for LB to try, then fall back to CC if marks persist." The implementation in this simulator achieves the same effect via the pure NOOP quadrant: since NOOP makes no window change, the load balancer has a full fulfill period (~1 RTT) to resolve the issue before the next batch of ACKs could shift the quadrant decision.

**The "early global congestion" corner case.** ECN + low delay can *also* be the very beginning of global congestion — queues are just starting to build everywhere, delay hasn't caught up yet. WTD's "defer" framing handles this naturally: if marks persist after the LB has tried rerouting, delay will rise (because every path is now congested), and the flow transitions from Q3 (NOOP) to Q2 (multiplicative decrease). The deferral costs at most one RTT of reaction time — a small price for avoiding unnecessary window cuts on the far more common ECMP collision case.

**When NOOP does and doesn't help — two contrasting scenarios:**

**Scenario 1: ECMP collision (load-balancer fixable)**
```
  t=0µs   Two flows collide on Path 3 (ECMP hash collision)
  t=2µs   ECN marks start arriving, delay still low
          → NOOP fires: cwnd unchanged
          → Multipath engine: Path 3 penalized
  t=4µs   Next packets routed to Path 4, 5 instead
  t=6µs   Clean ACKs arrive: no ECN, low delay
          → proportional_increase: cwnd grows
  t=8µs   Path 3 penalty decays, collision resolved

  Result: zero throughput loss. Steer first resolved it.
```

**Scenario 2: Receiver incast (NOT load-balancer fixable)**
```
  t=0µs   64 flows converge on one receiver port — ALL paths congested
  t=2µs   ECN marks + HIGH delay on every path
          → Multiplicative decrease fires (not NOOP — delay is high)
  t=4µs   Still congested. MD cuts 40% per RTT... but fair share is 1/64
  t=12µs  After several RTTs, still far above fair share
          → achieved_bytes < maxwnd/8 → Quick Adapt fires!
          → cwnd reset to achieved_bytes (~2KB)
  t=14µs  Flow ramps up from new baseline via proportional_increase

  Result: NOOP never engaged (delay was always high).
  MD was too slow; QA provided the emergency reset.
  "Throttle second" kicked in because steering couldn't help.
```

The key distinction: NOOP + LB handles **per-path** congestion (one hot path among many). When congestion is **global** (all paths affected), delay rises above target and the system falls through to multiplicative decrease. If even MD is too slow, Quick Adapt provides the backstop. The three mechanisms form a hierarchy: **steer → throttle → emergency reset**.

---

## 7. Scaling & Parameter Design

### One Algorithm, Every Network Speed

NSCC was designed for a 100 Gbps reference network. But the same algorithm must work at 400G, 800G, and beyond. How do you make parameters that scale?

### The Problem Without Scaling

Imagine taking the 100G parameters and running them on a 400G network. On the 100G reference network (BDP=150KB, target=9µs), one fulfill period of proportional_increase at delay≈0 accumulates roughly `alpha × 8×MTU × target`. With the reference alpha of 2731/µs, that's about `2731 × 32KB × 9µs ≈ 787 million` in raw accumulator units. After normalization (`÷ cwnd`), this gives a meaningful per-batch step that fills the 150KB pipe in a reasonable number of RTTs.

```
  100G network: BDP = 150KB.  Alpha = 2731/µs.
  Per-batch accumulation at zero delay is large relative to cwnd.
  Pipe fills in ~N RTTs. ✓

  400G network: BDP = 600KB.  Same alpha = 2731/µs (unscaled).
  Per-batch accumulation is IDENTICAL, but the pipe is 4× larger.
  Now it takes ~4N RTTs to fill. ✗

  400G network with scaling: Alpha = 10923/µs (4× larger).
  Per-batch accumulation is 4× larger, pipe is 4× larger.
  Pipe fills in ~N RTTs again. ✓
```

The fix: scale parameters proportionally to the network's BDP and target delay. The key invariant is that the number of RTTs to fill the pipe stays constant regardless of network speed.

### Reference Network

All NSCC parameters are designed for a **reference network** of 100 Gbps, 12µs RTT (`uec.cpp:93-95`):

```cpp
_reference_network_linkspeed = speedFromGbps(100);
_reference_network_rtt = timeFromUs(12);
_reference_network_bdp = timeAsSec(_reference_network_rtt) * (_reference_network_linkspeed / 8);
```

### Scaling Factors

Two ratios scale everything to the actual network (`uec.cpp:121-122`):

```
scaling_factor_a = network_bdp / reference_bdp    // BDP ratio
scaling_factor_b = target_Qdelay / reference_rtt  // delay ratio
```

For a 400 Gbps, 12µs RTT network: `scaling_factor_a = 4.0`, `scaling_factor_b = 0.5` (with default target = 0.75 * RTT with trimming).

### Concrete Numbers Across Network Speeds

Here are the actual computed parameter values for three representative networks (assuming MSS = 4KB = 4096 bytes, trimming enabled so target = 0.75 × RTT):

```
                     100G/12µs    400G/12µs    800G/6µs
                     (reference)
  ────────────────────────────────────────────────────────
  BDP                150KB        600KB        600KB
  maxwnd (1.5×BDP)   225KB        900KB        900KB
  target_Qdelay      9µs          9µs          4.5µs
  scale_a            1.0          4.0          4.0
  scale_b            0.75         0.75         0.375
  alpha              2731/µs      10923/µs     14564/µs
  fi                 20KB         80KB         80KB
  eta                614B         2458B        2458B
  fi_scale           0.25         1.0          1.0
  gamma              0.8          0.8          0.8
  delay_alpha        0.0125       0.0125       0.0125
```

Notice the pattern: `scale_a` captures the BDP ratio (800G/6µs has the same BDP as 400G/12µs, so both get `scale_a=4`). `scale_b` captures the delay ratio. Parameters that need to scale with *capacity* (fi, eta, fi_scale) use `scale_a`. Parameters that need to scale with *both* capacity and delay sensitivity (alpha) use both.

The key invariant: **at any network speed, the algorithm takes roughly the same number of RTTs to fill the pipe and the same number of RTTs to converge to fairness.**

### How Parameters Scale

| Parameter | Formula | Scaling |
|-----------|---------|---------|
| `_alpha` | `4.0 * MSS * a * b / target_Qdelay` | Proportional increase rate |
| `_fi` | `5 * MSS * a` | Fair increase constant |
| `_eta` | `0.15 * MSS * a` | Minimum additive increase |
| `_fi_scale` | `0.25 * a` | Fast increase multiplier |
| `_adjust_bytes_threshold` | `8 * MTU` | Fulfill trigger (fixed) |
| `_adjust_period_threshold` | `network_rtt` | Fulfill time trigger |

### Per-Flow vs Global Parameters

**Global (static):** All scaling factors, alpha, fi, eta, gamma, qa_threshold, delay_alpha, target_Qdelay, min_cwnd. Set once via `initNsccParams()` (`uec.cpp:86-159`).

**Per-flow (instance):** `_base_rtt`, `_bdp`, `_maxwnd`, `_cwnd`, `_avg_delay`, `_achieved_bytes`, `_inc_bytes`. Set via `initNscc()` (`uec.cpp:161-184`). Allows different flows to have different base RTTs (e.g., for inter-rack vs intra-rack).

---

## 8. SLEEK Loss Recovery

*Note: SLEEK is specific to this simulator's implementation; the UET NSCC spec [UET] focuses on the congestion management subsystem rather than detailed loss recovery.*

### Why TCP's Approach Fails with Spraying

TCP detects loss using 3 duplicate ACKs: if you receive 3 ACKs for the same sequence number, the missing packet was probably lost. This works when packets travel a single path and arrive in order:

```
  TCP (single path):              NSCC (sprayed):
  Sent: 1, 2, 3, 4, 5            Sent: 1, 2, 3, 4, 5
  Recv: 1, 2, _, 4, 5            Recv: 3, 1, 5, 2, _
            ↑ gap = loss!               ↑ reordering is NORMAL

  TCP: 3 dupACKs → retransmit    TCP: would trigger false retransmits
                                       on EVERY flow, EVERY RTT
```

With 8 or 16 paths of varying latency, packets routinely arrive out of order. A fixed threshold of 3 would trigger loss recovery constantly — on perfectly healthy flows. You need a threshold that scales with the amount of expected reordering, which scales with the window size and number of paths.

### SLEEK (`uec.cpp:1433-1497`)

SLEEK is a probe-based loss detection mechanism that replaces TCP's 3-duplicate-ACK approach.

### Threshold-Based Entry

Loss recovery triggers when out-of-order count exceeds a threshold:

```cpp
// uec.cpp:1435-1436
threshold = min(loss_retx_factor * _cwnd, _maxwnd);   // loss_retx_factor = 1.5
threshold = max(threshold, min_retx_config * avg_size); // min_retx_config = 5
```

The threshold is `1.5 × cwnd` — orders of magnitude higher than TCP's fixed 3. With a 600KB window and 4KB packets, that's ~225 out-of-order packets before triggering loss recovery. In a spraying network, this is appropriate: with 16 paths, any given packet might arrive 15 positions later than expected.

Why specifically 1.5× cwnd? With per-packet spraying across N paths, a packet might arrive up to ~N positions out of order (one full round-robin of paths). The cwnd typically holds many rounds' worth of packets, so 1.0× cwnd already covers normal reordering. The 0.5× extra headroom accounts for bursty arrivals and path-latency variance. Using 1.0× would cause occasional false positives on healthy flows; using 2.0× would delay real loss detection by a full extra window, allowing genuine losses to go unnoticed for too long.

### How It Differs from TCP's 3-DupACK

- **TCP**: Enters fast recovery after exactly 3 duplicate ACKs, regardless of window size.
- **SLEEK**: Threshold scales with cwnd (`1.5 * cwnd` packets OOO). With spraying, reordering is the norm, so a fixed threshold would cause constant false positives.

### Entry and Exit

```cpp
// Enter: uec.cpp:1460-1466
if (!_loss_recovery_mode && _rtx_queue.empty()) {
    _loss_recovery_mode = true;
    _recovery_seqno = _highest_sent;
}

// Exit: uec.cpp:1450-1455
if (cum_ack >= _recovery_seqno && _loss_recovery_mode) {
    _loss_recovery_mode = false;
}
```

During loss recovery, packets between `cum_ack` and `_recovery_seqno` are retransmitted. The mode exits when the cumulative ACK advances past the recovery point.

### Probe Mechanism

SLEEK also uses explicit probes (`uec.cpp:1066-1092`). When a flow has outstanding data and receives an ACK, it schedules a probe timer. If a probe ACK returns with low delay, it indicates lost (not delayed) packets and triggers loss recovery.

The intuition behind probes: a probe is a question — "did you receive everything I sent?" If the probe ACK comes back quickly (low delay), it means the network path is clear. So the missing packets aren't stuck in a queue somewhere waiting to be delivered — they're genuinely lost. A probe that returns with *high* delay is ambiguous (the missing packets might still be in transit on a slow path), but a probe that returns with *low* delay is a strong signal that the network has drained and anything still missing is gone for good.

---

## 9. Comparison: NSCC vs TCP Cubic vs DCQCN

A clean way to compare congestion-control algorithms is the triple: **Knob** (what you control) + **Signal** (what you observe) + **Assumptions** (what world you believe in). Every CC design makes a choice in each dimension, and those choices cascade into its behavior, failure modes, and ideal deployment context. The sections below use this lens to compare NSCC, TCP Cubic, and DCQCN — and to position bridge protocols like DCTCP and Swift.

### 9.1 Side-by-Side Table

| Property | NSCC | TCP Cubic | DCQCN |
|----------|------|-----------|-------|
| **CC Type** | Window-based | Window-based | Rate-based |
| **Primary Signal** | Delay + ECN | Loss + ECN | ECN via CNP |
| **Increase** | Proportional to (target - delay) | Cubic function of time | Timer + byte-counter recovery |
| **Decrease** | Proportional: `γ*(delay-target)/delay` | Multiplicative: `β=0.7` | Alpha-proportional: `RC*(1-α/2)` |
| **Pacing** | NIC-limited (no software pacing) | No (ACK-clocked) | Hardware rate limiter |
| **Retransmission** | SLEEK probe-based | 3 DupACK + RTO | Go-back-N + RTO |
| **Multi-path** | Native (per-packet spraying) | Single-path | Single-path |
| **Fairness** | High (Jain's FI ≈ 1.000) | Good (FI ≈ 0.976-0.997) | Adaptive (alpha-driven) |

### 9.2 Response Shape Comparison

How does each algorithm's window evolve through a congestion event? The shapes are fundamentally different:

```
  cwnd
   ↑
   │    ╱╲    ╱╲
   │   ╱  ╲  ╱  ╲       ← TCP Cubic: sawtooth with cubic recovery
   │  ╱    ╲╱    ╲          (sharp loss-driven cuts, time-based regrowth)
   │ ╱
   │╱─────────────────   ← NSCC: smooth, proportional (small oscillations)
   │                        (delay-proportional cuts, headroom-proportional growth)
   │
   │  ████
   │  █  █████           ← DCQCN: rate steps (timer-driven)
   │  █      █████          (CNP-driven cuts, 55µs timer recovery phases)
   └──────────────────→ time
```

These shapes are a direct consequence of signal type: TCP Cubic sees **binary** events (loss/no-loss) → sharp transitions between probing and cutting. DCQCN adjusts on **fixed timers** (55µs) → discrete staircase steps regardless of congestion severity. NSCC responds to **continuous** signals (delay magnitude) → smooth curves where both the direction and the magnitude of change are proportional to the network state. The smoothness isn't cosmetic — it means NSCC produces less queuing variance, less packet loss, and more predictable latency at equilibrium.

### 9.3 Bridge Protocols: DCTCP and Swift

To understand why NSCC combines *both* delay and ECN, it helps to see what happens when you use only one:

**DCTCP** [DCTCP] (ECN-only, window-based): Adjusts cwnd based on the *fraction* of marked packets in a window. If 30% of ACKs carry ECN, cut by ~30%. This is elegant — it extracts multi-bit information from a 1-bit signal via statistical aggregation. But DCTCP has no delay signal, so it can't distinguish "one path is hot" from "all paths are moderately loaded." In a spraying fabric, it would over-react to per-path collisions.

**Swift** [Swift] (delay-targeting, datacenter): Targets end-to-end delay as the primary congestion signal, adjusting the window to hold delay near a target. This gives rich magnitude information, but delay is a trailing indicator — Swift can be slow to react to sudden congestion because it has to wait for RTT measurements to reflect queue build-up.

An earlier delay-targeting datacenter protocol, **TIMELY** [TIMELY], pioneered the use of RTT gradients as a congestion signal; Swift refines the approach with fabric-level delay targeting and NIC-hardware timestamps.

NSCC combines the strengths of both families: ECN provides Swift's missing fast trigger (react quickly when congestion appears), while delay provides DCTCP's missing magnitude information (know *how much* to react). The four-quadrant design is what you get when you fuse these two approaches and add the spraying-specific NOOP case.

### 9.4 Design Philosophy Comparison

**NSCC:** *"Respond proportionally to what the network tells you, on two dimensions."* The delay signal provides magnitude information (how congested?) while ECN provides a binary classification (is this path specifically congested?). The four-quadrant design means NSCC never overreacts to ambiguous signals.

**TCP Cubic** [TCP Cubic]**:** *"Probe aggressively, back off on loss."* The cubic function `W(t) = C*(t-K)³ + W_max` (`tcpcubic.cpp:8`) creates a distinctive convex-then-concave growth curve. It probes cautiously near the last known good point (`W_max`) and aggressively when far from it. Time-based rather than RTT-based, so it's fair across flows with different RTTs. Fundamentally designed to probe until a "hard" congestion event (loss/mark) and then regrow over time — a mismatch for datacenter fabrics with shallow buffers and µs-scale RTTs.

**DCQCN** [DCQCN]**:** *"Adjust the sending rate based on ECN feedback via a separate control plane."* The receiver generates CNP packets on ECN marks (`dcqcn.cpp:228-238`). The sender cuts rate by `α/2` on each CNP (`dcqcn.cpp:56`) and recovers through three phases: fast recovery (averaging `RC` with target `RT`), active increase (`RT += RAI`), and hyper increase (`RT += RHAI`). Timer intervals are implementation-specific (this simulator uses 55µs per `dcqcn.cpp:20`; actual deployments may differ).

### 9.5 Key Assumptions Baked Into Each Design

**NSCC assumes:**
- Multi-path fabric with per-packet spraying
- Packet trimming available (optional but expected)
- Known base RTT (propagation delay is measurable)
- Shallow buffers (target queuing delay ~ 0.75 * RTT)
- ECN marking at switches

**TCP Cubic assumes:**
- Single path per flow
- Loss as primary congestion signal (ECN optional)
- Deep buffers tolerable (probes until loss)
- Internet-scale RTTs (the cubic function is time-based, not RTT-based)
- Reliable in-order delivery expected

**DCQCN assumes:**
- Lossless fabric (PFC prevents packet loss)
- ECN marking at switches with CNP feedback
- Hardware rate limiting at the NIC
- RDMA semantics (no retransmission in CC layer)
- Single-path routing

### 9.6 Fairness Properties

**NSCC:** The combination of proportional increase (same formula for all flows) and BDP-scaled parameters means flows converge to equal shares quickly. The fair_increase mode specifically handles the steady-state case where flows coexist at the target delay point. Quick Adapt acts as a backstop, preventing any flow from holding excess capacity for more than one QA interval (~1 RTT + target delay).

**TCP Cubic:** The time-based cubic function gives good inter-RTT fairness (flows with different RTTs grow at similar rates). However, the loss-driven decrease means fairness depends on which flow happens to experience loss. Fast convergence (`tcpcubic.cpp:391-393`) helps: if a flow loses below its previous `W_max`, it reduces `W_max` further, yielding capacity faster.

**DCQCN:** The alpha parameter tracks congestion intensity per-flow. Flows seeing more ECN marks develop higher alpha values and cut more aggressively. This creates adaptive fairness but convergence depends on the timer period (55µs) and the alpha decay rate (`g = 1/256`).

### 9.7 Response to Congestion: Timing Comparison

**NSCC response cycle:**
```
ACK arrives → extract delay & ECN
  → classify into quadrant
  → accumulate inc_bytes (or decrease immediately)
  → if (received_bytes > 8*MTU OR time > 1 RTT):
      apply fulfill_adjustment: cwnd += inc_bytes/cwnd
  → Quick Adapt check every (base_rtt + target_Qdelay)
```
Effective response time: **~1 RTT** (fulfill adjustment period). Decrease is immediate; increase is batched.

**TCP Cubic response cycle:**
```
ACK arrives → inflate_window()
  → if slow start: cwnd += MSS (with HyStart check)
  → if congestion avoidance: bictcp_update → increment when ack_cnt > cnt
Loss detected → deflate_window()
  → ssthresh = cwnd * 0.7
  → cwnd = ssthresh
```
Effective response time: **per-ACK** for increase, **per-loss-event** for decrease.

**DCQCN response cycle:**
```
ECN-marked packet at receiver → send CNP (max once per 50µs)
CNP at sender:
  → RT = RC
  → RC = RC * (1 - α/2)
  → reset timer and byte counters
Every 55µs:
  → T++, check byte counter for BC++
  → increaseRate() based on (T, BC) vs F=5 threshold
```
Effective response time: **55µs timer period**, independent of RTT. Decrease is CNP-driven (50µs minimum interval).

### 9.8 Why Proportional? The Mathematical Argument

The proportional increase and decrease formulas in §3 may feel like they were pulled from a hat. This section shows they are the *simplest* functions with the right properties — each can be derived by listing what we need and ruling out alternatives.

#### Proportional Increase: ΔW ∝ (target − delay)

We want an increase function `f(d)` where `d` is the measured queuing delay and `target` is the operating point. Requirements:

1. **f(0) > 0** — when the network is empty, we should grow.
2. **f(target) = 0** — at the operating point, growth stops naturally.
3. **f is monotonically decreasing** — more queuing → less growth.

The simplest function satisfying all three is the **linear ramp**:

```
ΔW = α × acked_bytes × (target − d)        for d < target
```

Why not a constant? A constant `f(d) = c` violates requirement 2 — it never tapers off, so the window overshoots the target every time. Why not quadratic? `f(d) = c × (target − d)²` satisfies all three requirements but is too conservative near `d = 0`: it wastes capacity when the network is nearly empty because the slope is flat at the origin. The linear ramp is the **Goldilocks curve** — maximum slope at `d = 0` (fill fast when empty), zero crossing at `d = target` (stop naturally at equilibrium), and uniform taper in between.

#### Proportional Decrease: severity factor s = (d − t) / d

For multiplicative decrease, we need a **severity factor** `s` that controls how much of the window we cut:

```
W_new = W × (1 − γ × s)
```

Requirements for `s`:

1. **s = 0 when d = target** — no cut at the boundary.
2. **s → 1 as d → ∞** — maximum cut under extreme congestion.
3. **0 ≤ s ≤ 1** — the cut fraction is always bounded.

Define `s = (d − t) / d` where `d` is the measured delay and `t` is the target. Check:

- At `d = t`: `s = 0` ✓
- As `d → ∞`: `s → 1` ✓
- For `d ≥ t`: `s ∈ [0, 1)` ✓

The alternative `s' = (d − t) / t` satisfies requirement 1 but **violates** requirement 3 — at `d = 10t`, `s' = 9`, which is unbounded. You'd need an external clamp. With `s = (d − t) / d`, the boundedness is *structural*: it's the fraction of the measured delay that is excess queuing, and a fraction can never exceed 1.

Plugging back in: `W_new = W × max(1 − γ × (d − t) / d, 0.5)`. This is exactly the formula in §3 (`uec.cpp:1255`). The 0.5 floor is an engineering guard (never halve more than once per RTT), but the core shape — the `(d − t) / d` curve — is *derived* from the three requirements above, not chosen arbitrarily.

#### The Connection

Together, these two functions give NSCC a **symmetric proportional** character:

| Direction | Formula | Key property |
|-----------|---------|--------------|
| Increase | `ΔW ∝ (target − delay)` | Linear approach to equilibrium from below |
| Decrease | `cut ∝ (delay − target) / delay` | Bounded retreat from equilibrium from above |

Both are zero at the target, both scale with distance from it, and both are the simplest functions with the right mathematical properties. This is why the control loop converges smoothly rather than oscillating — the actuator strength vanishes as the system approaches equilibrium.

#### Simulation: NSCC vs TCP Cubic Coexistence

The figures below show bandwidth share and Jain's Fairness Index across three traffic scenarios as the NSCC/Cubic ratio varies from 0% to 100%.

![NSCC vs Cubic Coexistence](figures/sim_coexistence.png)

![Traffic Pattern Comparison](figures/sim_traffic_pattern.png)

---

## 10. Intuition Summary

### What We Discovered

Walking through this document, we arrived at each piece of NSCC by asking *what we need* and *what goes wrong* with simpler approaches:

1. **Single-signal CC fails with spraying** → we need two signals (delay + ECN) → four quadrants emerge naturally
2. **Constant increase overshoots** → proportional increase to headroom → linear ramp `alpha × (target - delay)`
3. **Fixed decrease overreacts** → proportional decrease to severity → `gamma × (delay - target) / delay` with 50% floor
4. **Per-ACK updates oscillate** → batched fulfill adjustment → normalized by cwnd for fairness
5. **Fast filters thrash on path noise** → slow EWMA (α=0.0125) → ~80 samples to steady state
6. **Iterative decrease too slow for incast** → Quick Adapt resets to achieved throughput → 1-RTT convergence
7. **Fixed parameters don't scale** → BDP-ratio scaling → same behavior at any network speed
8. **3-dupACK fails with reordering** → SLEEK with cwnd-scaled threshold → tolerates spraying-induced reorder

### Anatomy of One ACK

To tie all the sections together, let's trace a single ACK through the entire system:

```
  A single ACK arrives. Here's everything that happens:

  1. [§4] Raw RTT measured. Queuing delay = RTT - base_RTT.
     The delay sample is fed into the EWMA filter (slow update).
  2. [§5] Quick Adapt check: is achieved_bytes < maxwnd/8? No → continue.
  3. [§2] Quadrant decision using RAW delay + ECN flag → proportional_increase.
  4. [§3] _inc_bytes += alpha × acked × (target - delay). cwnd NOT changed yet.
  5. [§3] received_bytes now exceeds 8×MTU → fulfill fires:
         cwnd += _inc_bytes / cwnd + eta.
  6. [§3] Bounds check: cwnd stays within [1 MTU, 1.5×BDP].
  7. [§6] Multipath engine receives PATH_GOOD feedback → path score unchanged.
```

Total time: nanoseconds. One ACK, seven subsystems, one coherent response. The dual-delay architecture (§4) ensures the quadrant was chosen by this packet's experience while any decrease magnitude would reflect the filtered history. The fulfill batching (§3) ensures the increase was normalized for fairness. The multipath feedback (§6) ensures the path selection adapts independently. Every section in this document describes one stage of this pipeline.

### When Does NSCC Shine?

- **Multi-path datacenter fabrics**: The four-quadrant design with NOOP for (ECN, low-delay) is specifically designed for packet spraying where per-path congestion shouldn't reduce the global window.
- **Shallow-buffered networks**: The delay-based signal is precise when buffers are small and base RTT is well-known.
- **Mixed traffic patterns**: Quick Adapt handles sudden incast, while proportional increase efficiently fills capacity for long flows.
- **Fairness-critical workloads**: The scaling mechanism and fair_increase mode achieve near-perfect fairness between competing flows.

### When Does NSCC Struggle?

- **Single-path networks**: The NOOP quadrant becomes counterproductive—ECN should always trigger a response on a single path.
- **Unknown or variable base RTT**: The entire delay measurement system depends on an accurate base RTT. If propagation delay changes (e.g., route changes), stale base RTT causes miscalculation.
- **Very deep buffers**: With large buffers, delay can grow far above target before ECN marks appear, making the four-quadrant classification less effective.

### Key Design Trade-offs

| Trade-off | Choice | What It Optimizes |
|-----------|--------|-------------------|
| Window vs Rate | Window | Simplicity, self-clocking |
| Per-ACK vs Batched increase | Batched (fulfill) | Stability, reduced oscillation |
| Immediate vs Batched decrease | Immediate | Fast congestion response |
| Fixed vs Proportional decrease | Proportional to delay | Avoids overreaction |
| Single vs Multi-signal | Delay + ECN (two signals) | Disambiguation of per-path vs global congestion |

### The Smoke Detector vs the Thermometer

The single most useful mental model for NSCC's dual-signal design:

- **ECN = smoke detector.** Fast, binary, sometimes path-local. It tells you *something is on fire* but not how big the fire is or whether it's in your room or three rooms away.
- **RTT = thermometer.** Slow, multi-bit, tells severity. It tells you *how hot the building is* but can't pinpoint where the heat is coming from, and it lags behind reality.

Neither signal alone gives you the full picture. Together, they produce five actionable rules:

| ECN | Delay | Interpretation | Action |
|-----|-------|----------------|--------|
| None | Low | Both fine — building is cool and quiet | **Push up** (proportional increase) |
| Yes | High | Both bad — fire is real and spreading | **Cut hard** (multiplicative decrease) |
| Yes | Low | Smoke but no heat — probably one room | **Steer first** (NOOP + LB reroute) |
| None | High | Heat but no smoke — cooling down | **Creep up** (fair increase) |
| — | Extreme | Off the charts | **QA reset** (emergency brake) |

**Signal disagreement is not confusion — it's a state indicator.** When ECN and delay disagree, the disagreement tells you whether congestion is building (leading indicator fired, trailing hasn't caught up) or clearing (trailing is still elevated, leading has gone quiet). This is the core insight behind the four-quadrant design.

### The "Feel" of NSCC

The highway analogy offers a complementary perspective — how NSCC *feels* from the sender's point of view:

- **Road is empty** (fast_increase): Floor it. Add capacity at `0.25 * scaling_factor_a` per ACK.
- **Road is filling but clear** (proportional_increase): Accelerate proportionally to remaining headroom. The closer to the target delay, the more gently we increase.
- **Road is busy but no one's honking** (fair_increase): Maintain speed with gentle acceleration. RTT is elevated but ECN marks have stopped — traffic is clearing. Increase gently.
- **Someone's honking on another lane** (NOOP): Ignore it—just avoid that lane next time. Multi-path means per-path congestion doesn't affect global behavior.
- **Everyone's braking** (multiplicative_decrease): Brake proportionally to how congested it is. The worse the congestion, the harder we brake, but never more than 50%.
- **Gridlock detected** (Quick Adapt): Emergency stop. Reset to what we actually achieved, ignoring stale feedback from packets still in the pipeline.

### Exercises

Test your understanding of the concepts in this document:

**Exercise 1 — BDP Intuition.**
A 400 Gbps link has a base RTT of 10 µs. (a) Compute the BDP in bytes and in MTU-sized packets (MTU = 4 KB). (b) If three senders each maintain a window of 1 × BDP, what is the total queuing at the bottleneck switch in steady state? Express your answer in microseconds of queuing delay. (c) At what window size does queuing delay equal the NSCC default target of 0.75 × RTT?

**Exercise 2 — Quadrant Reasoning.**
For each scenario below, identify which quadrant the sender enters and what action it takes. Explain whether the load balancer or the congestion controller is the primary responder.
- (a) A single ECMP hash collision on Path 5 out of 16 paths. All other paths are clean.
- (b) A 32-to-1 incast where every path shows ECN marks and delay is 3× target.
- (c) A burst of congestion has just cleared: ECN marks have stopped but the EWMA-filtered delay is still 1.2× target.
- (d) A routing change adds 2 µs of propagation delay to every path simultaneously.

**Exercise 3 — Quick Adapt Convergence.**
Consider a 64-sender incast to a single receiver on a 100 Gbps fabric (BDP = 150 KB). (a) How many RTTs does multiplicative decrease (50% per RTT) take to reduce a single flow's window from 150 KB to the fair share of ~2.3 KB? (b) Quick Adapt resets to `achieved_bytes` in one evaluation window. If the flow achieves 2 KB in one QA period, how does this compare to the iterative approach? (c) Why does QA require the `achieved_bytes < maxwnd/8` guard — what would go wrong without it?

**Exercise 4 — Compare Designs.**
Fill in the following table for each protocol. Use the *Knob + Signal + Assumptions* framework from §9.

| | NSCC | TCP Cubic | DCQCN | DCTCP | Swift |
|---|---|---|---|---|---|
| **Primary signal** | | | | | |
| **Main knob** | | | | | |
| **Fairness definition** | | | | | |
| **Best-fit deployment** | | | | | |

For each, write one sentence explaining why the signal choice leads to the fairness behavior.

---

## 11. Simulation Validation

This section presents results from actual htsim simulations that validate the algorithm concepts discussed throughout this document. All figures are generated from the `NSCC_DEEP_DIVE.ipynb` notebook using data collected by `run_nscc_experiments.py`.

### Phase 1: Aggregate Results

#### Fairness Convergence (§3)

Jain's Fairness Index remains near-perfect (>0.99) across flow counts from 8 to 128, validating the fulfill normalization mechanism (`inc_bytes / cwnd`).

![Fairness Convergence](figures/sim_fairness.png)

#### QA Gate Sensitivity (§5)

Under 32-to-1 incast, `qa_gate=3` (threshold = 12.5% of maxwnd) provides the best balance between completion rate and tail latency. Too aggressive (qa_gate=0) causes unnecessary resets; too conservative (qa_gate=4) misses severe incast.

![QA Gate Sensitivity](figures/sim_qa_gate.png)

#### Incast Degree Scaling (§5)

As incast fan-in increases from 8 to 64, Quick Adapt becomes increasingly important. Aggregate throughput at the bottleneck receiver scales well, while tail FCT degrades gracefully.

![Incast Degree Scaling](figures/sim_incast_degree.png)

#### Target Delay Sensitivity (§4/§7)

Sweeping `target_q_delay` from 3µs to 9µs reveals the classic throughput-latency tradeoff. Lower targets provide tighter latency but may sacrifice throughput under high load.

![Target Delay Sensitivity](figures/sim_target_delay.png)

#### NSCC vs TCP Cubic Coexistence (§9)

Three traffic scenarios (permutation, mixed sizes, incast+background) with `nscc_ratio` swept from 0% to 100%. NSCC achieves fair bandwidth sharing with TCP Cubic when both protocols compete on the same network.

![NSCC vs Cubic Coexistence](figures/sim_coexistence.png)

#### Traffic Pattern Comparison (§10)

NSCC handles diverse traffic patterns effectively: permutation (uniform load), incast (concentrated), and mixed (varied sizes).

![Traffic Pattern Comparison](figures/sim_traffic_pattern.png)

### Phase 2: Time-Series Traces

These figures use the `NsccTraceLogger` to capture per-flow, per-fulfill-period data including cwnd, delay, and quadrant decisions.

#### Quadrant Decisions in Action (§2)

Each dot represents one fulfill period for one flow, colored by the quadrant decision taken. Shows how flows transition between quadrants as congestion evolves.

![Quadrant Time-Series](figures/sim_quadrant_timeseries.png)

#### CWND Evolution & Convergence (§3)

16 flows starting with different initial conditions converge to fair share through the fulfill normalization mechanism. BDP and maxwnd reference lines show the operating bounds.

![CWND Evolution](figures/sim_cwnd_evolution.png)

#### Delay Filtering (§4)

Raw per-path delay vs EWMA-smoothed `avg_delay` for a single flow. Demonstrates how the slow filter (α=0.0125) smooths out per-path noise while tracking sustained congestion.

![Delay Traces](figures/sim_delay_traces.png)

#### Quick Adapt Firing (§5)

During 64-to-1 incast, QA resets (triangles) bring cwnd down to `achieved_bytes` in a single RTT. The quadrant histogram shows how the decision mix shifts as congestion develops and resolves.

![QA Firing](figures/sim_qa_firing.png)

![Quadrant Histogram](figures/sim_quadrant_histogram.png)

#### NSCC vs Cubic cwnd Under Shared Bottleneck (§9)

Side-by-side cwnd traces for NSCC flows in a 50/50 mix with TCP Cubic, showing the quadrant distribution over time.

![NSCC vs Cubic cwnd](figures/sim_cwnd_nscc_vs_cubic.png)

### How to Reproduce

1. **Build the simulator:**
   ```bash
   cd build && cmake .. && make htsim_uec htsim_mixed -j$(nproc)
   ```

2. **Run all experiments:**
   ```bash
   cd htsim/sim/datacenter
   python3 run_nscc_experiments.py --experiments all
   ```

3. **Generate figures:**
   Open `NSCC_DEEP_DIVE.ipynb` in Jupyter and run all cells. Figures are saved to `figures/`.

4. **Run specific experiments:**
   ```bash
   # Phase 1 only (aggregate results)
   python3 run_nscc_experiments.py -e fairness,qa_gate,incast_degree,target_delay,coexistence,traffic_pattern

   # Phase 2 only (time-series traces)
   python3 run_nscc_experiments.py -e trace_quadrant,trace_cwnd,trace_delay,trace_qa,trace_coexist
   ```

| Experiment | Binary | Traffic | Key Parameter | Section |
|---|---|---|---|---|
| Fairness vs flow count | `htsim_uec` | Permutation | N=8..128 flows | §3 |
| QA gate sensitivity | `htsim_uec` | 32-to-1 incast | `qa_gate` 0..4 | §5 |
| Incast degree | `htsim_uec` | N-to-1 incast | N=8..64 | §5 |
| Target delay | `htsim_uec` | Permutation 128n | `target_q_delay` 3..9µs | §4/§7 |
| NSCC vs Cubic | `htsim_mixed` | Scenarios A,B,C | `nscc_ratio` 0..100% | §9 |
| Traffic patterns | `htsim_uec` | Perm/incast/mixed | Pattern type | §10 |

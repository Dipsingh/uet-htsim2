// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * TCP CUBIC congestion control implementation
 * Based on Linux kernel implementation (net/ipv4/tcp_cubic.c)
 * Reference: RFC 8312 - CUBIC for Fast Long-Distance Networks
 *
 * Key algorithm:
 * W_cubic(t) = C*(t-K)^3 + W_max
 * where K = cbrt(W_max*beta/C), C = 0.4, beta = 0.7
 */

#include "tcpcubic.h"
#include "ecn.h"
#include "mtcp.h"
#include <algorithm>

using namespace std;

// Cube root lookup table from Linux kernel (v^(1/3) * 256 for 0 <= v < 256)
const uint8_t TcpCubicSrc::_cube_root_table[256] = {
    0,   54,  54,  54,  118, 118, 118, 118, 123, 129, 134, 138, 143, 147, 151, 156,
    157, 161, 164, 168, 170, 173, 176, 179, 181, 185, 187, 190, 192, 194, 197, 199,
    200, 202, 204, 206, 209, 211, 213, 215, 217, 219, 221, 222, 224, 225, 227, 229,
    231, 232, 234, 236, 237, 239, 240, 242, 244, 245, 246, 248, 250, 251, 252, 254,
    255, 0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,
    15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,
    31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,
    47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,
    63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,
    79,  80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,
    95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126,
    127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
    143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158,
    159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174,
    175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190
};

TcpCubicSrc::TcpCubicSrc(TcpLogger* logger, TrafficLogger* pktlogger,
                         EventList &eventlist)
    : TcpSrc(logger, pktlogger, eventlist)
{
    // Initialize CUBIC state
    _last_max_cwnd = 0;
    _bic_origin_point = 0;
    _epoch_start = 0;
    _bic_K = 0;
    _delay_min = 0;

    // TCP-friendly state
    _tcp_cwnd = 0;
    _ack_cnt = 0;
    _cnt = 0;

    // HyStart state
    _hystart_enabled = true;
    _round_start = 0;
    _last_ack_time = 0;
    _curr_rtt = 0;
    _sample_cnt = 0;
    _found_slow_start_exit = false;
    _end_seq = 0;
    _delay_min_sample = 0;

    // Feature flags (Linux defaults)
    _tcp_friendliness = true;
    _fast_convergence = true;
}

/*
 * Integer cube root using lookup table and Newton-Raphson
 * This is a direct port from Linux kernel's tcp_cubic.c
 */
uint32_t TcpCubicSrc::cubic_root(uint64_t a)
{
    uint32_t x, b, shift;

    if (a == 0)
        return 0;

    // Find the highest bit position
    // Equivalent to fls64(a) in Linux
    b = 0;
    uint64_t tmp = a;
    while (tmp > 0) {
        tmp >>= 1;
        b++;
    }

    // Normalize to fit in 8 bits for table lookup
    // We want (b + 2) / 3 * 3 - b shift bits
    if (b < 7) {
        // Small number, can use table directly after shifting
        shift = (3 * 6 - b * 2);
        x = ((uint32_t)_cube_root_table[(uint32_t)(a << shift >> 6)] + 10) >> shift;
    } else {
        shift = b - 6;
        // Initial estimate from table
        uint64_t shifted = a >> (shift * 3);
        if (shifted >= 256)
            shifted = 255;
        x = (((uint32_t)_cube_root_table[(uint32_t)shifted] + 10) << shift);
    }

    // Newton-Raphson iterations
    // x = (2*x + a/x^2) / 3
    x = (2 * x + (uint32_t)(a / ((uint64_t)x * (uint64_t)x))) / 3;
    x = (2 * x + (uint32_t)(a / ((uint64_t)x * (uint64_t)x))) / 3;

    return x;
}

/*
 * Reset CUBIC state - called at the start of a connection
 */
void TcpCubicSrc::bictcp_reset()
{
    _last_max_cwnd = 0;
    _bic_origin_point = 0;
    _epoch_start = 0;
    _bic_K = 0;
    _tcp_cwnd = 0;
    _ack_cnt = 0;
    _cnt = 0;
}

/*
 * Reset HyStart state for a new round
 */
void TcpCubicSrc::hystart_reset()
{
    _round_start = 0;
    _last_ack_time = 0;
    _curr_rtt = 0;
    _sample_cnt = 0;
}

/*
 * Combined reset for connection start
 */
void TcpCubicSrc::bictcp_hystart_reset()
{
    bictcp_reset();
    hystart_reset();
    _found_slow_start_exit = false;
}

/*
 * TCP-friendly region - ensure CUBIC is at least as aggressive as Reno
 * Uses the AIMD formula: W_tcp(t) = W_max*(1-beta) + 3*beta/(2-beta) * t/RTT
 */
void TcpCubicSrc::tcp_friendliness_update(uint32_t cwnd)
{
    // Calculate what Reno would have
    // tcp_cwnd tracks the Reno cwnd estimate
    // For each RTT, Reno increases by approximately mss^2/cwnd

    // Every ack_cnt acks, we should have increased by one mss in Reno
    // ack_cnt is reset when cwnd increases

    uint32_t delta;

    // tcp_cwnd in MSS units for easier calculation
    uint32_t cwnd_mss = cwnd / _mss;
    uint32_t tcp_cwnd_mss = _tcp_cwnd / _mss;

    // Calculate how much Reno would have grown
    // Reno: cwnd += mss for every cwnd/mss acks
    // So for _ack_cnt acks: increase = _ack_cnt * mss / cwnd ≈ _ack_cnt / cwnd_mss

    delta = (cwnd_mss * BICTCP_BETA_SCALE) / (3 * (BICTCP_BETA_SCALE - BETA));

    // If we're below Reno's cwnd, use Reno's increase
    if (tcp_cwnd_mss <= cwnd_mss) {
        // Update tcp_cwnd based on acks received
        tcp_cwnd_mss += _ack_cnt / cwnd_mss;
        _tcp_cwnd = tcp_cwnd_mss * _mss;
    }

    if (tcp_cwnd_mss > cwnd_mss) {
        // TCP-friendly region: use Reno's increment
        uint32_t max_cnt = cwnd / (tcp_cwnd_mss - cwnd_mss);
        if (_cnt > max_cnt)
            _cnt = max_cnt;
    }
}

/*
 * Main CUBIC cwnd update function
 * Calculates the CUBIC target and updates _cnt (the increment threshold)
 */
void TcpCubicSrc::bictcp_update(uint32_t cwnd, uint32_t acked)
{
    uint32_t delta, bic_target, max_cnt;
    uint64_t offs, t;

    _ack_cnt += acked;

    // If this is the start of an epoch, initialize
    if (_epoch_start == 0) {
        _epoch_start = eventlist().now();
        _ack_cnt = acked;
        _tcp_cwnd = cwnd;

        if (_last_max_cwnd <= cwnd) {
            // Previous loss was smaller than current cwnd
            // Start from current cwnd
            _bic_K = 0;
            _bic_origin_point = cwnd;
        } else {
            // Calculate K = cbrt(W_max - cwnd) * scaling
            // K is the time in units of (RTT/C^(1/3)) to reach W_max
            // In Linux: K = cubic_root(cube_rtt_scale * (Wmax - cwnd))
            // cube_rtt_scale = bic_scale * 10 = 41 * 10 = 410

            // K calculation using integer math:
            // K = cbrt((W_max - cwnd) * BIC_SCALE * 10 / (RTT_in_us/1000000))
            // But we use BICTCP_HZ = 10 (100ms units)

            // In mss units for stability
            uint32_t cwnd_mss = cwnd / _mss;
            uint32_t last_max_mss = _last_max_cwnd / _mss;

            // K = cbrt((Wmax - cwnd) / C) where C = 0.4
            // With scaling: K = cbrt((Wmax - cwnd) * BIC_SCALE)
            _bic_K = cubic_root((uint64_t)(last_max_mss - cwnd_mss) * BIC_SCALE);
            _bic_origin_point = _last_max_cwnd;
        }
    }

    // Calculate t = time since epoch start + min_RTT (in 100ms units like Linux)
    // t needs to account for the delay_min to be in sync with actual network
    simtime_picosec now = eventlist().now();
    simtime_picosec time_since_epoch = now - _epoch_start;

    // Add delay_min to t (this accounts for min RTT)
    if (_delay_min > 0) {
        time_since_epoch += _delay_min;
    }

    // Convert to BICTCP_HZ units (100ms units, so divide by 100ms in picoseconds)
    // 100ms = 100,000 us = 100,000,000,000 ps
    t = time_since_epoch / timeFromMs(100);

    // Calculate offs = |t - K|
    if (t < _bic_K) {
        offs = _bic_K - t;
    } else {
        offs = t - _bic_K;
    }

    // Calculate delta = C * offs^3 (in MSS units)
    // delta = (offs^3 * BIC_SCALE) / 10
    // But we're working in bytes, so multiply back
    delta = (uint32_t)((offs * offs * offs * BIC_SCALE) / 10);
    delta *= _mss; // Convert to bytes

    // Calculate bic_target = origin_point ± delta
    if (t < _bic_K) {
        // Before reaching W_max, target is origin - delta
        bic_target = (_bic_origin_point > delta) ?
                     _bic_origin_point - delta : 0;
    } else {
        // After K, target is origin + delta
        bic_target = _bic_origin_point + delta;
    }

    // Make sure target is at least cwnd
    if (bic_target > cwnd) {
        // Calculate cnt = cwnd / (bic_target - cwnd)
        // This is how many acks before we increase cwnd
        _cnt = cwnd / (bic_target - cwnd);
    } else {
        // In concave region close to W_max, be more aggressive
        // Use smaller cnt for faster convergence
        _cnt = 100 * cwnd;
    }

    // Apply TCP-friendliness
    if (_tcp_friendliness) {
        tcp_friendliness_update(cwnd);
    }
}

/*
 * HyStart - Hybrid Slow Start
 * Detects when to exit slow start using delay increase detection
 */
void TcpCubicSrc::hystart_update(simtime_picosec rtt)
{
    if (!_hystart_enabled || _found_slow_start_exit)
        return;

    // Only run HyStart when cwnd is large enough
    if (_cwnd < HYSTART_LOW_WINDOW * _mss)
        return;

    // Start of new round?
    if (_highest_sent > _end_seq) {
        _end_seq = _highest_sent;
        _round_start = eventlist().now();
        _sample_cnt = 0;
        _delay_min_sample = 0;
    }

    // Convert RTT to microseconds for comparison
    uint32_t rtt_us = (uint32_t)timeAsUs(rtt);
    uint32_t delay_min_us = (_delay_min > 0) ? (uint32_t)timeAsUs(_delay_min) : rtt_us;

    // Track minimum RTT in this round
    if (_delay_min_sample == 0 || rtt < _delay_min_sample) {
        _delay_min_sample = rtt;
    }

    _sample_cnt++;

    // Need enough samples before making a decision
    if (_sample_cnt < HYSTART_MIN_SAMPLES)
        return;

    // Delay-based detection:
    // Exit slow start if RTT increased significantly
    uint32_t thresh = delay_min_us >> HYSTART_DELAY_THRESH_SHIFT; // delay_min / 8

    // Clamp threshold between 4ms and 16ms
    uint32_t min_thresh = HYSTART_DELAY_MIN_US;
    uint32_t max_thresh = HYSTART_DELAY_MAX_US;

    if (thresh < min_thresh)
        thresh = min_thresh;
    if (thresh > max_thresh)
        thresh = max_thresh;

    // If current RTT exceeds min + threshold, exit slow start
    if (rtt_us > delay_min_us + thresh) {
        _found_slow_start_exit = true;
        _ssthresh = _cwnd;
        // cout << "HyStart: Exiting slow start at cwnd=" << _cwnd/_mss
        //      << " mss, rtt=" << rtt_us << "us, min_rtt=" << delay_min_us << "us" << endl;
    }
}

/*
 * inflate_window - called on each ACK to increase cwnd
 * Overrides TcpSrc::inflate_window
 */
void TcpCubicSrc::inflate_window()
{
    // Update minimum RTT
    if (_rtt > 0) {
        if (_delay_min == 0 || _rtt < _delay_min) {
            _delay_min = _rtt;
        }
    }

    // Slow start
    if (_cwnd < _ssthresh) {
        // Exponential increase in slow start
        uint32_t increase = min(_ssthresh - _cwnd, (uint32_t)_mss);
        _cwnd += increase;

        // HyStart detection during slow start
        if (_hystart_enabled && _rtt > 0) {
            hystart_update(_rtt);
        }
        return;
    }

    // Congestion avoidance - CUBIC algorithm
    bictcp_update(_cwnd, 1);

    // Increase cwnd when we've received enough acks
    // cnt is the number of acks before incrementing cwnd by mss
    if (_ack_cnt > _cnt) {
        _cwnd += _mss;
        _ack_cnt = 0;
    }
}

/*
 * deflate_window - called on loss detection
 * Overrides TcpSrc::deflate_window
 */
void TcpCubicSrc::deflate_window()
{
    // Start a new epoch
    _epoch_start = 0;

    // Fast convergence: if we're losing before reaching last_max,
    // the network has become more congested, so be more conservative
    if (_fast_convergence && _cwnd < _last_max_cwnd) {
        // Further reduce last_max by beta
        _last_max_cwnd = (_cwnd * (BICTCP_BETA_SCALE + BETA)) / (2 * BICTCP_BETA_SCALE);
    } else {
        _last_max_cwnd = _cwnd;
    }

    // Multiplicative decrease: ssthresh = cwnd * beta
    // beta = 0.7 (717/1024)
    if (_mSrc == NULL) {
        _ssthresh = max((_cwnd * BETA) / BICTCP_BETA_SCALE, (uint32_t)(2 * _mss));
    } else {
        _ssthresh = _mSrc->deflate_window(_cwnd, _mss);
    }

    // Reset HyStart for next time we enter slow start
    hystart_reset();
}

/*
 * receivePacket - handle incoming ACKs
 * Overrides to add ECN handling for CUBIC
 */
void TcpCubicSrc::receivePacket(Packet& pkt)
{
    // Check for ECN marking
    if (pkt.flags() & ECN_ECHO) {
        // ECN-triggered congestion event
        // Only react once per RTT (check if we haven't already reacted)
        if (_cwnd > _ssthresh) {
            // Treat ECN like a loss event
            deflate_window();
            _cwnd = _ssthresh;
        }
    }

    // Call base class to handle the rest
    TcpSrc::receivePacket(pkt);
}

/*
 * rtx_timer_hook - handle RTO timeouts
 * Overrides to reset CUBIC state on timeout
 */
void TcpCubicSrc::rtx_timer_hook(simtime_picosec now, simtime_picosec period)
{
    // Check if RTO has occurred
    if (now > _RFC2988_RTO_timeout && _RFC2988_RTO_timeout != timeInf) {
        // Reset CUBIC state on timeout
        _epoch_start = 0;
        _last_max_cwnd = max(_cwnd, (uint32_t)(2 * _mss));
    }

    // Call base class
    TcpSrc::rtx_timer_hook(now, period);
}

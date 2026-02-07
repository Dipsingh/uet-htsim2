// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef TCPCUBIC_H
#define TCPCUBIC_H

/*
 * TCP CUBIC congestion control implementation
 * Based on Linux kernel implementation (net/ipv4/tcp_cubic.c)
 * Reference: RFC 8312 - CUBIC for Fast Long-Distance Networks
 */

#include "tcp.h"

class TcpCubicSrc : public TcpSrc {
public:
    TcpCubicSrc(TcpLogger* logger, TrafficLogger* pktlogger, EventList &eventlist);
    virtual ~TcpCubicSrc() {}

    // Override methods from TcpSrc
    virtual void inflate_window() override;
    virtual void deflate_window() override;
    virtual void receivePacket(Packet& pkt) override;
    virtual void rtx_timer_hook(simtime_picosec now, simtime_picosec period) override;

    // Enable/disable features
    void setHystartEnabled(bool enabled) { _hystart_enabled = enabled; }
    void setTcpFriendlinessEnabled(bool enabled) { _tcp_friendliness = enabled; }
    void setFastConvergenceEnabled(bool enabled) { _fast_convergence = enabled; }
    void setEcnEnabled(bool enabled) { _ecn_enabled = enabled; }

protected:
    // Reset CUBIC + HyStart state for reuse (e.g., transfer mode)
    void bictcp_hystart_reset();

private:
    // CUBIC state (matching Linux kernel)
    uint32_t _last_max_cwnd;      // W_max: window at last loss (in bytes)
    uint32_t _bic_origin_point;   // Origin point of cubic function (bytes)
    simtime_picosec _epoch_start; // Start of current epoch
    uint32_t _bic_K;              // Time to reach W_max (in BICTCP_HZ units)
    simtime_picosec _delay_min;   // Minimum RTT observed

    // TCP-friendly mode
    uint32_t _tcp_cwnd;           // Estimated Reno cwnd (bytes)
    uint32_t _ack_cnt;            // ACK counter for cwnd increase
    uint32_t _cnt;                // cwnd increase count threshold

    // HyStart state
    bool _hystart_enabled;
    simtime_picosec _round_start; // Start of current RTT round
    simtime_picosec _last_ack_time; // Time of last ACK in train
    uint32_t _curr_rtt;           // Current RTT sample (in us for comparison)
    uint32_t _sample_cnt;         // RTT sample count in round
    bool _found_slow_start_exit;  // HyStart triggered flag
    uint32_t _end_seq;            // Sequence number at round end
    simtime_picosec _delay_min_sample; // Min RTT in current round

    // Feature flags
    bool _tcp_friendliness;
    bool _fast_convergence;
    bool _ecn_enabled;

    // ECN per-RTT guard: don't react until ACKs pass this seqno
    uint64_t _ecn_next_seq;

    // Constants from Linux kernel (using default values)
    // BICTCP_BETA_SCALE = 1024, beta = 717 (0.7 * 1024)
    static constexpr uint32_t BICTCP_BETA_SCALE = 1024;
    static constexpr uint32_t BICTCP_HZ = 10;        // BIC HZ (100ms intervals)
    static constexpr uint32_t BIC_SCALE = 41;        // C = 0.4 scaled by BIC_SCALE
    static constexpr uint32_t BETA = 717;            // 717/1024 ≈ 0.7

    // HyStart parameters
    static constexpr uint32_t HYSTART_LOW_WINDOW = 16; // Minimum cwnd for HyStart
    static constexpr uint32_t HYSTART_MIN_SAMPLES = 8; // Minimum RTT samples
    static constexpr uint32_t HYSTART_ACK_DELTA_US = 2000; // 2ms in microseconds
    static constexpr uint32_t HYSTART_DELAY_MIN_US = 4000; // 4ms in microseconds
    static constexpr uint32_t HYSTART_DELAY_MAX_US = 16000; // 16ms in microseconds
    static constexpr uint32_t HYSTART_DELAY_THRESH_SHIFT = 3; // Divide by 8

    // Cube root lookup table (matches Linux kernel)
    static const uint8_t _cube_root_table[256];

    // Helper methods
    uint32_t cubic_root(uint64_t a);
    void bictcp_update(uint32_t cwnd, uint32_t acked);
    void tcp_friendliness_update(uint32_t cwnd);
    void hystart_update(simtime_picosec rtt);
    void hystart_reset();
    void bictcp_reset();
};

#endif // TCPCUBIC_H

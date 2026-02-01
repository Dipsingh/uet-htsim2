// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef NSCC_TRACE_LOGGER_H
#define NSCC_TRACE_LOGGER_H

#include <fstream>
#include <string>
#include <cstdint>
#include "network.h"

/**
 * Lightweight CSV trace logger for NSCC time-series analysis.
 *
 * Records one row per fulfill period per flow, capturing cwnd evolution,
 * delay filtering, quadrant decisions, and QA events. For 128 flows over
 * 500us this produces ~5000 rows — negligible overhead.
 *
 * Quadrant encoding:
 *   0 = no-trim, delay >= target  (fair increase)
 *   1 = no-trim, delay <  target  (proportional increase)
 *   2 = trim,    delay >= target  (multiplicative decrease)
 *   3 = trim,    delay <  target  (noop / switch path)
 *   5 = quick-adapt fired
 */
class NsccTraceLogger {
public:
    NsccTraceLogger(const std::string& filepath);
    ~NsccTraceLogger();

    bool is_open() const { return _file.is_open(); }

    void logSample(simtime_picosec time,
                   flowid_t flow_id,
                   mem_b cwnd,
                   mem_b in_flight,
                   mem_b bdp,
                   mem_b maxwnd,
                   simtime_picosec avg_delay,
                   simtime_picosec raw_delay,
                   simtime_picosec target_delay,
                   simtime_picosec base_rtt,
                   bool ecn,
                   uint8_t quadrant,
                   mem_b inc_fair,
                   mem_b inc_prop,
                   mem_b inc_fast,
                   mem_b inc_eta,
                   mem_b dec_multi,
                   mem_b dec_quick);

    void logQAEvent(simtime_picosec time,
                    flowid_t flow_id,
                    mem_b cwnd_before,
                    mem_b cwnd_after,
                    mem_b achieved_bytes,
                    mem_b in_flight);

private:
    std::ofstream _file;
};

#endif // NSCC_TRACE_LOGGER_H

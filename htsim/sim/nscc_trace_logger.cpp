// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "nscc_trace_logger.h"
#include <iostream>

NsccTraceLogger::NsccTraceLogger(const std::string& filepath) {
    _file.open(filepath);
    if (!_file.is_open()) {
        std::cerr << "NsccTraceLogger: failed to open " << filepath << std::endl;
        return;
    }
    // Write CSV header
    _file << "time_us,flow_id,cwnd,in_flight,bdp,maxwnd,"
          << "avg_delay_us,raw_delay_us,target_us,base_rtt_us,"
          << "ecn,quadrant,"
          << "inc_fair,inc_prop,inc_fast,inc_eta,dec_multi,dec_quick"
          << std::endl;
}

NsccTraceLogger::~NsccTraceLogger() {
    if (_file.is_open()) {
        _file.close();
    }
}

void NsccTraceLogger::logSample(simtime_picosec time,
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
                                mem_b dec_quick) {
    if (!_file.is_open()) return;

    _file << timeAsUs(time) << ","
          << flow_id << ","
          << cwnd << ","
          << in_flight << ","
          << bdp << ","
          << maxwnd << ","
          << timeAsUs(avg_delay) << ","
          << timeAsUs(raw_delay) << ","
          << timeAsUs(target_delay) << ","
          << timeAsUs(base_rtt) << ","
          << (ecn ? 1 : 0) << ","
          << (int)quadrant << ","
          << inc_fair << ","
          << inc_prop << ","
          << inc_fast << ","
          << inc_eta << ","
          << dec_multi << ","
          << dec_quick
          << "\n";  // use \n not endl to avoid flush overhead
}

void NsccTraceLogger::logQAEvent(simtime_picosec time,
                                 flowid_t flow_id,
                                 mem_b cwnd_before,
                                 mem_b cwnd_after,
                                 mem_b achieved_bytes,
                                 mem_b in_flight) {
    if (!_file.is_open()) return;

    // QA events use quadrant=5 and store cwnd_before/cwnd_after in inc_fair/inc_prop
    _file << timeAsUs(time) << ","
          << flow_id << ","
          << cwnd_after << ","
          << in_flight << ","
          << 0 << ","   // bdp (not relevant for QA event)
          << 0 << ","   // maxwnd
          << 0 << ","   // avg_delay
          << 0 << ","   // raw_delay
          << 0 << ","   // target
          << 0 << ","   // base_rtt
          << 0 << ","   // ecn
          << 5 << ","   // quadrant=5 means QA event
          << cwnd_before << ","   // store cwnd_before in inc_fair column
          << cwnd_after << ","    // store cwnd_after in inc_prop column
          << achieved_bytes << "," // store achieved_bytes in inc_fast column
          << 0 << ","
          << 0 << ","
          << 0
          << "\n";
}

// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * Mixed NSCC + TCP Cubic simulation for inter-protocol fairness comparison
 * Runs both protocols on the SAME network to measure how they compete for bandwidth
 *
 * Architecture:
 * - NSCC uses switch-based routing (packets forwarded via FatTreeSwitch)
 * - TCP Cubic uses route-based routing (pre-computed paths)
 * - Both traverse the SAME queues, competing for bandwidth
 *
 * Fairness measurement uses two complementary approaches:
 * - Steady-state: use infinite flows (size 0) so bytes_received IS competitive throughput
 * - Phase-analysis: for finite flows, decompose into overlap (competitive) and solo phases
 */
#include "config.h"
#include <sstream>
#include <iostream>
#include <fstream>
#include <string.h>
#include <math.h>
#include <climits>
#include <list>
#include <vector>
#include <algorithm>
#include <numeric>
#include <memory>

#include "network.h"
#include "randomqueue.h"
#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "clock.h"
#include "tcp.h"
#include "tcpcubic.h"
#include "uec.h"
#include "uec_mp.h"
#include "nscc_trace_logger.h"
#include "trigger.h"
#include "firstfit.h"
#include "topology.h"
#include "connection_matrix.h"
#include "fat_tree_topology.h"
#include "fat_tree_switch.h"
#include "compositequeue.h"

#define PRINT_PATHS 0
#include "main.h"

uint32_t RTT = 1;
uint32_t DEFAULT_NODES = 128;

FirstFit* ff = NULL;
EventList eventlist;

// Per-flow record for CSV output
struct FlowRecord {
    uint32_t flow_id;
    string protocol;
    int src;
    int dst;
    uint64_t flow_size_bytes;
    simtime_picosec start_time;
    // Populated after simulation:
    bool finished;
    uint64_t bytes_received;
    uint64_t retransmits;
    simtime_picosec finish_time;  // 0 = not finished or infinite flow
};

// TriggerTarget subclass to capture NSCC flow completion time.
// When an NSCC flow finishes, its end_trigger fires, which calls activate()
// on this target, recording the current simulation time.
class FlowFinishTracker : public TriggerTarget {
public:
    FlowFinishTracker(EventList& el, simtime_picosec* finish_time_ptr)
        : _eventlist(el), _finish_time_ptr(finish_time_ptr) {}

    void activate() override {
        if (*_finish_time_ptr == 0) {
            *_finish_time_ptr = _eventlist.now();
        }
    }
private:
    EventList& _eventlist;
    simtime_picosec* _finish_time_ptr;
};

// Periodic sampler — writes a unified time-series CSV at fixed intervals.
// Uses CompositeQueue::_queuesize_low (not total queuesize()) because ECN
// marking in decide_ECN() is based solely on the low-priority data queue.
class PeriodicSampler : public EventSource {
public:
    PeriodicSampler(EventList& ev, simtime_picosec interval, const string& filepath,
                    vector<TcpSrc*>& tcp_srcs, vector<TcpSink*>& tcp_sinks,
                    vector<UecSrc*>& nscc_srcs, vector<UecSink*>& nscc_sinks,
                    CompositeQueue* bottleneck_queue,
                    mem_b ecn_kmin, mem_b ecn_kmax, mem_b bdp, double linkspeed_gbps,
                    bool tcp_ecn_enabled)
        : EventSource(ev, "sampler"), _interval(interval),
          _tcp_srcs(tcp_srcs), _tcp_sinks(tcp_sinks),
          _nscc_srcs(nscc_srcs), _nscc_sinks(nscc_sinks),
          _bottleneck(bottleneck_queue)
    {
        _out.open(filepath);
        if (!_out.is_open()) {
            cerr << "PeriodicSampler: failed to open " << filepath << endl;
            return;
        }
        // Write metadata line (parsed by plot script)
        _out << "# ecn_kmin=" << ecn_kmin
             << " ecn_kmax=" << ecn_kmax
             << " bdp=" << bdp
             << " linkspeed_gbps=" << linkspeed_gbps
             << " tcp_ecn=" << (tcp_ecn_enabled ? 1 : 0) << endl;
        // Write header
        _out << "time_us";
        for (size_t i = 0; i < _tcp_srcs.size(); i++)
            _out << ",tcp" << i << "_cwnd,tcp" << i << "_bytes_acked,tcp" << i << "_drops";
        for (size_t i = 0; i < _nscc_srcs.size(); i++)
            _out << ",nscc" << i << "_cwnd,nscc" << i << "_bytes"
                 << ",nscc" << i << "_q0,nscc" << i << "_q1"
                 << ",nscc" << i << "_q2,nscc" << i << "_q3"
                 << ",nscc" << i << "_qa,nscc" << i << "_q4";
        _out << ",queue_bytes,queue_drops" << endl;

        ev.sourceIsPending(*this, ev.now());
    }

    void doNextEvent() override {
        if (!_out.is_open()) return;

        double t_us = timeAsUs(eventlist().now());
        _out << t_us;

        for (size_t i = 0; i < _tcp_srcs.size(); i++) {
            _out << "," << _tcp_srcs[i]->_cwnd
                 << "," << _tcp_sinks[i]->total_received()
                 << "," << _tcp_srcs[i]->_drops;
        }
        for (size_t i = 0; i < _nscc_srcs.size(); i++) {
            uint64_t data_pkts = _nscc_sinks[i]->cumulative_ack();
            uint64_t rts_pkts = _nscc_srcs[i]->stats().rts_pkts_sent;
            uint64_t unique_bytes = (data_pkts > rts_pkts) ? (data_pkts - rts_pkts) * UecSrc::_mss : 0;
            _out << "," << _nscc_srcs[i]->cwnd()
                 << "," << unique_bytes
                 << "," << _nscc_srcs[i]->_q0_count
                 << "," << _nscc_srcs[i]->_q1_count
                 << "," << _nscc_srcs[i]->_q2_count
                 << "," << _nscc_srcs[i]->_q3_count
                 << "," << _nscc_srcs[i]->_qa_count
                 << "," << _nscc_srcs[i]->_q4_count;
        }
        // Sample _queuesize_low — this is what decide_ECN() compares against Kmin/Kmax
        _out << "," << _bottleneck->_queuesize_low
             << "," << _bottleneck->num_drops() << endl;

        eventlist().sourceIsPending(*this, eventlist().now() + _interval);
    }

    ~PeriodicSampler() {
        if (_out.is_open()) _out.close();
    }

private:
    simtime_picosec _interval;
    vector<TcpSrc*>& _tcp_srcs;
    vector<TcpSink*>& _tcp_sinks;
    vector<UecSrc*>& _nscc_srcs;
    vector<UecSink*>& _nscc_sinks;
    CompositeQueue* _bottleneck;
    ofstream _out;
};

void exit_error(char* progr) {
    cout << "Usage " << progr
         << " [-o output_file]"
         << " [-nodes N]"
         << " [-conns N]"
         << " [-tm traffic_matrix_file]"
         << " [-topo topology_file]"
         << " [-end end_time_in_us]"
         << " [-seed random_seed]"
         << " [-q queue_size_packets]"
         << " [-linkspeed Mbps]"
         << " [-nscc_ratio 0.0-1.0]"
         << " [-target_q_delay us]"
         << " [-qa_gate N]"
         << " [-path_entropy N]"
         << " [-cwnd packets]"
         << " [-hystart 0|1]"
         << " [-fast_conv 0|1]"
         << " [-csv csv_output_file]"
         << " [-trace trace_output_file]"
         << " [-sample timeseries_csv_file]"
         << " [-ecn]"
         << " [-disable_trim]"
         << " [-tail_drop]"
         << " [-ecn_kmin bytes]"
         << " [-ecn_kmax bytes]"
         << " [-maxwnd_mult multiplier]"
         << " [-delay_hysteresis half_band_us]"
         << " [-q3_pressure fraction]"
         << " [-symmetric_delay]"
         << " [-tcp_reno]"
         << endl;
    exit(1);
}

int main(int argc, char **argv) {
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    uint32_t no_of_conns = 0, no_of_nodes = DEFAULT_NODES;
    stringstream filename(ios_base::out);

    int i = 1;
    filename << "logout.dat";

    // Default parameters
    int seed = 13;
    int end_time = 100000; // 100ms in microseconds
    uint32_t queuesize_pkt = 100;
    simtime_picosec logtime = timeFromMs(0.25);
    double nscc_ratio = 0.5;  // 50% NSCC, 50% TCP Cubic by default
    uint32_t ports = 1;
    uint16_t path_entropy_size = 16;
    bool enable_ecn = false;
    bool disable_trim = false;
    bool tail_drop = false;
    mem_b ecn_kmin_override = 0;  // 0 = use default (queue/4)
    mem_b ecn_kmax_override = 0;  // 0 = use default (queue*97/100)

    // NSCC parameters
    uint32_t target_q_delay_us = 5;
    uint32_t qa_gate = 2;
    double maxwnd_mult = 1.5;
    double delay_hysteresis_us = 0.0;
    double q3_pressure = 0.0;
    bool symmetric_delay = false;

    // TCP parameters
    uint32_t cwnd_pkts = 10;
    bool hystart_enabled = true;
    bool fast_convergence = true;
    bool tcp_ecn_enabled = true;
    bool tcp_reno = false;

    char* tm_file = NULL;
    char* topo_file = NULL;
    char* csv_file = NULL;
    char* trace_file = NULL;
    char* sample_file = NULL;

    while (i < argc) {
        if (!strcmp(argv[i], "-o")) {
            filename.str(std::string());
            filename << argv[i+1];
            i++;
        } else if (!strcmp(argv[i], "-conns")) {
            no_of_conns = atoi(argv[i+1]);
            cout << "no_of_conns " << no_of_conns << endl;
            i++;
        } else if (!strcmp(argv[i], "-nodes")) {
            no_of_nodes = atoi(argv[i+1]);
            cout << "no_of_nodes " << no_of_nodes << endl;
            i++;
        } else if (!strcmp(argv[i], "-end")) {
            end_time = atoi(argv[i+1]);
            cout << "end_time " << end_time << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-tm")) {
            tm_file = argv[i+1];
            cout << "traffic matrix file: " << tm_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-topo")) {
            topo_file = argv[i+1];
            cout << "topology file: " << topo_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-seed")) {
            seed = atoi(argv[i+1]);
            cout << "random seed " << seed << endl;
            i++;
        } else if (!strcmp(argv[i], "-q")) {
            queuesize_pkt = atoi(argv[i+1]);
            cout << "queue size " << queuesize_pkt << " packets" << endl;
            i++;
        } else if (!strcmp(argv[i], "-linkspeed")) {
            linkspeed = speedFromMbps(atof(argv[i+1]));
            cout << "linkspeed " << argv[i+1] << " Mbps" << endl;
            i++;
        } else if (!strcmp(argv[i], "-nscc_ratio")) {
            nscc_ratio = atof(argv[i+1]);
            cout << "NSCC ratio " << nscc_ratio << endl;
            i++;
        } else if (!strcmp(argv[i], "-target_q_delay")) {
            target_q_delay_us = atoi(argv[i+1]);
            cout << "NSCC target queue delay " << target_q_delay_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-qa_gate")) {
            qa_gate = atoi(argv[i+1]);
            cout << "NSCC qa_gate " << qa_gate << endl;
            i++;
        } else if (!strcmp(argv[i], "-path_entropy")) {
            path_entropy_size = atoi(argv[i+1]);
            cout << "NSCC path entropy " << path_entropy_size << endl;
            i++;
        } else if (!strcmp(argv[i], "-cwnd")) {
            cwnd_pkts = atoi(argv[i+1]);
            cout << "TCP Cubic initial cwnd " << cwnd_pkts << " packets" << endl;
            i++;
        } else if (!strcmp(argv[i], "-hystart")) {
            hystart_enabled = atoi(argv[i+1]) != 0;
            cout << "TCP Cubic HyStart " << (hystart_enabled ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-fast_conv")) {
            fast_convergence = atoi(argv[i+1]) != 0;
            cout << "TCP Cubic fast convergence " << (fast_convergence ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-csv")) {
            csv_file = argv[i+1];
            cout << "CSV output: " << csv_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-trace")) {
            trace_file = argv[i+1];
            cout << "Trace output: " << trace_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-ecn")) {
            enable_ecn = true;
            cout << "ECN enabled" << endl;
        } else if (!strcmp(argv[i], "-tcp_ecn")) {
            tcp_ecn_enabled = atoi(argv[i+1]) != 0;
            cout << "TCP Cubic ECN response " << (tcp_ecn_enabled ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-sample")) {
            sample_file = argv[i+1];
            cout << "Time-series sampling: " << sample_file << endl;
            i++;
        } else if (!strcmp(argv[i], "-maxwnd_mult")) {
            maxwnd_mult = atof(argv[i+1]);
            cout << "NSCC maxwnd multiplier " << maxwnd_mult << "x BDP" << endl;
            i++;
        } else if (!strcmp(argv[i], "-delay_hysteresis")) {
            delay_hysteresis_us = atof(argv[i+1]);
            cout << "NSCC delay hysteresis band +/-" << delay_hysteresis_us << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-q3_pressure")) {
            q3_pressure = atof(argv[i+1]);
            cout << "NSCC Q3 pressure " << q3_pressure << " per RTT" << endl;
            i++;
        } else if (!strcmp(argv[i], "-symmetric_delay")) {
            symmetric_delay = true;
            cout << "NSCC symmetric delay estimator enabled" << endl;
        } else if (!strcmp(argv[i], "-disable_trim")) {
            disable_trim = true;
            cout << "Trimming disabled, dropping instead." << endl;
        } else if (!strcmp(argv[i], "-tail_drop")) {
            tail_drop = true;
            cout << "Tail-drop mode: always drop arriving packet when queue full" << endl;
        } else if (!strcmp(argv[i], "-tcp_reno")) {
            tcp_reno = true;
            cout << "Using TCP NewReno (instead of Cubic)" << endl;
        } else if (!strcmp(argv[i], "-ecn_kmin")) {
            ecn_kmin_override = atoi(argv[i+1]);
            i++;
            cout << "ECN Kmin override: " << ecn_kmin_override << " bytes" << endl;
        } else if (!strcmp(argv[i], "-ecn_kmax")) {
            ecn_kmax_override = atoi(argv[i+1]);
            i++;
            cout << "ECN Kmax override: " << ecn_kmax_override << " bytes" << endl;
        } else {
            cout << "Unknown parameter: " << argv[i] << endl;
            exit_error(argv[0]);
        }
        i++;
    }

    srand(seed);
    srandom(seed);

    eventlist.setEndtime(timeFromUs((uint32_t)end_time));
    Clock c(timeFromSec(50 / 100.), eventlist);

    // Prepare loggers
    cout << "Logging to " << filename.str() << endl;
    Logfile logfile(filename.str(), eventlist);
    logfile.setStartTime(timeFromSec(0));

    TcpSinkLoggerSampling* tcp_sink_logger = new TcpSinkLoggerSampling(logtime, eventlist);
    logfile.addLogger(*tcp_sink_logger);

    TcpRtxTimerScanner tcpRtxScanner(timeFromMs(10), eventlist);

    QueueLoggerFactory qlf(&logfile, QueueLoggerFactory::LOGGER_SAMPLING, eventlist);
    qlf.set_sample_period(logtime);

    // Load connection matrix
    auto conns = std::make_unique<ConnectionMatrix>(no_of_nodes);

    if (tm_file) {
        cout << "Loading connection matrix from " << tm_file << endl;
        if (!conns->load(tm_file)) {
            cout << "Failed to load connection matrix " << tm_file << endl;
            exit(-1);
        }
    } else {
        cout << "No traffic matrix specified, using permutation" << endl;
        if (no_of_conns == 0)
            no_of_conns = no_of_nodes;
        conns->setPermutation(no_of_conns);
    }

    no_of_nodes = conns->N;
    cout << "Using " << no_of_nodes << " nodes" << endl;

    // Create topology
    FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
    FatTreeSwitch::_disable_trim = disable_trim;
    CompositeQueue::_tail_drop = tail_drop;

    unique_ptr<FatTreeTopologyCfg> topo_cfg;
    if (topo_file) {
        topo_cfg = FatTreeTopologyCfg::load(topo_file, memFromPkt(queuesize_pkt), COMPOSITE, FAIR_PRIO);
        if (topo_cfg->no_of_nodes() != no_of_nodes) {
            cerr << "Mismatch between connection matrix (" << no_of_nodes
                 << " nodes) and topology (" << topo_cfg->no_of_nodes() << " nodes)" << endl;
            exit(1);
        }
        topo_cfg->set_queue_sizes(memFromPkt(queuesize_pkt));
    } else {
        topo_cfg = make_unique<FatTreeTopologyCfg>(no_of_nodes, linkspeed, memFromPkt(queuesize_pkt), COMPOSITE);
    }

    // Enable ECN if requested
    if (enable_ecn) {
        mem_b queue_bytes = memFromPkt(queuesize_pkt);
        mem_b ecn_low = (ecn_kmin_override > 0) ? ecn_kmin_override : queue_bytes / 4;
        mem_b ecn_high = (ecn_kmax_override > 0) ? ecn_kmax_override : queue_bytes * 97 / 100;
        topo_cfg->set_ecn_parameters(true, true, ecn_low, ecn_high);
        cout << "ECN thresholds: low=" << ecn_low << " bytes, high=" << ecn_high << " bytes" << endl;
    }

    cout << *topo_cfg << endl;

    FatTreeTopology* top = new FatTreeTopology(topo_cfg.get(), &qlf, &eventlist, ff);
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    // Calculate network diameter RTT (for reference only)
    simtime_picosec diameter_rtt = topo_cfg->get_diameter_latency() * 2;
    cout << "Network diameter RTT: " << timeAsUs(diameter_rtt) << " us" << endl;

    // Initialize NSCC parameters using first NSCC flow's actual path RTT
    UecSrc::_maxwnd_multiplier = maxwnd_mult;
    UecSrc::_delay_hysteresis_band = (simtime_picosec)(delay_hysteresis_us * 1000000.0);
    UecSrc::_q3_pressure = q3_pressure;
    UecSrc::_symmetric_delay_estimator = symmetric_delay;
    simtime_picosec target_Qdelay = timeFromUs(target_q_delay_us);
    {
        vector<connection*>* pre_conns = conns->getAllConnections();
        uint32_t nscc_target_pre = (uint32_t)(pre_conns->size() * nscc_ratio);
        // First NSCC flow is at index 0 (if nscc_target > 0)
        if (nscc_target_pre > 0 && pre_conns->size() > 0) {
            connection* first_nscc = pre_conns->at(0);
            simtime_picosec flow_rtt = topo_cfg->get_two_point_diameter_latency(first_nscc->src, first_nscc->dst) * 2;
            cout << "NSCC path RTT (src " << first_nscc->src << " -> dst " << first_nscc->dst << "): " << timeAsUs(flow_rtt) << " us" << endl;
            UecSrc::initNsccParams(flow_rtt, linkspeed, target_Qdelay, qa_gate, !disable_trim);
        } else {
            // No NSCC flows, use diameter RTT as fallback
            UecSrc::initNsccParams(diameter_rtt, linkspeed, target_Qdelay, qa_gate, !disable_trim);
        }
    }

    // Set up trace logger if requested
    unique_ptr<NsccTraceLogger> trace_logger;
    if (trace_file) {
        trace_logger = make_unique<NsccTraceLogger>(trace_file);
        if (trace_logger->is_open()) {
            UecSrc::setTraceLogger(trace_logger.get());
            cout << "NSCC trace logging enabled: " << trace_file << endl;
        } else {
            cerr << "Failed to open trace file: " << trace_file << endl;
            trace_logger.reset();
        }
    }

    // Create UecNIC objects for each node (needed for NSCC)
    vector<unique_ptr<UecNIC>> nics;
    for (uint32_t ix = 0; ix < no_of_nodes; ix++) {
        nics.emplace_back(make_unique<UecNIC>(ix, eventlist, linkspeed, ports));
    }

    // Pre-compute paths for TCP Cubic
    vector<const Route*>*** net_paths;
    net_paths = new vector<const Route*>**[no_of_nodes];
    for (uint32_t n = 0; n < no_of_nodes; n++) {
        net_paths[n] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t m = 0; m < no_of_nodes; m++)
            net_paths[n][m] = NULL;
    }

    // Setup connections - split between NSCC and TCP Cubic
    vector<connection*>* all_conns = conns->getAllConnections();

    vector<TcpSrc*> tcp_srcs;
    vector<TcpSink*> tcp_sinks;
    vector<UecSrc*> nscc_srcs;
    vector<UecSink*> nscc_sinks;
    vector<FlowRecord> flow_records;

    // Triggers and trackers for capturing NSCC completion times
    vector<SingleShotTrigger*> nscc_triggers;
    vector<FlowFinishTracker*> nscc_trackers;

    uint32_t nscc_count = 0, cubic_count = 0;
    uint32_t total_conns = all_conns->size();
    uint32_t nscc_target = (uint32_t)(total_conns * nscc_ratio);

    cout << "Creating " << nscc_target << " NSCC flows and " << (total_conns - nscc_target) << " TCP " << (tcp_reno ? "Reno" : "Cubic") << " flows" << endl;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;

        simtime_picosec starttime = timeFromUs((uint32_t)crt->start);

        // Decide protocol based on ratio
        bool use_nscc = (c < nscc_target);

        FlowRecord rec;
        rec.flow_id = c;
        rec.src = src;
        rec.dst = dest;
        rec.flow_size_bytes = crt->size;
        rec.start_time = starttime;
        rec.finished = false;
        rec.bytes_received = 0;
        rec.retransmits = 0;
        rec.finish_time = 0;

        if (use_nscc) {
            rec.protocol = "NSCC";

            // Create NSCC source and sink with switch-based routing
            unique_ptr<UecMultipath> mp = make_unique<UecMpOblivious>(path_entropy_size, false);

            UecSrc* uecSrc = new UecSrc(NULL, eventlist, std::move(mp), *nics.at(src), ports);
            uecSrc->setName("nscc_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*uecSrc);
            uecSrc->setDst(dest);

            // Create sink (sender-driven mode)
            UecSink* uecSnk = new UecSink(NULL, linkspeed, 1.1,
                                          UecBasePacket::unquantize(UecSink::_credit_per_pull),
                                          eventlist, *nics.at(dest), ports);
            ((DataReceiver*)uecSnk)->setName("nscc_sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*(DataReceiver*)uecSnk);
            uecSnk->setSrc(src);

            if (crt->size > 0) {
                uecSrc->setFlowsize(crt->size);
            } else {
                uecSrc->setFlowsize((uint64_t)1e15);  // ~1 PB: won't complete, stays in signed mem_b range
            }

            // Initialize per-flow NSCC using actual path RTT (not diameter RTT)
            simtime_picosec flow_rtt = topo_cfg->get_two_point_diameter_latency(src, dest) * 2;
            uecSrc->initNscc(0, flow_rtt);

            // Set up switch-based routing
            Route* srctotor = new Route();
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->pipes_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]->getRemoteEndpoint());

            Route* dsttotor = new Route();
            dsttotor->push_back(top->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
            dsttotor->push_back(top->pipes_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
            dsttotor->push_back(top->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]->getRemoteEndpoint());

            uecSrc->connectPort(0, *srctotor, *dsttotor, *uecSnk, crt->start);

            // Register with switches for packet delivery
            top->switches_lp[topo_cfg->HOST_POD_SWITCH(src)]->addHostPort(src, uecSnk->flowId(), uecSrc->getPort(0));
            top->switches_lp[topo_cfg->HOST_POD_SWITCH(dest)]->addHostPort(dest, uecSrc->flowId(), uecSnk->getPort(0));

            nscc_srcs.push_back(uecSrc);
            nscc_sinks.push_back(uecSnk);
            nscc_count++;

        } else {
            rec.protocol = tcp_reno ? "RENO" : "CUBIC";

            // Get paths through the SAME topology
            if (!net_paths[src][dest])
                net_paths[src][dest] = top->get_paths(src, dest);

            size_t choice = 0;
            if (net_paths[src][dest]->size() > 1) {
                choice = rand() % net_paths[src][dest]->size();
            }

            Route* routeout = new Route(*(net_paths[src][dest]->at(choice)));
            Route* routein = new Route();

            TcpSrc* tcpSrc;
            string prefix;
            if (tcp_reno) {
                tcpSrc = new TcpSrc(NULL, NULL, eventlist);
                prefix = "reno_";
            } else {
                TcpCubicSrc* cubicSrc = new TcpCubicSrc(NULL, NULL, eventlist);
                cubicSrc->setHystartEnabled(hystart_enabled);
                cubicSrc->setFastConvergenceEnabled(fast_convergence);
                cubicSrc->setTcpFriendlinessEnabled(true);
                cubicSrc->setEcnEnabled(tcp_ecn_enabled);
                tcpSrc = cubicSrc;
                prefix = "cubic_";
            }
            tcpSrc->setName(prefix + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*tcpSrc);

            TcpSink* tcpSnk = new TcpSink();
            tcpSnk->setName(prefix + "sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*tcpSnk);

            if (crt->size > 0) {
                tcpSrc->set_flowsize(crt->size);
            } else {
                tcpSrc->set_flowsize(UINT64_MAX / 2);  // effectively infinite
            }

            tcpSrc->set_cwnd(cwnd_pkts * Packet::data_packet_size());
            tcpSrc->set_ssthresh(0xffffffff);

            tcpRtxScanner.registerTcp(*tcpSrc);

            routeout->push_back(tcpSnk);
            routein->push_back(tcpSrc);

            tcpSrc->connect(*routeout, *routein, *tcpSnk, starttime);

            tcp_sink_logger->monitorSink(tcpSnk);

            tcp_srcs.push_back(tcpSrc);
            tcp_sinks.push_back(tcpSnk);
            cubic_count++;
        }

        flow_records.push_back(rec);
    }

    // Hook up NSCC end triggers to capture completion times.
    // We do this after flow_records is fully populated so pointers to finish_time are stable.
    {
        uint32_t nscc_ix = 0;
        for (auto& rec : flow_records) {
            if (rec.protocol == "NSCC") {
                if (rec.flow_size_bytes > 0) {
                    auto* tracker = new FlowFinishTracker(eventlist, &rec.finish_time);
                    auto* trigger = new SingleShotTrigger(eventlist, rec.flow_id);
                    trigger->add_target(*tracker);
                    nscc_srcs[nscc_ix]->setEndTrigger(*trigger);
                    nscc_triggers.push_back(trigger);
                    nscc_trackers.push_back(tracker);
                }
                nscc_ix++;
            }
        }
    }

    cout << "Created " << nscc_count << " NSCC flows and " << cubic_count << " TCP " << (tcp_reno ? "Reno" : "Cubic") << " flows" << endl;
    cout << "Both protocols share the SAME network queues - they will compete for bandwidth" << endl;

    // Set up periodic time-series sampler if requested
    unique_ptr<PeriodicSampler> sampler;
    if (sample_file) {
        if (flow_records.empty()) {
            cerr << "Warning: -sample requested but no flows exist; skipping sampler" << endl;
        } else {
            // Bottleneck = ToR downlink to the first flow's destination node.
            // NOTE: This assumes all flows share a single bottleneck (e.g. 2-to-1 incast).
            // For multi-sink or multi-bottleneck workloads, this samples only one queue.
            uint32_t sink_node = flow_records[0].dst;

            // Check that all flows share the same destination (warn if not)
            for (const auto& rec : flow_records) {
                if ((uint32_t)rec.dst != sink_node) {
                    cerr << "Warning: -sample bottleneck is ToR downlink to node " << sink_node
                         << ", but flow " << rec.flow_id << " has dst=" << rec.dst
                         << " — its bottleneck is NOT being sampled" << endl;
                    break;
                }
            }

            auto* bottleneck = dynamic_cast<CompositeQueue*>(
                top->queues_nlp_ns[topo_cfg->HOST_POD_SWITCH(sink_node)][sink_node][0]);
            assert(bottleneck && "Bottleneck queue must be a CompositeQueue");
            cout << "Sampling bottleneck queue: " << bottleneck->nodename()
                 << " (ToR downlink to node " << sink_node << ")" << endl;

            // Compute ECN thresholds and BDP for metadata
            mem_b ecn_kmin = 0, ecn_kmax = 0;
            if (enable_ecn) {
                mem_b queue_bytes = memFromPkt(queuesize_pkt);
                ecn_kmin = (ecn_kmin_override > 0) ? ecn_kmin_override : queue_bytes / 4;
                ecn_kmax = (ecn_kmax_override > 0) ? ecn_kmax_override : queue_bytes * 97 / 100;
            }
            simtime_picosec flow_rtt = topo_cfg->get_two_point_diameter_latency(
                flow_records[0].src, flow_records[0].dst) * 2;
            mem_b bdp = (mem_b)(timeAsSec(flow_rtt) * (double)linkspeed / 8.0);
            double linkspeed_gbps = (double)linkspeed / 1e9;

            simtime_picosec sample_interval = timeFromUs((uint32_t)1); // 1us
            sampler = make_unique<PeriodicSampler>(eventlist, sample_interval, string(sample_file),
                                                    tcp_srcs, tcp_sinks,
                                                    nscc_srcs, nscc_sinks, bottleneck,
                                                    ecn_kmin, ecn_kmax, bdp, linkspeed_gbps,
                                                    tcp_ecn_enabled);
            cout << "Time-series sampling at 1us intervals to " << sample_file << endl;
        }
    }

    // Record setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# protocol = MIXED (NSCC + TCP Cubic competing)");
    logfile.write("# nscc_flows = " + ntoa(nscc_count));
    logfile.write("# cubic_flows = " + ntoa(cubic_count));

    // Run simulation
    cout << "Starting simulation" << endl;
    while (eventlist.doNextEvent()) {
    }

    simtime_picosec sim_end = eventlist.now();
    cout << "Done at " << timeAsUs(sim_end) << " us" << endl;

    // ========================================
    // Populate flow records with results
    // ========================================
    uint32_t nscc_idx = 0, cubic_idx = 0;
    for (auto& rec : flow_records) {
        if (rec.protocol == "NSCC") {
            UecSrc* src_ptr = nscc_srcs[nscc_idx];
            UecSink* snk_ptr = nscc_sinks[nscc_idx];
            rec.finished = src_ptr->isTotallyFinished();
            // Use unique bytes: (cumulative_ack - rts_pkts) * mss
            // cumulative_ack() returns _expected_epsn (packet count),
            // total_received() double-counts retransmits and includes headers
            uint64_t data_pkts = snk_ptr->cumulative_ack();
            uint64_t rts_pkts = src_ptr->stats().rts_pkts_sent;
            rec.bytes_received = (data_pkts > rts_pkts) ? (data_pkts - rts_pkts) * UecSrc::_mss : 0;
            rec.retransmits = 0; // NSCC doesn't expose retransmit count the same way
            // finish_time already set by FlowFinishTracker via end_trigger
            nscc_idx++;
        } else {
            TcpSrc* src_ptr = tcp_srcs[cubic_idx];
            TcpSink* snk_ptr = tcp_sinks[cubic_idx];
            rec.bytes_received = snk_ptr->total_received();
            rec.finished = (src_ptr->_last_acked >= src_ptr->_flow_size && src_ptr->_flow_size > 0);
            rec.retransmits = src_ptr->_drops;
            rec.finish_time = src_ptr->_finish_time;
            cubic_idx++;
        }
    }

    // ========================================
    // Write CSV output
    // ========================================
    if (csv_file) {
        ofstream csv(csv_file);
        if (csv.is_open()) {
            csv << "flow_id,protocol,src,dst,size_bytes,start_us,finish_time_us,fct_us,throughput_gbps,finished,bytes_received,retransmits" << endl;
            for (auto& rec : flow_records) {
                double start_us = timeAsUs(rec.start_time);
                double finish_us = rec.finish_time > 0 ? timeAsUs(rec.finish_time) : -1;
                double fct_us = -1;
                double throughput_gbps = 0;

                if (rec.finished && rec.finish_time > 0) {
                    fct_us = finish_us - start_us;
                    if (fct_us > 0) {
                        throughput_gbps = (rec.bytes_received * 8.0) / (fct_us * 1000.0);
                    }
                } else if (rec.bytes_received > 0) {
                    double elapsed_us = timeAsUs(sim_end) - start_us;
                    if (elapsed_us > 0) {
                        throughput_gbps = (rec.bytes_received * 8.0) / (elapsed_us * 1000.0);
                    }
                }

                csv << rec.flow_id << ","
                    << rec.protocol << ","
                    << rec.src << ","
                    << rec.dst << ","
                    << rec.flow_size_bytes << ","
                    << start_us << ","
                    << finish_us << ","
                    << fct_us << ","
                    << throughput_gbps << ","
                    << (rec.finished ? 1 : 0) << ","
                    << rec.bytes_received << ","
                    << rec.retransmits
                    << endl;
            }
            csv.close();
            cout << "CSV results written to " << csv_file << endl;
        } else {
            cerr << "Failed to open CSV file: " << csv_file << endl;
        }
    }

    // ========================================
    // Console statistics
    // ========================================
    cout << "\n========================================" << endl;
    cout << "INTER-PROTOCOL FAIRNESS RESULTS" << endl;
    cout << "========================================" << endl;

    // NSCC statistics
    cout << "\n=== NSCC Statistics ===" << endl;
    uint64_t nscc_total_bytes = 0;
    uint32_t nscc_finished = 0;
    vector<double> nscc_throughputs;
    for (auto& rec : flow_records) {
        if (rec.protocol != "NSCC") continue;
        nscc_total_bytes += rec.bytes_received;
        if (rec.finished) nscc_finished++;
        if (rec.bytes_received > 0) {
            double end_us = (rec.finish_time > 0) ? timeAsUs(rec.finish_time) : timeAsUs(sim_end);
            double elapsed_us = end_us - timeAsUs(rec.start_time);
            if (elapsed_us > 0) {
                nscc_throughputs.push_back((rec.bytes_received * 8.0) / (elapsed_us * 1000.0));
            }
        }
    }
    cout << "NSCC flows completed: " << nscc_finished << "/" << nscc_count << endl;
    cout << "NSCC total bytes received (unique): " << nscc_total_bytes << endl;
    if (!nscc_throughputs.empty()) {
        sort(nscc_throughputs.begin(), nscc_throughputs.end());
        double sum = accumulate(nscc_throughputs.begin(), nscc_throughputs.end(), 0.0);
        cout << "NSCC per-flow throughput (Gbps): mean=" << sum / nscc_throughputs.size()
             << " median=" << nscc_throughputs[nscc_throughputs.size()/2]
             << " p99=" << nscc_throughputs[(size_t)(nscc_throughputs.size() * 0.99)]
             << endl;
    }

    // TCP statistics
    string tcp_name = tcp_reno ? "TCP Reno" : "TCP Cubic";
    cout << "\n=== " << tcp_name << " Statistics ===" << endl;
    uint64_t cubic_total_bytes = 0;
    uint64_t cubic_retx = 0;
    uint32_t cubic_finished = 0;
    vector<double> cubic_throughputs;
    for (auto& rec : flow_records) {
        if (rec.protocol == "NSCC") continue;
        cubic_total_bytes += rec.bytes_received;
        cubic_retx += rec.retransmits;
        if (rec.finished) cubic_finished++;
        if (rec.bytes_received > 0) {
            double end_us = (rec.finish_time > 0) ? timeAsUs(rec.finish_time) : timeAsUs(sim_end);
            double elapsed_us = end_us - timeAsUs(rec.start_time);
            if (elapsed_us > 0) {
                cubic_throughputs.push_back((rec.bytes_received * 8.0) / (elapsed_us * 1000.0));
            }
        }
    }
    cout << tcp_name << " flows completed: " << cubic_finished << "/" << cubic_count << endl;
    cout << tcp_name << " total bytes received: " << cubic_total_bytes << endl;
    cout << tcp_name << " retransmits: " << cubic_retx << endl;
    if (!cubic_throughputs.empty()) {
        sort(cubic_throughputs.begin(), cubic_throughputs.end());
        double sum = accumulate(cubic_throughputs.begin(), cubic_throughputs.end(), 0.0);
        cout << tcp_name << " per-flow throughput (Gbps): mean=" << sum / cubic_throughputs.size()
             << " median=" << cubic_throughputs[cubic_throughputs.size()/2]
             << " p99=" << cubic_throughputs[(size_t)(cubic_throughputs.size() * 0.99)]
             << endl;
    }

    // ========================================
    // Phase-based competitive fairness analysis
    // ========================================
    cout << "\n=== Competitive Fairness Analysis ===" << endl;

    bool all_still_running = true;
    simtime_picosec earliest_finish = sim_end;
    simtime_picosec latest_finish = 0;
    simtime_picosec latest_start = 0;
    for (auto& rec : flow_records) {
        if (rec.finish_time > 0) {
            all_still_running = false;
            if (rec.finish_time < earliest_finish)
                earliest_finish = rec.finish_time;
            if (rec.finish_time > latest_finish)
                latest_finish = rec.finish_time;
        }
        if (rec.start_time > latest_start)
            latest_start = rec.start_time;
    }

    if (all_still_running) {
        cout << "Mode: STEADY-STATE (all flows active for entire simulation)" << endl;
        cout << "Measurement window: " << timeAsUs(latest_start) << " - " << timeAsUs(sim_end)
             << " us (" << timeAsUs(sim_end - latest_start) << " us)" << endl;

        double total_bytes = nscc_total_bytes + cubic_total_bytes;
        if (total_bytes > 0 && nscc_count > 0 && cubic_count > 0) {
            double nscc_share = (nscc_total_bytes * 100.0) / total_bytes;
            double cubic_share = (cubic_total_bytes * 100.0) / total_bytes;

            double window_us = timeAsUs(sim_end - latest_start);
            double nscc_gbps = (nscc_total_bytes * 8.0) / (window_us * 1000.0);
            double cubic_gbps = (cubic_total_bytes * 8.0) / (window_us * 1000.0);

            cout << "NSCC:  " << nscc_total_bytes << " bytes, "
                 << nscc_gbps << " Gbps, share=" << nscc_share << "%" << endl;
            cout << "Cubic: " << cubic_total_bytes << " bytes, "
                 << cubic_gbps << " Gbps, share=" << cubic_share << "%" << endl;

            double sum_x = nscc_gbps + cubic_gbps;
            double sum_x2 = nscc_gbps * nscc_gbps + cubic_gbps * cubic_gbps;
            double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
            cout << "Competitive JFI: " << jfi << endl;
        }

    } else {
        cout << "Mode: PHASE ANALYSIS (at least one flow completed)" << endl;

        simtime_picosec overlap_end = earliest_finish;
        simtime_picosec overlap_start = latest_start;
        double overlap_us = timeAsUs(overlap_end) - timeAsUs(overlap_start);

        cout << "Phase 1 (overlap): " << timeAsUs(overlap_start) << " - " << timeAsUs(overlap_end)
             << " us (" << overlap_us << " us)" << endl;
        simtime_picosec phase2_end = latest_finish > 0 ? latest_finish : sim_end;
        cout << "Phase 2 (solo):    " << timeAsUs(overlap_end) << " - " << timeAsUs(phase2_end)
             << " us (" << (timeAsUs(phase2_end) - timeAsUs(overlap_end)) << " us)" << endl;

        if (overlap_us > 0 && nscc_count > 0 && cubic_count > 0) {
            double phase2_us = timeAsUs(phase2_end) - timeAsUs(overlap_end);

            int64_t nscc_phase1_bytes = (int64_t)nscc_total_bytes;
            int64_t cubic_phase1_bytes = (int64_t)cubic_total_bytes;

            bool nscc_finished_first = false;
            bool cubic_finished_first = false;
            for (auto& rec : flow_records) {
                if (rec.finish_time == earliest_finish) {
                    if (rec.protocol == "NSCC") nscc_finished_first = true;
                    else cubic_finished_first = true;
                }
            }

            // Estimate Phase 2 solo bytes per-flow: each surviving-protocol flow
            // contributes based on its own average rate and its active time in Phase 2.
            // This handles multi-flow experiments with staggered starts correctly.
            double phase2_start_us = timeAsUs(overlap_end);
            double phase2_end_us = timeAsUs(phase2_end);

            auto estimate_phase2_bytes = [&](const string& survivor_proto) -> uint64_t {
                uint64_t total = 0;
                for (auto& rec : flow_records) {
                    if (rec.protocol != survivor_proto) continue;
                    double flow_start_us = timeAsUs(rec.start_time);
                    double flow_end_us = (rec.finish_time > 0)
                        ? timeAsUs(rec.finish_time) : phase2_end_us;
                    double flow_lifetime_us = flow_end_us - flow_start_us;
                    if (flow_lifetime_us <= 0 || rec.bytes_received == 0) continue;
                    // How much of Phase 2 was this flow active?
                    double active_start = max(phase2_start_us, flow_start_us);
                    double active_end = min(phase2_end_us, flow_end_us);
                    double active_us = max(0.0, active_end - active_start);
                    // Estimate using this flow's own average rate
                    double flow_avg_bps = (double)rec.bytes_received * 8.0 / (flow_lifetime_us / 1e6);
                    total += (uint64_t)(flow_avg_bps * active_us / 1e6 / 8.0);
                }
                return total;
            };

            if (nscc_finished_first && !cubic_finished_first) {
                // NSCC finished first, TCP ran solo in Phase 2.
                uint64_t phase2_solo_bytes = estimate_phase2_bytes(tcp_reno ? "RENO" : "CUBIC");
                cubic_phase1_bytes = (int64_t)cubic_total_bytes - (int64_t)phase2_solo_bytes;
                if (cubic_phase1_bytes < 0) cubic_phase1_bytes = 0;
                cout << "NSCC finished first. Cubic ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated Cubic solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else if (cubic_finished_first && !nscc_finished_first) {
                // Cubic finished first, NSCC ran solo in Phase 2.
                uint64_t phase2_solo_bytes = estimate_phase2_bytes("NSCC");
                nscc_phase1_bytes = (int64_t)nscc_total_bytes - (int64_t)phase2_solo_bytes;
                if (nscc_phase1_bytes < 0) nscc_phase1_bytes = 0;
                cout << "Cubic finished first. NSCC ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated NSCC solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else {
                cout << "Both protocols finished at the same time (or all finished)" << endl;
            }

            double phase1_total = (double)nscc_phase1_bytes + (double)cubic_phase1_bytes;
            if (phase1_total > 0) {
                double nscc_share = (nscc_phase1_bytes * 100.0) / phase1_total;
                double cubic_share = (cubic_phase1_bytes * 100.0) / phase1_total;

                double nscc_phase1_gbps = (nscc_phase1_bytes * 8.0) / (overlap_us * 1000.0);
                double cubic_phase1_gbps = (cubic_phase1_bytes * 8.0) / (overlap_us * 1000.0);

                cout << "\nCompetitive throughput (Phase 1 only):" << endl;
                cout << "  NSCC:  " << nscc_phase1_bytes << " bytes, " << nscc_phase1_gbps << " Gbps" << endl;
                cout << "  Cubic: " << cubic_phase1_bytes << " bytes, " << cubic_phase1_gbps << " Gbps" << endl;
                cout << "\nCompetitive bandwidth share:" << endl;
                cout << "  NSCC:  " << nscc_share << "%" << endl;
                cout << "  Cubic: " << cubic_share << "%" << endl;

                double sum_x = nscc_phase1_gbps + cubic_phase1_gbps;
                double sum_x2 = nscc_phase1_gbps * nscc_phase1_gbps + cubic_phase1_gbps * cubic_phase1_gbps;
                double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
                cout << "Competitive JFI: " << jfi << endl;
            }
        }
    }

    // Raw bandwidth share for comparison
    cout << "\n=== Raw Bandwidth Share (total bytes, for reference) ===" << endl;
    double total_bytes = nscc_total_bytes + cubic_total_bytes;
    if (total_bytes > 0) {
        cout << "NSCC:  " << (nscc_total_bytes * 100.0) / total_bytes << "%" << endl;
        cout << "Cubic: " << (cubic_total_bytes * 100.0) / total_bytes << "%" << endl;
    }

    // Jain's fairness index across all flows
    cout << "\n=== Jain's Fairness Index (per-flow) ===" << endl;
    vector<double> all_throughputs;
    all_throughputs.insert(all_throughputs.end(), nscc_throughputs.begin(), nscc_throughputs.end());
    all_throughputs.insert(all_throughputs.end(), cubic_throughputs.begin(), cubic_throughputs.end());
    if (all_throughputs.size() > 1) {
        double sum_x = 0, sum_x2 = 0;
        for (double t : all_throughputs) {
            sum_x += t;
            sum_x2 += t * t;
        }
        double n = all_throughputs.size();
        double jains_fi = (sum_x * sum_x) / (n * sum_x2);
        cout << "Jain's Fairness Index (all flows): " << jains_fi << endl;
    }

    // Clean up
    if (trace_logger) {
        UecSrc::setTraceLogger(nullptr);
        trace_logger.reset();
    }

    for (auto* t : nscc_triggers) delete t;
    for (auto* t : nscc_trackers) delete t;

    for (uint32_t n = 0; n < no_of_nodes; n++) {
        delete[] net_paths[n];
    }
    delete[] net_paths;

    return 0;
}

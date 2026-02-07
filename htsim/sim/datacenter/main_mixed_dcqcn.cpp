// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * Mixed DCQCN + TCP Cubic simulation for inter-protocol fairness comparison
 * Runs both protocols on the SAME network to measure how they compete for bandwidth
 *
 * Architecture:
 * - DCQCN uses switch-based routing (packets forwarded via FatTreeSwitch)
 * - TCP Cubic uses route-based routing (pre-computed paths)
 * - Both traverse the SAME queues, competing for bandwidth
 * - DCQCN relies on ECN marking -> CNP feedback for rate control
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
#include "roce.h"
#include "dcqcn.h"
#include "cnppacket.h"
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

// TriggerTarget subclass to capture DCQCN flow completion time.
// When a DCQCN flow finishes, its end_trigger fires, which calls activate()
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
         << " [-dcqcn_ratio 0.0-1.0]"
         << " [-cwnd packets]"
         << " [-hystart 0|1]"
         << " [-fast_conv 0|1]"
         << " [-csv csv_output_file]"
         << " [-ecn]"
         << endl;
    exit(1);
}

int main(int argc, char **argv) {
    Clock c(timeFromSec(50 / 100.), eventlist);
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
    double dcqcn_ratio = 0.5;  // 50% DCQCN, 50% TCP Cubic by default
    bool enable_ecn = false;

    // TCP Cubic parameters
    uint32_t cwnd_pkts = 10;
    bool hystart_enabled = true;
    bool fast_convergence = true;

    char* tm_file = NULL;
    char* topo_file = NULL;
    char* csv_file = NULL;

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
        } else if (!strcmp(argv[i], "-dcqcn_ratio")) {
            dcqcn_ratio = atof(argv[i+1]);
            cout << "DCQCN ratio " << dcqcn_ratio << endl;
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
        } else if (!strcmp(argv[i], "-ecn")) {
            enable_ecn = true;
            cout << "ECN enabled" << endl;
        } else {
            cout << "Unknown parameter: " << argv[i] << endl;
            exit_error(argv[0]);
        }
        i++;
    }

    srand(seed);
    srandom(seed);

    // Enable out-of-order packet reception for RoCE/DCQCN sinks.
    // Without this, ECMP multipath causes packet reordering, triggering
    // go-back-N retransmit storms that make the simulation unusably slow.
    RoceSink::ooo_enabled = true;

    eventlist.setEndtime(timeFromUs((uint32_t)end_time));

    // Prepare loggers
    cout << "Logging to " << filename.str() << endl;
    Logfile logfile(filename.str(), eventlist);
    logfile.setStartTime(timeFromSec(0));

    TcpSinkLoggerSampling* tcp_sink_logger = new TcpSinkLoggerSampling(logtime, eventlist);
    logfile.addLogger(*tcp_sink_logger);

    RoceSinkLoggerSampling roce_sink_logger(logtime, eventlist);
    logfile.addLogger(roce_sink_logger);

    TcpRtxTimerScanner tcpRtxScanner(timeFromMs(10), eventlist);

    RoceSrc::setMinRTO(1000); // increase RTO to avoid spurious retransmits

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

    // Create topology — use COMPOSITE queues so both DCQCN and TCP share the same queues
    FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);

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

    // Enable ECN — critical for DCQCN to function (it needs ECN marks to generate CNPs)
    if (enable_ecn) {
        mem_b queue_bytes = memFromPkt(queuesize_pkt);
        mem_b ecn_low = queue_bytes / 4;
        mem_b ecn_high = queue_bytes * 97 / 100;
        topo_cfg->set_ecn_parameters(true, true, ecn_low, ecn_high);
        cout << "ECN thresholds: low=" << ecn_low << " bytes, high=" << ecn_high << " bytes" << endl;
    } else {
        cout << "WARNING: ECN is disabled. DCQCN requires ECN to function properly!" << endl;
    }

    cout << *topo_cfg << endl;

    FatTreeTopology* top = new FatTreeTopology(topo_cfg.get(), &qlf, &eventlist, ff);
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    // Pre-compute paths for TCP Cubic
    vector<const Route*>*** net_paths;
    net_paths = new vector<const Route*>**[no_of_nodes];
    for (uint32_t n = 0; n < no_of_nodes; n++) {
        net_paths[n] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t m = 0; m < no_of_nodes; m++)
            net_paths[n][m] = NULL;
    }

    // Setup connections - split between DCQCN and TCP Cubic
    vector<connection*>* all_conns = conns->getAllConnections();

    vector<TcpCubicSrc*> cubic_srcs;
    vector<TcpSink*> cubic_sinks;
    vector<RoceSrc*> dcqcn_srcs;
    vector<RoceSink*> dcqcn_sinks;
    vector<FlowRecord> flow_records;

    // Triggers and trackers for capturing DCQCN completion times
    vector<SingleShotTrigger*> dcqcn_triggers;
    vector<FlowFinishTracker*> dcqcn_trackers;

    uint32_t dcqcn_count = 0, cubic_count = 0;
    uint32_t total_conns = all_conns->size();
    uint32_t dcqcn_target = (uint32_t)(total_conns * dcqcn_ratio);

    cout << "Creating " << dcqcn_target << " DCQCN flows and " << (total_conns - dcqcn_target) << " TCP Cubic flows" << endl;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;

        // Note: crt->start is already in the right units for RoceSrc::connect
        // (which calls timeFromUs internally), but TCP's connect expects picoseconds.
        // So we keep both: raw start for DCQCN, converted for TCP.
        simtime_picosec starttime_tcp = timeFromUs((uint32_t)crt->start);

        // Decide protocol based on ratio
        bool use_dcqcn = (c < dcqcn_target);

        FlowRecord rec;
        rec.flow_id = c;
        rec.src = src;
        rec.dst = dest;
        rec.flow_size_bytes = crt->size;
        rec.start_time = starttime_tcp; // always store in picoseconds for stats
        rec.finished = false;
        rec.bytes_received = 0;
        rec.retransmits = 0;
        rec.finish_time = 0;

        if (use_dcqcn) {
            rec.protocol = "DCQCN";

            // Create DCQCN source and sink
            DCQCNSrc* roceSrc = new DCQCNSrc(NULL, NULL, eventlist, linkspeed);
            roceSrc->setName("dcqcn_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*roceSrc);
            roceSrc->set_dst(dest);

            if (crt->size > 0) {
                roceSrc->set_flowsize(crt->size);
            }

            DCQCNSink* roceSnk = new DCQCNSink(eventlist);
            ((DataReceiver*)roceSnk)->setName("dcqcn_sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*(DataReceiver*)roceSnk);
            roceSnk->set_src(src);

            // Register with HostQueue for pause/unpause
            ((HostQueue*)top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0])->addHostSender(roceSrc);

            // Set up switch-based routing (ECMP_FIB)
            Route* srctotor = new Route();
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->pipes_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]->getRemoteEndpoint());

            Route* dsttotor = new Route();
            dsttotor->push_back(top->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
            dsttotor->push_back(top->pipes_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]);
            dsttotor->push_back(top->queues_ns_nlp[dest][topo_cfg->HOST_POD_SWITCH(dest)][0]->getRemoteEndpoint());

            roceSrc->connect(srctotor, dsttotor, *roceSnk, crt->start);

            // Register src and sink to receive packets from their respective ToRs
            top->switches_lp[topo_cfg->HOST_POD_SWITCH(src)]->addHostPort(src, roceSrc->flow_id(), roceSrc);
            top->switches_lp[topo_cfg->HOST_POD_SWITCH(dest)]->addHostPort(dest, roceSrc->flow_id(), roceSnk);

            roce_sink_logger.monitorSink(roceSnk);

            dcqcn_srcs.push_back(roceSrc);
            dcqcn_sinks.push_back(roceSnk);
            dcqcn_count++;

        } else {
            rec.protocol = "CUBIC";

            // Get paths through the SAME topology
            if (!net_paths[src][dest])
                net_paths[src][dest] = top->get_paths(src, dest);

            size_t choice = 0;
            if (net_paths[src][dest]->size() > 1) {
                choice = rand() % net_paths[src][dest]->size();
            }

            Route* routeout = new Route(*(net_paths[src][dest]->at(choice)));
            Route* routein = new Route();

            TcpCubicSrc* tcpSrc = new TcpCubicSrc(NULL, NULL, eventlist);
            tcpSrc->setName("cubic_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*tcpSrc);

            TcpSink* tcpSnk = new TcpSink();
            tcpSnk->setName("cubic_sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*tcpSnk);

            if (crt->size > 0) {
                tcpSrc->set_flowsize(crt->size);
            }

            tcpSrc->set_cwnd(cwnd_pkts * Packet::data_packet_size());
            tcpSrc->set_ssthresh(0xffffffff);
            tcpSrc->setHystartEnabled(hystart_enabled);
            tcpSrc->setFastConvergenceEnabled(fast_convergence);
            tcpSrc->setTcpFriendlinessEnabled(true);

            tcpRtxScanner.registerTcp(*tcpSrc);

            routeout->push_back(tcpSnk);
            routein->push_back(tcpSrc);

            tcpSrc->connect(*routeout, *routein, *tcpSnk, starttime_tcp);

            tcp_sink_logger->monitorSink(tcpSnk);

            cubic_srcs.push_back(tcpSrc);
            cubic_sinks.push_back(tcpSnk);
            cubic_count++;
        }

        flow_records.push_back(rec);
    }

    // Hook up DCQCN end triggers to capture completion times.
    // We do this after flow_records is fully populated so pointers to finish_time are stable.
    {
        uint32_t dcqcn_ix = 0;
        for (auto& rec : flow_records) {
            if (rec.protocol == "DCQCN") {
                if (rec.flow_size_bytes > 0) {
                    auto* tracker = new FlowFinishTracker(eventlist, &rec.finish_time);
                    auto* trigger = new SingleShotTrigger(eventlist, rec.flow_id);
                    trigger->add_target(*tracker);
                    dcqcn_srcs[dcqcn_ix]->set_end_trigger(*trigger);
                    dcqcn_triggers.push_back(trigger);
                    dcqcn_trackers.push_back(tracker);
                }
                dcqcn_ix++;
            }
        }
    }

    cout << "Created " << dcqcn_count << " DCQCN flows and " << cubic_count << " TCP Cubic flows" << endl;
    cout << "Both protocols share the SAME network queues - they will compete for bandwidth" << endl;

    // Record setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# protocol = MIXED (DCQCN + TCP Cubic competing)");
    logfile.write("# dcqcn_flows = " + ntoa(dcqcn_count));
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
    uint32_t dcqcn_idx = 0, cubic_idx = 0;
    for (auto& rec : flow_records) {
        if (rec.protocol == "DCQCN") {
            RoceSrc* src_ptr = dcqcn_srcs[dcqcn_idx];
            RoceSink* snk_ptr = dcqcn_sinks[dcqcn_idx];
            // cumulative_ack() returns packet sequence number, convert to bytes
            rec.bytes_received = snk_ptr->cumulative_ack() * Packet::data_packet_size();
            rec.finished = (rec.flow_size_bytes > 0 && rec.bytes_received >= rec.flow_size_bytes);
            rec.retransmits = src_ptr->_rtx_packets_sent;
            // finish_time already set by FlowFinishTracker via end_trigger
            dcqcn_idx++;
        } else {
            TcpCubicSrc* src_ptr = cubic_srcs[cubic_idx];
            TcpSink* snk_ptr = cubic_sinks[cubic_idx];
            rec.bytes_received = snk_ptr->cumulative_ack();
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
                    // Use actual finish time for accurate FCT and throughput
                    fct_us = finish_us - start_us;
                    if (fct_us > 0) {
                        throughput_gbps = (rec.bytes_received * 8.0) / (fct_us * 1000.0);
                    }
                } else if (rec.bytes_received > 0) {
                    // Still running at sim end (infinite flow or unfinished)
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

    // DCQCN statistics
    cout << "\n=== DCQCN Statistics ===" << endl;
    uint64_t dcqcn_total_bytes = 0;
    uint32_t dcqcn_finished = 0;
    uint64_t dcqcn_retx = 0;
    vector<double> dcqcn_throughputs;
    for (auto& rec : flow_records) {
        if (rec.protocol != "DCQCN") continue;
        dcqcn_total_bytes += rec.bytes_received;
        dcqcn_retx += rec.retransmits;
        if (rec.finished) dcqcn_finished++;
        if (rec.bytes_received > 0) {
            // Use actual finish time if available, otherwise sim_end
            double end_us = (rec.finish_time > 0) ? timeAsUs(rec.finish_time) : timeAsUs(sim_end);
            double elapsed_us = end_us - timeAsUs(rec.start_time);
            if (elapsed_us > 0) {
                dcqcn_throughputs.push_back((rec.bytes_received * 8.0) / (elapsed_us * 1000.0));
            }
        }
    }
    cout << "DCQCN flows completed: " << dcqcn_finished << "/" << dcqcn_count << endl;
    cout << "DCQCN total bytes received: " << dcqcn_total_bytes << endl;
    cout << "DCQCN retransmits: " << dcqcn_retx << endl;
    if (!dcqcn_throughputs.empty()) {
        sort(dcqcn_throughputs.begin(), dcqcn_throughputs.end());
        double sum = accumulate(dcqcn_throughputs.begin(), dcqcn_throughputs.end(), 0.0);
        cout << "DCQCN per-flow throughput (Gbps): mean=" << sum / dcqcn_throughputs.size()
             << " median=" << dcqcn_throughputs[dcqcn_throughputs.size()/2]
             << " p99=" << dcqcn_throughputs[(size_t)(dcqcn_throughputs.size() * 0.99)]
             << endl;
    }

    // TCP Cubic statistics
    cout << "\n=== TCP Cubic Statistics ===" << endl;
    uint64_t cubic_total_bytes = 0;
    uint64_t cubic_retx = 0;
    uint32_t cubic_finished = 0;
    vector<double> cubic_throughputs;
    for (auto& rec : flow_records) {
        if (rec.protocol != "CUBIC") continue;
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
    cout << "TCP Cubic flows completed: " << cubic_finished << "/" << cubic_count << endl;
    cout << "TCP Cubic total bytes received: " << cubic_total_bytes << endl;
    cout << "TCP Cubic retransmits: " << cubic_retx << endl;
    if (!cubic_throughputs.empty()) {
        sort(cubic_throughputs.begin(), cubic_throughputs.end());
        double sum = accumulate(cubic_throughputs.begin(), cubic_throughputs.end(), 0.0);
        cout << "TCP Cubic per-flow throughput (Gbps): mean=" << sum / cubic_throughputs.size()
             << " median=" << cubic_throughputs[cubic_throughputs.size()/2]
             << " p99=" << cubic_throughputs[(size_t)(cubic_throughputs.size() * 0.99)]
             << endl;
    }

    // ========================================
    // Phase-based competitive fairness analysis
    // ========================================
    cout << "\n=== Competitive Fairness Analysis ===" << endl;

    // Determine if any flows finished (finite flow experiment) or all still running (steady-state)
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
        // Steady-state mode: all flows still active at sim_end.
        // bytes_received directly reflects competitive throughput.
        cout << "Mode: STEADY-STATE (all flows active for entire simulation)" << endl;
        cout << "Measurement window: " << timeAsUs(latest_start) << " - " << timeAsUs(sim_end)
             << " us (" << timeAsUs(sim_end - latest_start) << " us)" << endl;

        double total_bytes = dcqcn_total_bytes + cubic_total_bytes;
        if (total_bytes > 0 && dcqcn_count > 0 && cubic_count > 0) {
            double dcqcn_share = (dcqcn_total_bytes * 100.0) / total_bytes;
            double cubic_share = (cubic_total_bytes * 100.0) / total_bytes;

            double window_us = timeAsUs(sim_end - latest_start);
            double dcqcn_gbps = (dcqcn_total_bytes * 8.0) / (window_us * 1000.0);
            double cubic_gbps = (cubic_total_bytes * 8.0) / (window_us * 1000.0);

            cout << "DCQCN: " << dcqcn_total_bytes << " bytes, "
                 << dcqcn_gbps << " Gbps, share=" << dcqcn_share << "%" << endl;
            cout << "Cubic: " << cubic_total_bytes << " bytes, "
                 << cubic_gbps << " Gbps, share=" << cubic_share << "%" << endl;

            // Jain's Fairness Index over per-protocol aggregate throughputs
            double sum_x = dcqcn_gbps + cubic_gbps;
            double sum_x2 = dcqcn_gbps * dcqcn_gbps + cubic_gbps * cubic_gbps;
            double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
            cout << "Competitive JFI: " << jfi << endl;
        }

    } else {
        // Phase analysis mode: at least one flow finished before sim_end.
        // Decompose into Phase 1 (overlap: all flows active) and Phase 2 (solo: only slower flow).
        cout << "Mode: PHASE ANALYSIS (at least one flow completed)" << endl;

        simtime_picosec overlap_end = earliest_finish;
        simtime_picosec overlap_start = latest_start;
        double overlap_us = timeAsUs(overlap_end) - timeAsUs(overlap_start);

        cout << "Phase 1 (overlap): " << timeAsUs(overlap_start) << " - " << timeAsUs(overlap_end)
             << " us (" << overlap_us << " us)" << endl;
        // Phase 2 ends at the latest finish time (not sim_end, which may be much later for infinite sim)
        simtime_picosec phase2_end = latest_finish > 0 ? latest_finish : sim_end;
        cout << "Phase 2 (solo):    " << timeAsUs(overlap_end) << " - " << timeAsUs(phase2_end)
             << " us (" << (timeAsUs(phase2_end) - timeAsUs(overlap_end)) << " us)" << endl;

        if (overlap_us > 0 && dcqcn_count > 0 && cubic_count > 0) {
            // Estimate Phase 2 bytes for the flow that was still running alone.
            // The solo flow runs at approximately link_rate during Phase 2.
            double phase2_us = timeAsUs(phase2_end) - timeAsUs(overlap_end);
            double link_rate_gbps = linkspeed / 1e9;
            uint64_t phase2_solo_bytes = (uint64_t)(link_rate_gbps * 1e9 / 8.0 * phase2_us / 1e6);

            // Compute Phase 1 bytes per protocol using signed arithmetic to avoid underflow
            int64_t dcqcn_phase1_bytes = (int64_t)dcqcn_total_bytes;
            int64_t cubic_phase1_bytes = (int64_t)cubic_total_bytes;

            // Identify which protocol(s) finished first
            bool dcqcn_finished_first = false;
            bool cubic_finished_first = false;
            for (auto& rec : flow_records) {
                if (rec.finish_time == earliest_finish) {
                    if (rec.protocol == "DCQCN") dcqcn_finished_first = true;
                    else cubic_finished_first = true;
                }
            }

            if (dcqcn_finished_first && !cubic_finished_first) {
                // DCQCN finished first, Cubic ran solo in Phase 2
                cubic_phase1_bytes = (int64_t)cubic_total_bytes - (int64_t)phase2_solo_bytes;
                if (cubic_phase1_bytes < 0) cubic_phase1_bytes = 0;
                cout << "DCQCN finished first. Cubic ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated Cubic solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else if (cubic_finished_first && !dcqcn_finished_first) {
                // Cubic finished first, DCQCN ran solo in Phase 2
                dcqcn_phase1_bytes = (int64_t)dcqcn_total_bytes - (int64_t)phase2_solo_bytes;
                if (dcqcn_phase1_bytes < 0) dcqcn_phase1_bytes = 0;
                cout << "Cubic finished first. DCQCN ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated DCQCN solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else {
                cout << "Both protocols finished at the same time (or all finished)" << endl;
            }

            double phase1_total = (double)dcqcn_phase1_bytes + (double)cubic_phase1_bytes;
            if (phase1_total > 0) {
                double dcqcn_share = (dcqcn_phase1_bytes * 100.0) / phase1_total;
                double cubic_share = (cubic_phase1_bytes * 100.0) / phase1_total;

                double dcqcn_phase1_gbps = (dcqcn_phase1_bytes * 8.0) / (overlap_us * 1000.0);
                double cubic_phase1_gbps = (cubic_phase1_bytes * 8.0) / (overlap_us * 1000.0);

                cout << "\nCompetitive throughput (Phase 1 only):" << endl;
                cout << "  DCQCN: " << dcqcn_phase1_bytes << " bytes, " << dcqcn_phase1_gbps << " Gbps" << endl;
                cout << "  Cubic: " << cubic_phase1_bytes << " bytes, " << cubic_phase1_gbps << " Gbps" << endl;
                cout << "\nCompetitive bandwidth share:" << endl;
                cout << "  DCQCN: " << dcqcn_share << "%" << endl;
                cout << "  Cubic: " << cubic_share << "%" << endl;

                // Jain's Fairness Index
                double sum_x = dcqcn_phase1_gbps + cubic_phase1_gbps;
                double sum_x2 = dcqcn_phase1_gbps * dcqcn_phase1_gbps + cubic_phase1_gbps * cubic_phase1_gbps;
                double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
                cout << "Competitive JFI: " << jfi << endl;
            }
        }
    }

    // Also show raw (flawed) bandwidth share for comparison
    cout << "\n=== Raw Bandwidth Share (total bytes, for reference) ===" << endl;
    double total_bytes = dcqcn_total_bytes + cubic_total_bytes;
    if (total_bytes > 0) {
        cout << "DCQCN: " << (dcqcn_total_bytes * 100.0) / total_bytes << "%" << endl;
        cout << "Cubic: " << (cubic_total_bytes * 100.0) / total_bytes << "%" << endl;
    }

    // Jain's fairness index across all flows (per-flow throughput)
    cout << "\n=== Jain's Fairness Index (per-flow) ===" << endl;
    vector<double> all_throughputs;
    all_throughputs.insert(all_throughputs.end(), dcqcn_throughputs.begin(), dcqcn_throughputs.end());
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

    // DCQCN-specific stats
    cout << "\n=== DCQCN Protocol Details ===" << endl;
    for (size_t ix = 0; ix < dcqcn_srcs.size(); ix++) {
        DCQCNSrc* src = dynamic_cast<DCQCNSrc*>(dcqcn_srcs[ix]);
        if (src) {
            cout << "  " << src->nodename()
                 << " CNPs=" << src->_cnps_received
                 << " new_pkts=" << src->_new_packets_sent
                 << " rtx_pkts=" << src->_rtx_packets_sent
                 << endl;
        }
    }

    // Clean up
    for (auto* t : dcqcn_triggers) delete t;
    for (auto* t : dcqcn_trackers) delete t;

    for (uint32_t n = 0; n < no_of_nodes; n++) {
        delete[] net_paths[n];
    }
    delete[] net_paths;

    return 0;
}

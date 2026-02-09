// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * Mixed Swift + TCP Cubic simulation for inter-protocol fairness comparison
 * Runs both protocols on the SAME network to measure how they compete for bandwidth
 *
 * Architecture:
 * - Swift uses route-based routing (pre-computed paths, delay-based CC)
 * - TCP Cubic uses route-based routing (pre-computed paths, loss/ECN-based CC)
 * - Both traverse the SAME queues, competing for bandwidth
 * - Swift uses delay signals for congestion control; TCP Cubic uses loss/ECN
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
#include <climits>
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
#include "swift.h"
#include "firstfit.h"
#include "topology.h"
#include "connection_matrix.h"
#include "fat_tree_topology.h"
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
         << " [-swift_ratio 0.0-1.0]"
         << " [-swift_cwnd packets]"
         << " [-cwnd packets]"
         << " [-hystart 0|1]"
         << " [-fast_conv 0|1]"
         << " [-csv csv_output_file]"
         << " [-ecn]"
         << " [-tcp_ecn 0|1]"
         << " [-plb on|off]"
         << " [-subflows N]"
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
    double swift_ratio = 0.5;  // 50% Swift, 50% TCP Cubic by default
    bool enable_ecn = false;

    // Swift parameters
    uint32_t swift_cwnd_pkts = 15;
    bool plb = false;
    uint32_t no_of_subflows = 1;

    // TCP Cubic parameters
    uint32_t cwnd_pkts = 10;
    bool hystart_enabled = true;
    bool fast_convergence = true;
    bool tcp_ecn_enabled = true;

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
        } else if (!strcmp(argv[i], "-swift_ratio")) {
            swift_ratio = atof(argv[i+1]);
            if (swift_ratio < 0.0) swift_ratio = 0.0;
            if (swift_ratio > 1.0) swift_ratio = 1.0;
            cout << "Swift ratio " << swift_ratio << endl;
            i++;
        } else if (!strcmp(argv[i], "-swift_cwnd")) {
            swift_cwnd_pkts = atoi(argv[i+1]);
            cout << "Swift initial cwnd " << swift_cwnd_pkts << " packets" << endl;
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
        } else if (!strcmp(argv[i], "-tcp_ecn")) {
            tcp_ecn_enabled = atoi(argv[i+1]) != 0;
            cout << "TCP Cubic ECN response " << (tcp_ecn_enabled ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-plb")) {
            if (strcmp(argv[i+1], "off") == 0) {
                plb = false;
            } else if (strcmp(argv[i+1], "on") == 0) {
                plb = true;
            } else {
                exit_error(argv[0]);
            }
            cout << "PLB " << (plb ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-subflows")) {
            no_of_subflows = atoi(argv[i+1]);
            cout << "Swift subflows " << no_of_subflows << endl;
            i++;
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

    SwiftSinkLoggerSampling swift_sink_logger(logtime, eventlist);
    logfile.addLogger(swift_sink_logger);

    TcpRtxTimerScanner tcpRtxScanner(timeFromMs(10), eventlist);
    SwiftRtxTimerScanner swiftRtxScanner(timeFromMs(10), eventlist);

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
    // Use COMPOSITE queues so both Swift and TCP share ECN-capable queues.
    // RANDOM queues silently ignore ECN thresholds, making -ecn/-tcp_ecn no-ops.
    // SWIFT_SCHEDULER is used as the sender queue type for Swift pacing.
    unique_ptr<FatTreeTopologyCfg> topo_cfg;
    if (topo_file) {
        topo_cfg = FatTreeTopologyCfg::load(topo_file, memFromPkt(queuesize_pkt), COMPOSITE, SWIFT_SCHEDULER);
        if (topo_cfg->no_of_nodes() != no_of_nodes) {
            cerr << "Mismatch between connection matrix (" << no_of_nodes
                 << " nodes) and topology (" << topo_cfg->no_of_nodes() << " nodes)" << endl;
            exit(1);
        }
        topo_cfg->set_queue_sizes(memFromPkt(queuesize_pkt));
    } else {
        topo_cfg = make_unique<FatTreeTopologyCfg>(no_of_nodes, linkspeed, memFromPkt(queuesize_pkt),
                                                    (uint32_t)0, COMPOSITE, SWIFT_SCHEDULER);
    }

    // Enable ECN — needed for TCP Cubic ECN response; Swift uses delay signals instead
    if (enable_ecn) {
        mem_b queue_bytes = memFromPkt(queuesize_pkt);
        mem_b ecn_low = queue_bytes / 4;
        mem_b ecn_high = queue_bytes * 97 / 100;
        topo_cfg->set_ecn_parameters(true, true, ecn_low, ecn_high);
        cout << "ECN thresholds: low=" << ecn_low << " bytes, high=" << ecn_high << " bytes" << endl;
    }

    cout << *topo_cfg << endl;

    FatTreeTopology* top = new FatTreeTopology(topo_cfg.get(), &qlf, &eventlist, ff);
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    // Pre-compute paths (both Swift and TCP use route-based routing)
    vector<const Route*>*** net_paths;
    net_paths = new vector<const Route*>**[no_of_nodes];
    for (uint32_t n = 0; n < no_of_nodes; n++) {
        net_paths[n] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t m = 0; m < no_of_nodes; m++)
            net_paths[n][m] = NULL;
    }

    // Setup connections - split between Swift and TCP Cubic
    vector<connection*>* all_conns = conns->getAllConnections();

    vector<TcpCubicSrc*> cubic_srcs;
    vector<TcpSink*> cubic_sinks;
    vector<SwiftSrc*> swift_srcs;
    vector<SwiftSink*> swift_sinks;
    vector<FlowRecord> flow_records;

    uint32_t swift_count = 0, cubic_count = 0;
    uint32_t total_conns = all_conns->size();
    uint32_t swift_target = (uint32_t)(total_conns * swift_ratio);

    if (total_conns == 0) {
        cout << "No connections to simulate" << endl;
        return 0;
    }

    // Build a randomized protocol assignment vector to avoid order bias.
    // With order-biased assignment (c < target), the first N flows always get
    // one protocol, which can correlate with traffic matrix structure.
    vector<bool> use_swift_vec(total_conns, false);
    for (uint32_t j = 0; j < swift_target; j++)
        use_swift_vec[j] = true;
    for (uint32_t j = total_conns - 1; j > 0; j--) {
        uint32_t k = rand() % (j + 1);
        swap(use_swift_vec[j], use_swift_vec[k]);
    }

    cout << "Creating " << swift_target << " Swift flows and " << (total_conns - swift_target) << " TCP Cubic flows" << endl;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;

        // Both Swift and TCP connect() take picoseconds
        simtime_picosec starttime = timeFromUs((uint32_t)crt->start);

        // Use randomized protocol assignment
        bool use_swift = use_swift_vec[c];

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

        // Both protocols use route-based routing — get paths
        if (!net_paths[src][dest])
            net_paths[src][dest] = top->get_paths(src, dest);
        if (!net_paths[dest][src])
            net_paths[dest][src] = top->get_paths(dest, src);

        size_t choice = 0;
        if (net_paths[src][dest]->size() > 1)
            choice = rand() % net_paths[src][dest]->size();

        if (use_swift) {
            rec.protocol = "SWIFT";

            SwiftSrc* swiftSrc = new SwiftSrc(swiftRtxScanner, NULL, NULL, eventlist);
            swiftSrc->setName("swift_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*swiftSrc);

            swiftSrc->set_cwnd(swift_cwnd_pkts * Packet::data_packet_size());

            if (crt->size > 0) {
                swiftSrc->set_flowsize(crt->size);  // adds mss internally
            } else {
                swiftSrc->set_flowsize(UINT64_MAX / 2);
            }

            SwiftSink* swiftSnk = new SwiftSink();
            swiftSnk->setName("swift_sink_" + ntoa(src) + "_" + ntoa(dest));
            logfile.writeName(*swiftSnk);

            Route* routeout = new Route(*(net_paths[src][dest]->at(choice)));
            Route* routein = new Route(*net_paths[dest][src]->at(choice));

            if (no_of_subflows == 1) {
                swiftSrc->connect(*routeout, *routein, *swiftSnk, starttime);
            }
            swiftSrc->set_paths(net_paths[src][dest]);
            if (no_of_subflows > 1) {
                swiftSrc->multipath_connect(*swiftSnk, starttime, no_of_subflows);
            }
            if (plb) swiftSrc->enable_plb();

            swift_sink_logger.monitorSink(swiftSnk);

            swift_srcs.push_back(swiftSrc);
            swift_sinks.push_back(swiftSnk);
            swift_count++;

        } else {
            rec.protocol = "CUBIC";

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
            } else {
                tcpSrc->set_flowsize(UINT64_MAX / 2);
            }

            tcpSrc->set_cwnd(cwnd_pkts * Packet::data_packet_size());
            tcpSrc->set_ssthresh(0xffffffff);
            tcpSrc->setHystartEnabled(hystart_enabled);
            tcpSrc->setFastConvergenceEnabled(fast_convergence);
            tcpSrc->setTcpFriendlinessEnabled(true);
            tcpSrc->setEcnEnabled(tcp_ecn_enabled);

            tcpRtxScanner.registerTcp(*tcpSrc);

            routeout->push_back(tcpSnk);
            routein->push_back(tcpSrc);

            tcpSrc->connect(*routeout, *routein, *tcpSnk, starttime);

            tcp_sink_logger->monitorSink(tcpSnk);

            cubic_srcs.push_back(tcpSrc);
            cubic_sinks.push_back(tcpSnk);
            cubic_count++;
        }

        flow_records.push_back(rec);
    }

    cout << "Created " << swift_count << " Swift flows and " << cubic_count << " TCP Cubic flows" << endl;
    cout << "Both protocols share the SAME network queues - they will compete for bandwidth" << endl;

    // Record setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# protocol = MIXED (Swift + TCP Cubic competing)");
    logfile.write("# swift_flows = " + ntoa(swift_count));
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
    uint32_t swift_idx = 0, cubic_idx = 0;
    for (auto& rec : flow_records) {
        if (rec.protocol == "SWIFT") {
            SwiftSrc* src_ptr = swift_srcs[swift_idx];
            SwiftSink* snk_ptr = swift_sinks[swift_idx];
            // SwiftSink::cumulative_ack() returns _cumulative_data_ack + _mss,
            // which overstates by one MSS. Use _cumulative_data_ack directly
            // for accurate cross-protocol byte comparison.
            rec.bytes_received = snk_ptr->_cumulative_data_ack;
            rec.finished = (src_ptr->_finish_time > 0);
            rec.retransmits = src_ptr->drops();
            rec.finish_time = src_ptr->_finish_time;
            swift_idx++;
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

    // Swift statistics
    cout << "\n=== Swift Statistics ===" << endl;
    uint64_t swift_total_bytes = 0;
    uint32_t swift_finished = 0;
    uint64_t swift_retx = 0;
    vector<double> swift_throughputs;
    for (auto& rec : flow_records) {
        if (rec.protocol != "SWIFT") continue;
        swift_total_bytes += rec.bytes_received;
        swift_retx += rec.retransmits;
        if (rec.finished) swift_finished++;
        if (rec.bytes_received > 0) {
            double end_us = (rec.finish_time > 0) ? timeAsUs(rec.finish_time) : timeAsUs(sim_end);
            double elapsed_us = end_us - timeAsUs(rec.start_time);
            if (elapsed_us > 0) {
                swift_throughputs.push_back((rec.bytes_received * 8.0) / (elapsed_us * 1000.0));
            }
        }
    }
    cout << "Swift flows completed: " << swift_finished << "/" << swift_count << endl;
    cout << "Swift total bytes received: " << swift_total_bytes << endl;
    cout << "Swift retransmits: " << swift_retx << endl;
    if (!swift_throughputs.empty()) {
        sort(swift_throughputs.begin(), swift_throughputs.end());
        double sum = accumulate(swift_throughputs.begin(), swift_throughputs.end(), 0.0);
        cout << "Swift per-flow throughput (Gbps): mean=" << sum / swift_throughputs.size()
             << " median=" << swift_throughputs[swift_throughputs.size()/2]
             << " p99=" << swift_throughputs[(size_t)(swift_throughputs.size() * 0.99)]
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

    bool all_still_running = true;
    simtime_picosec earliest_finish = sim_end;
    simtime_picosec latest_finish = 0;

    // Per-protocol earliest start times for accurate window computation.
    // Using global latest_start skews throughput when starts are staggered.
    simtime_picosec swift_earliest_start = sim_end;
    simtime_picosec cubic_earliest_start = sim_end;
    for (auto& rec : flow_records) {
        if (rec.finish_time > 0) {
            all_still_running = false;
            if (rec.finish_time < earliest_finish)
                earliest_finish = rec.finish_time;
            if (rec.finish_time > latest_finish)
                latest_finish = rec.finish_time;
        }
        if (rec.protocol == "SWIFT" && rec.start_time < swift_earliest_start)
            swift_earliest_start = rec.start_time;
        if (rec.protocol == "CUBIC" && rec.start_time < cubic_earliest_start)
            cubic_earliest_start = rec.start_time;
    }

    // Overlap starts when the later protocol's first flow begins
    simtime_picosec latest_start = max(swift_earliest_start, cubic_earliest_start);

    if (all_still_running) {
        // Steady-state mode: all flows still active at sim_end.
        cout << "Mode: STEADY-STATE (all flows active for entire simulation)" << endl;
        cout << "Measurement window: " << timeAsUs(latest_start) << " - " << timeAsUs(sim_end)
             << " us (" << timeAsUs(sim_end - latest_start) << " us)" << endl;

        double total_bytes = swift_total_bytes + cubic_total_bytes;
        if (total_bytes > 0 && swift_count > 0 && cubic_count > 0) {
            double swift_share = (swift_total_bytes * 100.0) / total_bytes;
            double cubic_share = (cubic_total_bytes * 100.0) / total_bytes;

            double window_us = timeAsUs(sim_end - latest_start);
            double swift_gbps = (swift_total_bytes * 8.0) / (window_us * 1000.0);
            double cubic_gbps = (cubic_total_bytes * 8.0) / (window_us * 1000.0);

            cout << "Swift: " << swift_total_bytes << " bytes, "
                 << swift_gbps << " Gbps, share=" << swift_share << "%" << endl;
            cout << "Cubic: " << cubic_total_bytes << " bytes, "
                 << cubic_gbps << " Gbps, share=" << cubic_share << "%" << endl;

            double sum_x = swift_gbps + cubic_gbps;
            double sum_x2 = swift_gbps * swift_gbps + cubic_gbps * cubic_gbps;
            double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
            cout << "Competitive JFI: " << jfi << endl;
        }

    } else {
        // Phase analysis mode
        cout << "Mode: PHASE ANALYSIS (at least one flow completed)" << endl;

        simtime_picosec overlap_end = earliest_finish;
        simtime_picosec overlap_start = latest_start;
        double overlap_us = timeAsUs(overlap_end) - timeAsUs(overlap_start);

        cout << "Phase 1 (overlap): " << timeAsUs(overlap_start) << " - " << timeAsUs(overlap_end)
             << " us (" << overlap_us << " us)" << endl;
        simtime_picosec phase2_end = latest_finish > 0 ? latest_finish : sim_end;
        cout << "Phase 2 (solo):    " << timeAsUs(overlap_end) << " - " << timeAsUs(phase2_end)
             << " us (" << (timeAsUs(phase2_end) - timeAsUs(overlap_end)) << " us)" << endl;

        if (overlap_us > 0 && swift_count > 0 && cubic_count > 0) {
            double phase2_us = timeAsUs(phase2_end) - timeAsUs(overlap_end);

            int64_t swift_phase1_bytes = (int64_t)swift_total_bytes;
            int64_t cubic_phase1_bytes = (int64_t)cubic_total_bytes;

            bool swift_finished_first = false;
            bool cubic_finished_first = false;
            for (auto& rec : flow_records) {
                if (rec.finish_time == earliest_finish) {
                    if (rec.protocol == "SWIFT") swift_finished_first = true;
                    else cubic_finished_first = true;
                }
            }

            // Per-flow Phase 2 estimator: each surviving-protocol flow
            // contributes based on its own average rate and its active time in Phase 2.
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
                    double active_start = max(phase2_start_us, flow_start_us);
                    double active_end = min(phase2_end_us, flow_end_us);
                    double active_us = max(0.0, active_end - active_start);
                    double flow_avg_bps = (double)rec.bytes_received * 8.0 / (flow_lifetime_us / 1e6);
                    total += (uint64_t)(flow_avg_bps * active_us / 1e6 / 8.0);
                }
                return total;
            };

            if (swift_finished_first && !cubic_finished_first) {
                uint64_t phase2_solo_bytes = estimate_phase2_bytes("CUBIC");
                cubic_phase1_bytes = (int64_t)cubic_total_bytes - (int64_t)phase2_solo_bytes;
                if (cubic_phase1_bytes < 0) cubic_phase1_bytes = 0;
                cout << "Swift finished first. Cubic ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated Cubic solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else if (cubic_finished_first && !swift_finished_first) {
                uint64_t phase2_solo_bytes = estimate_phase2_bytes("SWIFT");
                swift_phase1_bytes = (int64_t)swift_total_bytes - (int64_t)phase2_solo_bytes;
                if (swift_phase1_bytes < 0) swift_phase1_bytes = 0;
                cout << "Cubic finished first. Swift ran solo for " << phase2_us << " us" << endl;
                cout << "Estimated Swift solo bytes (Phase 2): " << phase2_solo_bytes << endl;
            } else {
                cout << "Both protocols finished at the same time (or all finished)" << endl;
            }

            double phase1_total = (double)swift_phase1_bytes + (double)cubic_phase1_bytes;
            if (phase1_total > 0) {
                double swift_share = (swift_phase1_bytes * 100.0) / phase1_total;
                double cubic_share = (cubic_phase1_bytes * 100.0) / phase1_total;

                double swift_phase1_gbps = (swift_phase1_bytes * 8.0) / (overlap_us * 1000.0);
                double cubic_phase1_gbps = (cubic_phase1_bytes * 8.0) / (overlap_us * 1000.0);

                cout << "\nCompetitive throughput (Phase 1 only):" << endl;
                cout << "  Swift: " << swift_phase1_bytes << " bytes, " << swift_phase1_gbps << " Gbps" << endl;
                cout << "  Cubic: " << cubic_phase1_bytes << " bytes, " << cubic_phase1_gbps << " Gbps" << endl;
                cout << "\nCompetitive bandwidth share:" << endl;
                cout << "  Swift: " << swift_share << "%" << endl;
                cout << "  Cubic: " << cubic_share << "%" << endl;

                double sum_x = swift_phase1_gbps + cubic_phase1_gbps;
                double sum_x2 = swift_phase1_gbps * swift_phase1_gbps + cubic_phase1_gbps * cubic_phase1_gbps;
                double jfi = (sum_x * sum_x) / (2.0 * sum_x2);
                cout << "Competitive JFI: " << jfi << endl;
            }
        }
    }

    // Raw bandwidth share for reference
    cout << "\n=== Raw Bandwidth Share (total bytes, for reference) ===" << endl;
    double total_bytes = swift_total_bytes + cubic_total_bytes;
    if (total_bytes > 0) {
        cout << "Swift: " << (swift_total_bytes * 100.0) / total_bytes << "%" << endl;
        cout << "Cubic: " << (cubic_total_bytes * 100.0) / total_bytes << "%" << endl;
    }

    // Jain's fairness index across all flows
    cout << "\n=== Jain's Fairness Index (per-flow) ===" << endl;
    vector<double> all_throughputs;
    all_throughputs.insert(all_throughputs.end(), swift_throughputs.begin(), swift_throughputs.end());
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

    // Swift-specific stats
    cout << "\n=== Swift Protocol Details ===" << endl;
    for (size_t ix = 0; ix < swift_srcs.size(); ix++) {
        SwiftSrc* src = swift_srcs[ix];
        cout << "  " << src->str()
             << " drops=" << src->drops()
             << " dsn_sent=" << src->_highest_dsn_sent
             << endl;
    }

    // Clean up
    for (uint32_t n = 0; n < no_of_nodes; n++) {
        delete[] net_paths[n];
    }
    delete[] net_paths;

    return 0;
}

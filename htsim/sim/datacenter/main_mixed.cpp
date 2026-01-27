// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * Mixed NSCC + TCP Cubic simulation for inter-protocol fairness comparison
 * Runs both protocols on the SAME network to measure how they compete for bandwidth
 *
 * Architecture:
 * - NSCC uses switch-based routing (packets forwarded via FatTreeSwitch)
 * - TCP Cubic uses route-based routing (pre-computed paths)
 * - Both traverse the SAME queues, competing for bandwidth
 */
#include "config.h"
#include <sstream>
#include <iostream>
#include <fstream>
#include <string.h>
#include <math.h>
#include <list>
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

void exit_error(char* progr) {
    cout << "Usage " << progr
         << " [-o output_file]"
         << " [-nodes N]"
         << " [-conns N]"
         << " [-tm traffic_matrix_file]"
         << " [-end end_time_in_us]"
         << " [-seed random_seed]"
         << " [-q queue_size_packets]"
         << " [-nscc_ratio 0.0-1.0]"
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
    double nscc_ratio = 0.5;  // 50% NSCC, 50% TCP Cubic by default
    uint32_t ports = 1;
    uint16_t path_entropy_size = 16;
    bool enable_ecn = false;  // ECN off by default, enable with -ecn flag

    char* tm_file = NULL;

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
        } else if (!strcmp(argv[i], "-seed")) {
            seed = atoi(argv[i+1]);
            cout << "random seed " << seed << endl;
            i++;
        } else if (!strcmp(argv[i], "-q")) {
            queuesize_pkt = atoi(argv[i+1]);
            cout << "queue size " << queuesize_pkt << " packets" << endl;
            i++;
        } else if (!strcmp(argv[i], "-nscc_ratio")) {
            nscc_ratio = atof(argv[i+1]);
            cout << "NSCC ratio " << nscc_ratio << endl;
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

    eventlist.setEndtime(timeFromUs((uint32_t)end_time));

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

    // Create topology with switch-based routing for NSCC
    // Using COMPOSITE queue type to support both protocols
    FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);

    unique_ptr<FatTreeTopologyCfg> topo_cfg;
    topo_cfg = make_unique<FatTreeTopologyCfg>(no_of_nodes, linkspeed, memFromPkt(queuesize_pkt), COMPOSITE);

    // Enable ECN if requested
    // Using typical thresholds: low = 25% of queue, high = 97% of queue
    if (enable_ecn) {
        mem_b queue_bytes = memFromPkt(queuesize_pkt);
        mem_b ecn_low = queue_bytes / 4;       // 25% threshold
        mem_b ecn_high = queue_bytes * 97 / 100;  // 97% threshold
        topo_cfg->set_ecn_parameters(true, true, ecn_low, ecn_high);
        cout << "ECN thresholds: low=" << ecn_low << " bytes, high=" << ecn_high << " bytes" << endl;
    }

    cout << *topo_cfg << endl;

    FatTreeTopology* top = new FatTreeTopology(topo_cfg.get(), &qlf, &eventlist, ff);
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    // Calculate network RTT for NSCC initialization
    simtime_picosec network_rtt = topo_cfg->get_diameter_latency() * 2;
    cout << "Network RTT: " << timeAsUs(network_rtt) << " us" << endl;

    // Initialize NSCC parameters
    simtime_picosec target_Qdelay = timeFromUs((uint32_t)5);
    UecSrc::initNsccParams(network_rtt, linkspeed, target_Qdelay, 2, true);

    // Create UecNIC objects for each node (needed for NSCC)
    vector<unique_ptr<UecNIC>> nics;
    for (uint32_t ix = 0; ix < no_of_nodes; ix++) {
        nics.emplace_back(make_unique<UecNIC>(ix, eventlist, linkspeed, ports));
    }

    // Setup connections - split between NSCC and TCP Cubic
    vector<connection*>* all_conns = conns->getAllConnections();

    list<TcpCubicSrc*> cubic_srcs;
    list<TcpSink*> cubic_sinks;
    list<UecSrc*> nscc_srcs;
    list<UecSink*> nscc_sinks;

    uint32_t nscc_count = 0, cubic_count = 0;
    uint32_t total_conns = all_conns->size();
    uint32_t nscc_target = (uint32_t)(total_conns * nscc_ratio);

    cout << "Creating " << nscc_target << " NSCC flows and " << (total_conns - nscc_target) << " TCP Cubic flows" << endl;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;

        simtime_picosec starttime = timeFromUs((uint32_t)crt->start);

        // Decide protocol based on ratio
        bool use_nscc = (c < nscc_target);

        if (use_nscc) {
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
            }

            // Initialize NSCC congestion control
            uecSrc->initNscc(0, network_rtt);

            // Set up switch-based routing
            // Route from source to ToR switch
            Route* srctotor = new Route();
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->pipes_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]);
            srctotor->push_back(top->queues_ns_nlp[src][topo_cfg->HOST_POD_SWITCH(src)][0]->getRemoteEndpoint());

            // Route from destination to ToR switch (for ACKs)
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
            // Create TCP Cubic source and sink with route-based routing
            // Get paths through the SAME topology (same queues as NSCC)
            vector<const Route*>* paths = top->get_paths(src, dest);

            size_t choice = 0;
            if (paths->size() > 1) {
                choice = rand() % paths->size();
            }

            Route* routeout = new Route(*(paths->at(choice)));
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

            tcpSrc->set_cwnd(10 * Packet::data_packet_size());
            tcpSrc->set_ssthresh(0xffffffff);

            tcpRtxScanner.registerTcp(*tcpSrc);

            routeout->push_back(tcpSnk);
            routein->push_back(tcpSrc);

            tcpSrc->connect(*routeout, *routein, *tcpSnk, starttime);

            tcp_sink_logger->monitorSink(tcpSnk);

            cubic_srcs.push_back(tcpSrc);
            cubic_sinks.push_back(tcpSnk);
            cubic_count++;
        }
    }

    cout << "Created " << nscc_count << " NSCC flows and " << cubic_count << " TCP Cubic flows" << endl;
    cout << "Both protocols share the SAME network queues - they will compete for bandwidth" << endl;

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

    cout << "Done" << endl;

    // Collect statistics
    cout << "\n========================================" << endl;
    cout << "INTER-PROTOCOL FAIRNESS RESULTS" << endl;
    cout << "========================================" << endl;

    // NSCC statistics - use sink's bytes_received for accurate byte count
    cout << "\n=== NSCC Statistics ===" << endl;
    uint64_t nscc_total_bytes = 0;
    uint32_t nscc_finished = 0;
    for (auto src : nscc_srcs) {
        if (src->isTotallyFinished()) {
            nscc_finished++;
        }
    }
    // Use sink's total_received() for accurate byte count
    for (auto snk : nscc_sinks) {
        nscc_total_bytes += snk->total_received();
    }
    cout << "NSCC flows completed: " << nscc_finished << "/" << nscc_count << endl;
    cout << "NSCC total bytes received: " << nscc_total_bytes << endl;
    double nscc_throughput_gbps = (nscc_total_bytes * 8.0) / (end_time * 1000.0);  // end_time is in us
    cout << "NSCC aggregate throughput: " << nscc_throughput_gbps << " Gbps" << endl;

    // TCP Cubic statistics
    cout << "\n=== TCP Cubic Statistics ===" << endl;
    uint64_t cubic_total_bytes = 0;
    uint64_t cubic_retx = 0;
    uint32_t cubic_finished = 0;
    for (auto snk : cubic_sinks) {
        cubic_total_bytes += snk->cumulative_ack();
    }
    for (auto src : cubic_srcs) {
        cubic_retx += src->_drops;
        // Check if flow finished by comparing bytes sent to flow size
        if (src->_last_acked >= src->_flow_size && src->_flow_size > 0) {
            cubic_finished++;
        }
    }
    cout << "TCP Cubic flows completed: " << cubic_finished << "/" << cubic_count << endl;
    cout << "TCP Cubic total bytes received: " << cubic_total_bytes << endl;
    cout << "TCP Cubic retransmits: " << cubic_retx << endl;
    double cubic_throughput_gbps = (cubic_total_bytes * 8.0) / (end_time * 1000.0);
    cout << "TCP Cubic aggregate throughput: " << cubic_throughput_gbps << " Gbps" << endl;

    // Fairness comparison
    cout << "\n=== Bandwidth Share Analysis ===" << endl;
    double total_bytes = nscc_total_bytes + cubic_total_bytes;
    if (total_bytes > 0) {
        double nscc_share = (nscc_total_bytes * 100.0) / total_bytes;
        double cubic_share = (cubic_total_bytes * 100.0) / total_bytes;

        cout << "NSCC bandwidth share: " << nscc_share << "%" << endl;
        cout << "TCP Cubic bandwidth share: " << cubic_share << "%" << endl;

        // Expected fair share based on flow count
        double expected_nscc_share = (nscc_count * 100.0) / (nscc_count + cubic_count);
        double expected_cubic_share = (cubic_count * 100.0) / (nscc_count + cubic_count);
        cout << "\nExpected fair share (by flow count):" << endl;
        cout << "  NSCC: " << expected_nscc_share << "%" << endl;
        cout << "  TCP Cubic: " << expected_cubic_share << "%" << endl;

        // Fairness ratio
        double nscc_fairness_ratio = nscc_share / expected_nscc_share;
        double cubic_fairness_ratio = cubic_share / expected_cubic_share;
        cout << "\nFairness ratio (actual/expected):" << endl;
        cout << "  NSCC: " << nscc_fairness_ratio << "x" << endl;
        cout << "  TCP Cubic: " << cubic_fairness_ratio << "x" << endl;

        if (nscc_fairness_ratio > 1.1) {
            cout << "\n** NSCC is getting MORE than its fair share **" << endl;
        } else if (cubic_fairness_ratio > 1.1) {
            cout << "\n** TCP Cubic is getting MORE than its fair share **" << endl;
        } else {
            cout << "\n** Both protocols are getting approximately fair share **" << endl;
        }
    }

    return 0;
}

// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
 * TCP CUBIC simulation entry point for datacenter experiments
 * Based on main_tcp.cpp and main_uec.cpp patterns
 */
#include "config.h"
#include <sstream>
#include <iostream>
#include <fstream>
#include <string.h>
#include <math.h>
#include <list>

#include "network.h"
#include "randomqueue.h"
#include "pipe.h"
#include "eventlist.h"
#include "logfile.h"
#include "loggers.h"
#include "clock.h"
#include "tcpcubic.h"
#include "tcpcubic_transfer.h"
#include "tcp.h"
#include "firstfit.h"
#include "topology.h"
#include "connection_matrix.h"
#include "fat_tree_topology.h"
#include "fat_tree_switch.h"
#include "compositequeue.h"

#define PRINT_PATHS 0
#include "main.h"

uint32_t RTT = 1; // per hop delay in microseconds
uint32_t DEFAULT_NODES = 128;

FirstFit* ff = NULL;
size_t subflow_count = 1;

EventList eventlist;

void exit_error(char* progr) {
    cout << "Usage " << progr
         << " [-o output_file]"
         << " [-nodes N]"
         << " [-conns N]"
         << " [-cwnd packets]"
         << " [-tm traffic_matrix_file]"
         << " [-topo topology_file]"
         << " [-end end_time_in_us]"
         << " [-seed random_seed]"
         << " [-q queue_size_packets]"
         << " [-linkspeed Mbps]"
         << " [-hop_latency us]"
         << " [-switch_latency us]"
         << " [-strat ecmp_host]"
         << " [-log sink|flow_events]"
         << " [-hystart 0|1]"
         << " [-fast_conv 0|1]"
         << " [-tcp_friendly 0|1]"
         << endl;
    exit(1);
}

void print_path(std::ofstream &paths, const Route* rt) {
    for (uint32_t i = 1; i < rt->size() - 1; i += 2) {
        RandomQueue* q = (RandomQueue*)rt->at(i);
        if (q != NULL)
            paths << q->str() << " ";
        else
            paths << "NULL ";
    }
    paths << endl;
}

int main(int argc, char **argv) {
    Clock c(timeFromSec(50 / 100.), eventlist);
    linkspeed_bps linkspeed = speedFromMbps((double)HOST_NIC);
    uint32_t no_of_conns = 0, no_of_nodes = DEFAULT_NODES;
    stringstream filename(ios_base::out);

    int i = 1;
    filename << "logout.dat";

    // Default parameters
    uint32_t cwnd = 10;  // Initial cwnd in packets
    int seed = 13;
    int end_time = 100000; // 100ms in microseconds
    uint32_t queuesize_pkt = 100;
    simtime_picosec hop_latency = timeFromUs((uint32_t)1);
    simtime_picosec switch_latency = timeFromUs((uint32_t)0);
    bool log_sink = false;
    bool log_flow_events = true;
    simtime_picosec logtime = timeFromMs(0.25);

    // CUBIC options
    bool hystart_enabled = true;
    bool fast_convergence = true;
    bool tcp_friendliness = true;

    RouteStrategy route_strategy = NOT_SET;

    char* tm_file = NULL;
    char* topo_file = NULL;

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
        } else if (!strcmp(argv[i], "-cwnd")) {
            cwnd = atoi(argv[i+1]);
            cout << "cwnd " << cwnd << " packets" << endl;
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
            cout << "linkspeed " << linkspeed / 1000000000 << " Gbps" << endl;
            i++;
        } else if (!strcmp(argv[i], "-hop_latency")) {
            hop_latency = timeFromUs(atof(argv[i+1]));
            cout << "hop latency " << timeAsUs(hop_latency) << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-switch_latency")) {
            switch_latency = timeFromUs(atof(argv[i+1]));
            cout << "switch latency " << timeAsUs(switch_latency) << " us" << endl;
            i++;
        } else if (!strcmp(argv[i], "-logtime")) {
            logtime = timeFromMs(atof(argv[i+1]));
            cout << "logtime " << timeAsMs(logtime) << " ms" << endl;
            i++;
        } else if (!strcmp(argv[i], "-log")) {
            if (!strcmp(argv[i+1], "sink")) {
                log_sink = true;
                cout << "logging sink" << endl;
            } else if (!strcmp(argv[i+1], "flow_events")) {
                log_flow_events = true;
                cout << "logging flow events" << endl;
            }
            i++;
        } else if (!strcmp(argv[i], "-hystart")) {
            hystart_enabled = atoi(argv[i+1]) != 0;
            cout << "HyStart " << (hystart_enabled ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-fast_conv")) {
            fast_convergence = atoi(argv[i+1]) != 0;
            cout << "Fast convergence " << (fast_convergence ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-tcp_friendly")) {
            tcp_friendliness = atoi(argv[i+1]) != 0;
            cout << "TCP friendliness " << (tcp_friendliness ? "enabled" : "disabled") << endl;
            i++;
        } else if (!strcmp(argv[i], "-strat")) {
            if (!strcmp(argv[i+1], "ecmp_host")) {
                route_strategy = ECMP_FIB;
                FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
            } else if (!strcmp(argv[i+1], "single")) {
                route_strategy = SINGLE_PATH;
            } else {
                cout << "Unknown routing strategy: " << argv[i+1] << endl;
                exit_error(argv[0]);
            }
            cout << "routing strategy: " << argv[i+1] << endl;
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

    if (route_strategy == NOT_SET) {
        route_strategy = ECMP_FIB;
        FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
    }

    // Prepare loggers
    cout << "Logging to " << filename.str() << endl;
    Logfile logfile(filename.str(), eventlist);

#if PRINT_PATHS
    filename << ".paths";
    cout << "Logging path choices to " << filename.str() << endl;
    std::ofstream paths(filename.str().c_str());
    if (!paths) {
        cout << "Can't open for writing paths file!" << endl;
        exit(1);
    }
#endif

    logfile.setStartTime(timeFromSec(0));

    TcpSinkLoggerSampling* sink_logger = NULL;
    if (log_sink) {
        sink_logger = new TcpSinkLoggerSampling(logtime, eventlist);
        logfile.addLogger(*sink_logger);
    }

    TcpCubicSrc* tcpSrc;
    TcpSink* tcpSnk;
    Route* routeout, *routein;

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

    if (conns->N != no_of_nodes && no_of_nodes != 0) {
        cout << "Connection matrix nodes " << conns->N << " vs requested " << no_of_nodes << endl;
        no_of_nodes = conns->N;
    }

    no_of_nodes = conns->N;
    cout << "Using " << no_of_nodes << " nodes" << endl;

    // Load or create topology
    unique_ptr<FatTreeTopologyCfg> topo_cfg;
    if (topo_file) {
        topo_cfg = FatTreeTopologyCfg::load(topo_file, memFromPkt(queuesize_pkt), RANDOM, FAIR_PRIO);
        if (topo_cfg->no_of_nodes() != no_of_nodes) {
            cerr << "Mismatch between connection matrix (" << no_of_nodes
                 << " nodes) and topology (" << topo_cfg->no_of_nodes() << " nodes)" << endl;
            exit(1);
        }
        topo_cfg->set_queue_sizes(memFromPkt(queuesize_pkt));
    } else {
        // Create default 3-tier fat tree
        topo_cfg = make_unique<FatTreeTopologyCfg>(3, no_of_nodes, linkspeed, memFromPkt(queuesize_pkt),
                                                   hop_latency, switch_latency,
                                                   RANDOM, FAIR_PRIO);
    }

    cout << *topo_cfg << endl;

    FatTreeTopology* top = new FatTreeTopology(topo_cfg.get(), &qlf, &eventlist, ff);
    no_of_nodes = top->no_of_nodes();
    cout << "actual nodes " << no_of_nodes << endl;

    // Create paths matrix
    vector<const Route*>*** net_paths;
    net_paths = new vector<const Route*>**[no_of_nodes];

    for (uint32_t i = 0; i < no_of_nodes; i++) {
        net_paths[i] = new vector<const Route*>*[no_of_nodes];
        for (uint32_t j = 0; j < no_of_nodes; j++)
            net_paths[i][j] = NULL;
    }

    // Setup connections
    vector<connection*>* all_conns = conns->getAllConnections();

    list<TcpCubicSrc*> cubic_srcs;

    for (size_t c = 0; c < all_conns->size(); c++) {
        connection* crt = all_conns->at(c);
        int src = crt->src;
        int dest = crt->dst;

        if (!net_paths[src][dest])
            net_paths[src][dest] = top->get_paths(src, dest);

        // Create TCP CUBIC source and sink
        // Using non-transfer variant for now - TODO: fix TcpCubicSrcTransfer
        tcpSrc = new TcpCubicSrc(NULL, NULL, eventlist);
        tcpSnk = new TcpSink();
        if (crt->size > 0) {
            tcpSrc->set_flowsize(crt->size);
        }

        // Configure CUBIC options
        tcpSrc->setHystartEnabled(hystart_enabled);
        tcpSrc->setFastConvergenceEnabled(fast_convergence);
        tcpSrc->setTcpFriendlinessEnabled(tcp_friendliness);

        // Set initial cwnd
        tcpSrc->set_cwnd(cwnd * Packet::data_packet_size());
        tcpSrc->set_ssthresh(0xffffffff);

        tcpSrc->setName("cubic_" + ntoa(src) + "_" + ntoa(dest));
        logfile.writeName(*tcpSrc);

        tcpSnk->setName("cubic_sink_" + ntoa(src) + "_" + ntoa(dest));
        logfile.writeName(*tcpSnk);

        tcpRtxScanner.registerTcp(*tcpSrc);

        cubic_srcs.push_back(tcpSrc);

        // Choose route
        size_t choice = 0;
        if (net_paths[src][dest]->size() > 1) {
            choice = rand() % net_paths[src][dest]->size();
        }

#if PRINT_PATHS
        paths << "Route from " << ntoa(src) << " to " << ntoa(dest) << " (" << choice << ") -> ";
        print_path(paths, net_paths[src][dest]->at(choice));
#endif

        routeout = new Route(*(net_paths[src][dest]->at(choice)));
        routeout->push_back(tcpSnk);

        routein = new Route();
        routein->push_back(tcpSrc);

        simtime_picosec starttime = timeFromUs((uint32_t)crt->start);
        tcpSrc->connect(*routeout, *routein, *tcpSnk, starttime);

        if (log_sink && sink_logger) {
            sink_logger->monitorSink(tcpSnk);
        }
    }

    // Record setup
    int pktsize = Packet::data_packet_size();
    logfile.write("# pktsize=" + ntoa(pktsize) + " bytes");
    logfile.write("# cwnd=" + ntoa(cwnd) + " packets");
    logfile.write("# hostnicrate = " + ntoa(linkspeed/1000000) + " Mbps");
    logfile.write("# protocol = TCP CUBIC");

    // Run simulation
    cout << "Starting simulation" << endl;
    while (eventlist.doNextEvent()) {
    }

    cout << "Done" << endl;

    // Print statistics
    uint64_t total_sent = 0, total_retx = 0;
    for (auto src : cubic_srcs) {
        total_sent += src->_packets_sent;
        total_retx += src->_drops;
    }

    cout << "Total packets sent: " << total_sent / pktsize << endl;
    cout << "Total retransmits: " << total_retx << endl;

    return 0;
}

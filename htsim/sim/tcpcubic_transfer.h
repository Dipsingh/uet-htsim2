// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#ifndef TCP_CUBIC_TRANSFER_H
#define TCP_CUBIC_TRANSFER_H

/*
 * TCP CUBIC transfer variant for finite flows
 * Based on tcp_transfer.h pattern
 */

#include <list>
#include <vector>
#include <sstream>
#include <iostream>
#include "config.h"
#include "network.h"
#include "eventlist.h"
#include "tcpcubic.h"

class TcpCubicSinkTransfer;

class TcpCubicSrcTransfer : public TcpCubicSrc {
public:
    TcpCubicSrcTransfer(TcpLogger* logger, TrafficLogger* pktLogger, EventList &eventlist,
                        uint64_t bytes_to_send, vector<const Route*>* paths = NULL,
                        EventSource* stopped = NULL);

    virtual void connect(const Route& routeout, const Route& routeback, TcpSink& sink, simtime_picosec starttime) override;

    virtual void rtx_timer_hook(simtime_picosec now, simtime_picosec period) override;
    virtual void receivePacket(Packet& pkt) override;
    void reset(uint64_t bytes, int shouldRestart);
    virtual void doNextEvent() override;

    uint64_t _bytes_to_send;
    bool _is_active;
    simtime_picosec _started;
    vector<const Route*>* _paths;
    EventSource* _flow_stopped;
};

class TcpCubicSinkTransfer : public TcpSink {
    friend class TcpCubicSrcTransfer;
public:
    TcpCubicSinkTransfer();

    void reset();
};

#endif // TCP_CUBIC_TRANSFER_H

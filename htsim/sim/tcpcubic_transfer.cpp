// -*- c-basic-offset: 4; indent-tabs-mode: nil -*-
#include "tcpcubic_transfer.h"
#include "mtcp.h"
#include <iostream>
#include "config.h"

////////////////////////////////////////////////////////////////
//  TCP CUBIC TRANSFER SOURCE
////////////////////////////////////////////////////////////////

TcpCubicSrcTransfer::TcpCubicSrcTransfer(TcpLogger* logger, TrafficLogger* pktLogger,
                                         EventList &eventlist, uint64_t bytes_to_send,
                                         vector<const Route*>* paths, EventSource* stopped)
    : TcpCubicSrc(logger, pktLogger, eventlist)
{
    _is_active = false;
    _ssthresh = 0xffffffff;
    _bytes_to_send = bytes_to_send;
    set_flowsize(_bytes_to_send + _mss);
    _paths = paths;
    _flow_stopped = stopped;
}

void TcpCubicSrcTransfer::reset(uint64_t bb, int shouldRestart) {
    bictcp_hystart_reset();
    _sawtooth = 0;
    _rtt_avg = timeFromMs(0);
    _rtt_cum = timeFromMs(0);
    _highest_sent = 0;
    _effcwnd = 0;
    _ssthresh = 0xffffffff;
    _last_acked = 0;
    _dupacks = 0;
    _mdev = 0;
    _rto = timeFromMs(3000);
    _recoverq = 0;
    _in_fast_recovery = false;
    _established = false;

    _rtx_timeout_pending = false;
    _RFC2988_RTO_timeout = timeInf;

    if (shouldRestart)
        eventlist().sourceIsPendingRel(*this, timeFromMs(1));
}

void TcpCubicSrcTransfer::connect(const Route& routeout, const Route& routeback,
                                  TcpSink& sink, simtime_picosec starttime) {
    _is_active = false;
    TcpCubicSrc::connect(routeout, routeback, sink, starttime);
}

void TcpCubicSrcTransfer::doNextEvent() {
    if (!_is_active) {
        _is_active = true;

        if (_paths != NULL) {
            Route* rt = new Route(*(_paths->at(rand() % _paths->size())));
            rt->push_back(_sink);
            _route = rt;
        }

        ((TcpCubicSinkTransfer*)_sink)->reset();
        _started = eventlist().now();
        startflow();
    } else {
        TcpCubicSrc::doNextEvent();
    }
}

void TcpCubicSrcTransfer::receivePacket(Packet& pkt) {
    if (_is_active) {
        TcpCubicSrc::receivePacket(pkt);

        if (_bytes_to_send > 0) {
            if (!_mSrc && _last_acked >= _bytes_to_send) {
                _is_active = false;

                cout << "Flow " << _bytes_to_send << " finished after "
                     << timeAsMs(eventlist().now() - _started) << " ms" << endl;

                if (_flow_stopped) {
                    _flow_stopped->doNextEvent();
                } else {
                    reset(_bytes_to_send, 1);
                }
            }
        }
    } else {
        pkt.flow().logTraffic(pkt, *this, TrafficLogger::PKT_RCVDESTROY);
        pkt.free();
    }
}

void TcpCubicSrcTransfer::rtx_timer_hook(simtime_picosec now, simtime_picosec period) {
    if (!_is_active)
        return;

    if (now <= _RFC2988_RTO_timeout || _RFC2988_RTO_timeout == timeInf)
        return;
    if (_highest_sent == 0)
        return;

    cout << "CubicTransfer timeout: active " << _is_active
         << " bytes to send " << _bytes_to_send
         << " sent " << _last_acked
         << " established? " << _established
         << " HSENT " << _highest_sent << endl;

    TcpCubicSrc::rtx_timer_hook(now, period);
}

////////////////////////////////////////////////////////////////
//  TCP CUBIC TRANSFER SINK
////////////////////////////////////////////////////////////////

TcpCubicSinkTransfer::TcpCubicSinkTransfer() : TcpSink() {
}

void TcpCubicSinkTransfer::reset() {
    _cumulative_ack = 0;
    _received.clear();
}

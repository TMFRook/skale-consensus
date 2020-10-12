/*
    Copyright (C) 2018-2019 SKALE Labs

    This file is part of skale-consensus.

    skale-consensus is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    skale-consensus is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with skale-consensus.  If not, see <https://www.gnu.org/licenses/>.

    @file MonitoringAgent.cpp
    @author Stan Kladko
    @date 2018
*/

#include "SkaleCommon.h"
#include "Log.h"
#include "exceptions/ExitRequestedException.h"
#include "exceptions/FatalError.h"
#include "thirdparty/json.hpp"
#include <node/ConsensusEngine.h>

#include "utils/Time.h"
#include "node/Node.h"
#include "chains/Schain.h"
#include "LivelinessMonitor.h"
#include "MonitoringAgent.h"
#include "MonitoringThreadPool.h"


MonitoringAgent::MonitoringAgent(Schain &_sChain) : Agent(_sChain, false, true) {
    try {
        logThreadLocal_ = _sChain.getNode()->getLog();
        this->sChain = &_sChain;



        this->monitoringThreadPool = make_shared<MonitoringThreadPool>(1, this);
        monitoringThreadPool->startService();

    } catch (...) {
        throw_with_nested(FatalError(__FUNCTION__, __CLASS_NAME__));
    }

}


void MonitoringAgent::monitor() {

    if (ConsensusEngine::isOnTravis())
        return;



    map<uint64_t, weak_ptr<LivelinessMonitor>> monitorsCopy;

    {
        LOCK(m)

        lock_guard<recursive_mutex> lock(m);

        monitorsCopy = activeMonitors;

    }

    for (auto &&item: monitorsCopy ) {

        if( sChain->getNode()->isExitRequested() )
            return;

        ptr<LivelinessMonitor> monitor = item.second.lock();

        if (monitor) {

            CHECK_STATE(monitor != nullptr);

            auto currentTime = Time::getCurrentTimeMs();

            if (currentTime > monitor->getExpiryTime()) {
                LOG(warn,
                    monitor->toString() + " has been stuck for " + to_string(currentTime - monitor->getStartTime()) +
                    " ms");
            }
        }
    }


}

void MonitoringAgent::monitoringLoop(MonitoringAgent *_agent) {


    CHECK_ARGUMENT(_agent);

    setThreadName("MonitoringLoop", _agent->getSchain()->getNode()->getConsensusEngine());


    LOG(info, "Monitoring agent started monitoring");


    try {
        while (!_agent->getSchain()->getNode()->isExitRequested()) {
            usleep(_agent->getSchain()->getNode()->getMonitoringIntervalMs() * 1000);

            try {
                _agent->monitor();

                auto blockId = _agent->getSchain()->getLastCommittedBlockID() + 1;

                if (blockId == 1) {
                    try {
                        sleep(30);
                        LOG(info, "Timeout");
                        _agent->getSchain()->blockProposalReceiptTimeoutArrived(blockId);
                    } catch (...) {}
                }

                if (blockId > 1  || Time::getCurrentTimeMs() - _agent->getSchain()->getLastCommitTimeMs()
                                   > BLOCK_PROPOSAL_RECEIVE_TIMEOUT_MS) {
                    try {
                         LOG(info, "Timeout");
                        _agent->getSchain()->blockProposalReceiptTimeoutArrived(blockId);
                    } catch (...) {}
                }

            } catch (ExitRequestedException &) {
                return;
            } catch (exception &e) {
                SkaleException::logNested(e);
            }

        };
    } catch (FatalError& e) {
        _agent->getSchain()->getNode()->exitOnFatalError(e.getMessage());
    }
}

void MonitoringAgent::registerMonitor(ptr<LivelinessMonitor> _m) {

    CHECK_ARGUMENT(_m)
    LOCK(m)
    activeMonitors[_m->getId()] = _m;
}

void MonitoringAgent::unregisterMonitor(uint64_t _id) {

    LOCK(m)

    activeMonitors.erase(_id);

}


void MonitoringAgent::join() {
    CHECK_STATE(monitoringThreadPool);
    monitoringThreadPool->joinAll();
}

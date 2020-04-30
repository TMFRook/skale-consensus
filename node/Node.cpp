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

    @file Node.cpp
    @author Stan Kladko
    @date 2018
*/

#include "leveldb/db.h"

#include "Log.h"
#include "SkaleCommon.h"


#include "exceptions/ExitRequestedException.h"
#include "exceptions/FatalError.h"
#include "exceptions/InvalidArgumentException.h"
#include "exceptions/ParsingException.h"
#include "thirdparty/json.hpp"

#include "protocols/blockconsensus/BlockConsensusAgent.h"

#include "chains/TestConfig.h"
#include "crypto/bls_include.h"
#include "libBLS/bls/BLSPrivateKeyShare.h"
#include "libBLS/bls/BLSPublicKey.h"

#include "blockproposal/server/BlockProposalServerAgent.h"
#include "messages/NetworkMessageEnvelope.h"
#include "chains/Schain.h"
#include "network/Sockets.h"
#include "network/TCPServerSocket.h"
#include "network/ZMQNetwork.h"
#include "network/ZMQServerSocket.h"
#include "node/NodeInfo.h"
#include "catchup/server/CatchupServerAgent.h"
#include "messages/Message.h"
#include "db/BlockDB.h"
#include "db/DASigShareDB.h"
#include "db/DAProofDB.h"
#include "db/PriceDB.h"
#include "db/RandomDB.h"
#include "db/SigDB.h"
#include "db/ProposalHashDB.h"
#include "db/ProposalVectorDB.h"
#include "db/BlockSigShareDB.h"
#include "db/BlockProposalDB.h"
#include "db/MsgDB.h"
#include "db/ConsensusStateDB.h"
#include "ConsensusEngine.h"
#include "ConsensusInterface.h"
#include "Node.h"

using namespace std;

Node::Node(const nlohmann::json &_cfg, ConsensusEngine *_consensusEngine,
           bool _useSGX, ptr<string> _keyName, ptr<vector<string>> _publicKeys) {

    if (_useSGX) {
        CHECK_ARGUMENT(_keyName && _publicKeys);
        useSGX = true;
        keyName = _keyName;
        publicKeys = _publicKeys;
    }

    this->consensusEngine = _consensusEngine;
    this->nodeInfosByIndex = make_shared<map<uint64_t , ptr<NodeInfo> > >();
    this->nodeInfosById = make_shared<map<uint64_t , ptr<NodeInfo>> >();

    this->startedServers = false;
    this->startedClients = false;
    this->exitRequested = false;
    this->cfg = _cfg;

    try {
        initParamsFromConfig();
    } catch (...) {
        throw_with_nested(ParsingException("Could not parse params", __CLASS_NAME__));
    }
    initLogging();
}

void Node::initLevelDBs() {
    auto dbDir = *consensusEngine->getDbDir();
    string blockDBPrefix = "blocks_" + to_string(nodeID) + ".db";
    string randomDBPrefix = "randoms_" + to_string(nodeID) + ".db";
    string priceDBPrefix = "prices_" + to_string(nodeID) + ".db";
    string proposalHashDBPrefix = "/proposal_hashes_" + to_string(nodeID) + ".db";
    string proposalVectorDBPrefix = "/proposal_vectors_" + to_string(nodeID) + ".db";
    string outgoingMsgDBPrefix = "/outgoing_msgs_" + to_string(nodeID) + ".db";
    string incomingMsgDBPrefix = "/incoming_msgs_" + to_string(nodeID) + ".db";
    string consensusStateDBPrefix = "/consensus_state_" + to_string(nodeID) + ".db";
    string blockSigShareDBPrefix = "/block_sigshares_" + to_string(nodeID) + ".db";
    string daSigShareDBPrefix = "/da_sigshares_" + to_string(nodeID) + ".db";
    string daProofDBPrefix = "/da_proofs_" + to_string(nodeID) + ".db";
    string blockProposalDBPrefix = "/block_proposals_" + to_string(nodeID) + ".db";


    blockDB = make_shared<BlockDB>(getSchain(), dbDir, blockDBPrefix, getNodeID(), getBlockDBSize());
    randomDB = make_shared<RandomDB>(getSchain(), dbDir, randomDBPrefix, getNodeID(), getRandomDBSize());
    priceDB = make_shared<PriceDB>(getSchain(), dbDir, priceDBPrefix, getNodeID(), getPriceDBSize());
    proposalHashDB = make_shared<ProposalHashDB>(getSchain(), dbDir, proposalHashDBPrefix, getNodeID(),
                                                 getProposalHashDBSize());
    proposalVectorDB = make_shared<ProposalVectorDB>(getSchain(), dbDir, proposalVectorDBPrefix, getNodeID(),
                                                 getProposalVectorDBSize());

    outgoingMsgDB = make_shared<MsgDB>(getSchain(), dbDir, outgoingMsgDBPrefix, getNodeID(),
                                       getOutgoingMsgDBSize());

    incomingMsgDB = make_shared<MsgDB>(getSchain(), dbDir, incomingMsgDBPrefix, getNodeID(),
                                       getIncomingMsgDBSize());

    consensusStateDB = make_shared<ConsensusStateDB>(getSchain(), dbDir, consensusStateDBPrefix, getNodeID(),
                                       getConsensusStateDBSize());


    blockSigShareDB = make_shared<BlockSigShareDB>(getSchain(), dbDir, blockSigShareDBPrefix, getNodeID(),
                                                   getBlockSigShareDBSize());
    daSigShareDB = make_shared<DASigShareDB>(getSchain(), dbDir, daSigShareDBPrefix, getNodeID(),
                                             getDaSigShareDBSize());
    daProofDB = make_shared<DAProofDB>(getSchain(), dbDir, daProofDBPrefix, getNodeID(), getDaProofDBSize());
    blockProposalDB = make_shared<BlockProposalDB>(getSchain(), dbDir, blockProposalDBPrefix, getNodeID(),
                                                   getBlockProposalDBSize());

}

void Node::initLogging() {
    log = make_shared<Log>(nodeID, getConsensusEngine());

    if (cfg.find("logLevel") != cfg.end()) {
        ptr<string> logLevel = make_shared<string>(cfg.at("logLevel").get<string>());
        log->setGlobalLogLevel(*logLevel);
    }

    for (auto &&item : log->loggers) {
        string category = "logLevel" + item.first;
        if (cfg.find(category) != cfg.end()) {
            ptr<string> logLevel = make_shared<string>(cfg.at(category).get<string>());
            LOG(info, "Setting log level:" + category + ":" + *logLevel);
            log->loggers[item.first]->set_level(Log::logLevelFromString(*logLevel));
        }
    }
}


void Node::initParamsFromConfig() {
    nodeID = cfg.at("nodeID").get<uint64_t>();
    name = make_shared<string>(cfg.at("nodeName").get<string>());
    bindIP = make_shared<string>(cfg.at("bindIP").get<string>());
    basePort = network_port(cfg.at("basePort").get<int>());

    catchupIntervalMS = getParamUint64("catchupIntervalMs", CATCHUP_INTERVAL_MS);
    monitoringIntervalMS = getParamUint64("monitoringIntervalMs", MONITORING_INTERVAL_MS);
    waitAfterNetworkErrorMs = getParamUint64("waitAfterNetworkErrorMs", WAIT_AFTER_NETWORK_ERROR_MS);
    blockProposalHistorySize = getParamUint64("blockProposalHistorySize", BLOCK_PROPOSAL_HISTORY_SIZE);
    committedTransactionsHistory = getParamUint64("committedTransactionsHistory", COMMITTED_TRANSACTIONS_HISTORY);
    maxCatchupDownloadBytes = getParamUint64("maxCatchupDownloadBytes", MAX_CATCHUP_DOWNLOAD_BYTES);
    maxTransactionsPerBlock = getParamUint64("maxTransactionsPerBlock", MAX_TRANSACTIONS_PER_BLOCK);
    minBlockIntervalMs = getParamUint64("minBlockIntervalMs", MIN_BLOCK_INTERVAL_MS);
    blockDBSize = getParamUint64("blockDBSize", BLOCK_DB_SIZE);
    proposalHashDBSize = getParamUint64("proposalHashDBSize", PROPOSAL_HASH_DB_SIZE);
    proposalVectorDBSize = getParamUint64("proposalVectorDBSize", PROPOSAL_VECTOR_DB_SIZE);
    outgoingMsgDBSize = getParamUint64("outgoingMsgDBSize", OUTGOING_MSG_DB_SIZE);
    incomingMsgDBSize = getParamUint64("incomingMsgDBSize", INCOMING_MSG_DB_SIZE);
    consensusStateDBSize = getParamUint64("consensusStateDBSize", CONSENSUS_STATE_DB_SIZE);

    blockSigShareDBSize = getParamUint64("blockSigShareDBSize", BLOCK_SIG_SHARE_DB_SIZE);
    daSigShareDBSize = getParamUint64("daSigShareDBSize", DA_SIG_SHARE_DB_SIZE);
    daProofDBSize = getParamUint64("daProofDBSize", DA_PROOF_DB_SIZE);
    randomDBSize = getParamUint64("randomDBSize", RANDOM_DB_SIZE);
    priceDBSize = getParamUint64("priceDBSize", PRICE_DB_SIZE);
    blockProposalDBSize = getParamUint64("blockProposalDBSize", BLOCK_PROPOSAL_DB_SIZE);


    simulateNetworkWriteDelayMs = getParamInt64("simulateNetworkWriteDelayMs", 0);

    testConfig = make_shared<TestConfig>(cfg);
}

uint64_t Node::getProposalHashDBSize() const {
    return proposalHashDBSize;
}

uint64_t Node::getProposalVectorDBSize() const {
    return proposalVectorDBSize;
}

uint64_t Node::getOutgoingMsgDBSize() const {
    return outgoingMsgDBSize;
}

uint64_t Node::getIncomingMsgDBSize() const {
    return incomingMsgDBSize;
}

Node::~Node() {}


void Node::startServers() {
    initBLSKeys();

    ASSERT(!startedServers);

    LOG(info, "Starting node");

    LOG(trace, "Initing sockets");

    this->sockets = make_shared<Sockets>(*this);

    sockets->initSockets(bindIP, (uint16_t) basePort);

    LOG(trace, "Constructing servers");

    sChain->constructServers(sockets);

    LOG(trace, " Creating consensus network");

    network = make_shared<ZMQNetwork>(*sChain);

    LOG(trace, " Starting consensus messaging");

    network->startThreads();

    LOG(trace, "Starting schain");

    sChain->startThreads();

    LOG(trace, "Releasing server threads");

    releaseGlobalServerBarrier();
}

void Node::initBLSKeys() {
    auto prkStr = consensusEngine->getBlsPrivateKey();
    auto pbkStr1 = consensusEngine->getBlsPublicKey1();
    auto pbkStr2 = consensusEngine->getBlsPublicKey2();
    auto pbkStr3 = consensusEngine->getBlsPublicKey3();
    auto pbkStr4 = consensusEngine->getBlsPublicKey4();

    if (prkStr.size() > 0 && pbkStr1.size() > 0 && pbkStr2.size() > 0 && pbkStr3.size() > 0 &&
        pbkStr4.size() > 0) {
    } else {
        isBLSEnabled = true;
        try {
            prkStr = cfg.at("insecureTestBLSPrivateKey").get<string>();
            pbkStr1 = cfg.at("insecureTestBLSPublicKey1").get<string>();
            pbkStr2 = cfg.at("insecureTestBLSPublicKey2").get<string>();
            pbkStr3 = cfg.at("insecureTestBLSPublicKey3").get<string>();
            pbkStr4 = cfg.at("insecureTestBLSPublicKey4").get<string>();
        } catch (exception &e) {
            isBLSEnabled = false;
            /*throw_with_nested(ParsingException(
                    "Could not find bls key. You need to set it through either skaled or config
               file\n" + string(e.what()), __CLASS_NAME__));
                    */
        }


        if (prkStr.size() > 0 && pbkStr1.size() > 0 && pbkStr2.size() > 0 && pbkStr3.size() > 0 &&
            pbkStr4.size() > 0) {
        } else {
            isBLSEnabled = false;
            /*
            throw FatalError("Empty bls key. You need to set it through either skaled or config
            file");
             */
        }
    }

    if (isBLSEnabled) {
        blsPrivateKey = make_shared<BLSPrivateKeyShare>(
                prkStr, sChain->getTotalSigners(), sChain->getRequiredSigners());

        auto publicKeyStr = make_shared<vector<string> >();

        publicKeyStr->push_back(pbkStr1);
        publicKeyStr->push_back(pbkStr2);
        publicKeyStr->push_back(pbkStr3);
        publicKeyStr->push_back(pbkStr4);

        blsPublicKey = make_shared<BLSPublicKey>(
                publicKeyStr, sChain->getTotalSigners(), sChain->getRequiredSigners());
    }
}

void Node::startClients() {
    sChain->healthCheck();
    releaseGlobalClientBarrier();
}

void Node::setNodeInfo(ptr<NodeInfo> _nodeInfo) {

    CHECK_ARGUMENT(_nodeInfo);
    (*nodeInfosByIndex)[(uint64_t)_nodeInfo->getSchainIndex()] =  _nodeInfo;
    (*nodeInfosById)[(uint64_t ) _nodeInfo->getNodeID()] = _nodeInfo;
}

void Node::setSchain(ptr<Schain> _schain) {
    assert (this->sChain == nullptr);
    this->sChain = _schain;
    initLevelDBs();

}

void Node::initSchain(ptr<Node> _node, ptr<NodeInfo> _localNodeInfo, const vector<ptr<NodeInfo> > &remoteNodeInfos,
                      ConsensusExtFace *_extFace) {
    try {
        logThreadLocal_ = _node->getLog();

        for (auto &rni : remoteNodeInfos) {
            LOG(debug, "Adding Node Info:" + to_string(rni->getSchainIndex()));
            _node->setNodeInfo(rni);
            LOG(debug, "Got IP" + *rni->getBaseIP());
        }

        auto sChain = make_shared<Schain>(
                _node, _localNodeInfo->getSchainIndex(), _localNodeInfo->getSchainID(), _extFace);

        _node->setSchain(sChain);

        sChain->createBlockConsensusInstance();

    } catch (...) {
        throw_with_nested(FatalError(__FUNCTION__, __CLASS_NAME__));
    }

}


void Node::waitOnGlobalServerStartBarrier(Agent *agent) {
    logThreadLocal_ = agent->getSchain()->getNode()->getLog();


    std::unique_lock<std::mutex> mlock(threadServerCondMutex);
    while (!startedServers) {
        threadServerConditionVariable.wait(mlock);
    }
}

void Node::releaseGlobalServerBarrier() {
    std::lock_guard<std::mutex> lock(threadServerCondMutex);

    startedServers = true;
    threadServerConditionVariable.notify_all();
}


void Node::waitOnGlobalClientStartBarrier() {
    logThreadLocal_ = getLog();

    std::unique_lock<std::mutex> mlock(threadClientCondMutex);
    while (!startedClients) {
        threadClientConditionVariable.wait(mlock);
    }
}


void Node::releaseGlobalClientBarrier() {
    std::lock_guard<std::mutex> lock(threadClientCondMutex);

    //    ASSERT(!startedClients);

    startedClients = true;
    threadClientConditionVariable.notify_all();
}

void Node::exit() {
    // exit forcefully after timeout
    std::timed_mutex& mutex = getSchain()->getConsensusWorkingMutex();
    bool locked = mutex.try_lock_for( std::chrono::seconds(EXIT_FORCEFULLTY_SECONDS) );
    auto lock = locked ?
                std::make_unique<std::lock_guard<std::timed_mutex>>(mutex, std::adopt_lock) :
                std::unique_ptr<std::lock_guard<std::timed_mutex>>();

    if (exitRequested) {
        assert(false);           // shouldn't be!
        return;
    }

    exitRequested = true;

    if(!locked)
        LOG(warn, "Forcefully exiting Node after " + to_string(EXIT_FORCEFULLTY_SECONDS) + " seconds");

    releaseGlobalClientBarrier();
    releaseGlobalServerBarrier();
    LOG(info, "Exit requested");

    closeAllSocketsAndNotifyAllAgentsAndThreads();

}


void Node::closeAllSocketsAndNotifyAllAgentsAndThreads() {
    getSchain()->getNode()->threadServerConditionVariable.notify_all();

    ASSERT(agents.size() > 0);

    for (auto &&agent : agents) {
        agent->notifyAllConditionVariables();
    }

    if (sockets && sockets->blockProposalSocket)
        sockets->blockProposalSocket->touch();

    if (sockets && sockets->catchupSocket)
        sockets->catchupSocket->touch();
}


void Node::registerAgent(Agent *_agent) {
    agents.push_back(_agent);
}


void Node::exitCheck() {
    if (exitRequested) {
        BOOST_THROW_EXCEPTION(ExitRequestedException( __CLASS_NAME__ ));
    }
}

void Node::exitOnFatalError(const string &_message) {
    if (exitRequested)
        return;
    exit();

    //    consensusEngine->joinAll();
    auto extFace = consensusEngine->getExtFace();

    if (extFace) {
        extFace->terminateApplication();
    }
    LOG(critical, _message);
}



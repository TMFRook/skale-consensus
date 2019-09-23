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

    @file Schain.cpp
    @author Stan Kladko
    @date 2018
*/

#include "../Log.h"
#include "../SkaleCommon.h"
#include "../exceptions/FatalError.h"
#include "../exceptions/InvalidArgumentException.h"

#include "../thirdparty/json.hpp"

#include "../abstracttcpserver/ConnectionStatus.h"
#include "../node/ConsensusEngine.h"

#include <unordered_set>

#include "leveldb/db.h"

#include "../node/ConsensusEngine.h"
#include "../node/Node.h"

#include "../blockproposal/pusher/BlockProposalClientAgent.h"
#include "../headers/BlockProposalHeader.h"
#include "../pendingqueue/PendingTransactionsAgent.h"

#include "../blockfinalize/client/BlockFinalizeClientAgent.h"
#include "../blockproposal/server/BlockProposalServerAgent.h"
#include "../catchup/client/CatchupClientAgent.h"
#include "../catchup/server/CatchupServerAgent.h"
#include "../crypto/ConsensusBLSSigShare.h"
#include "../exceptions/EngineInitException.h"
#include "../exceptions/ParsingException.h"
#include "../messages/InternalMessageEnvelope.h"
#include "../messages/Message.h"
#include "../messages/MessageEnvelope.h"
#include "../messages/NetworkMessageEnvelope.h"
#include "../node/NodeInfo.h"


#include "../blockfinalize/received/ReceivedBlockSigSharesDatabase.h"
#include "../blockproposal/received/ReceivedBlockProposalsDatabase.h"
#include "../network/Sockets.h"
#include "../protocols/ProtocolInstance.h"
#include "../protocols/blockconsensus/BlockConsensusAgent.h"

#include "../network/ClientSocket.h"
#include "../network/IO.h"
#include "../network/ZMQServerSocket.h"
#include "SchainMessageThreadPool.h"

#include "../crypto/SHAHash.h"
#include "../datastructures/BlockProposal.h"
#include "../datastructures/BlockProposalSet.h"
#include "../datastructures/CommittedBlock.h"
#include "../datastructures/CommittedBlockList.h"
#include "../datastructures/MyBlockProposal.h"
#include "../datastructures/ReceivedBlockProposal.h"
#include "../datastructures/Transaction.h"
#include "../datastructures/TransactionList.h"
#include "../exceptions/ExitRequestedException.h"
#include "../messages/ConsensusProposalMessage.h"

#include "../exceptions/FatalError.h"


#include "../pricing/PricingAgent.h"


#include "../crypto/bls_include.h"
#include "../db/BlockDB.h"
#include "../db/LevelDB.h"
#include "../pendingqueue/TestMessageGeneratorAgent.h"
#include "SchainTest.h"


#include "../libBLS/bls/BLSPrivateKeyShare.h"
#include "Schain.h"


void Schain::postMessage( ptr< MessageEnvelope > m ) {

    checkForExit();

    lock_guard< mutex > lock( messageMutex );

    ASSERT( m );
    ASSERT( ( uint64_t ) m->getMessage()->getBlockId() != 0 );


    messageQueue.push( m );
    messageCond.notify_all();
}


void Schain::messageThreadProcessingLoop( Schain* s ) {
    ASSERT( s );

    setThreadName( __CLASS_NAME__ );
    s->waitOnGlobalStartBarrier();


    using namespace std::chrono;

    try {
        s->startTime = getCurrentTimeMilllis();


        logThreadLocal_ = s->getNode()->getLog();

        queue< ptr< MessageEnvelope > > newQueue;

        while ( !s->getNode()->isExitRequested() ) {
            {
                unique_lock< mutex > mlock( s->messageMutex );
                while ( s->messageQueue.empty() ) {
                    s->messageCond.wait( mlock );
                    if ( s->getNode()->isExitRequested() ) {
                        s->getNode()->getSockets()->consensusZMQSocket->closeSend();
                        return;
                    }
                }

                newQueue = s->messageQueue;


                while ( !s->messageQueue.empty() ) {
                    s->messageQueue.pop();
                }
            }


            while ( !newQueue.empty() ) {
                ptr< MessageEnvelope > m = newQueue.front();


                ASSERT( ( uint64_t ) m->getMessage()->getBlockId() != 0 );

                try {
                    s->getBlockConsensusInstance()->routeAndProcessMessage( m );
                } catch ( Exception& e ) {
                    if ( s->getNode()->isExitRequested() ) {
                        s->getNode()->getSockets()->consensusZMQSocket->closeSend();
                        return;
                    }
                    Exception::logNested( e );
                }

                newQueue.pop();
            }
        }


        s->getNode()->getSockets()->consensusZMQSocket->closeSend();
    } catch ( FatalError* e ) {
        s->getNode()->exitOnFatalError( e->getMessage() );
    }
}


uint64_t Schain::getHighResolutionTime() {
    auto now = std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast< std::chrono::nanoseconds >( now.time_since_epoch() ).count();
}


chrono::milliseconds Schain::getCurrentTimeMilllis() {
    return chrono::duration_cast< chrono::milliseconds >(
        chrono::system_clock::now().time_since_epoch() );
}


uint64_t Schain::getCurrentTimeSec() {
    uint64_t result =
        chrono::duration_cast< chrono::seconds >( chrono::system_clock::now().time_since_epoch() )
            .count();

    ASSERT( result < ( uint64_t ) MODERN_TIME + 1000000000 );

    return result;
}


uint64_t Schain::getCurrentTimeMs() {
    uint64_t result = chrono::duration_cast< chrono::milliseconds >(
        chrono::system_clock::now().time_since_epoch() )
                          .count();

    return result;
}

void Schain::startThreads() {
    this->consensusMessageThreadPool->startService();
}


Schain::Schain(
    Node* _node, schain_index _schainIndex, const schain_id& _schainID, ConsensusExtFace* _extFace )
    : Agent( *this, true, true ),
      totalTransactions( 0 ),
      extFace( _extFace ),
      schainID( _schainID ),
      consensusMessageThreadPool( new SchainMessageThreadPool( this ) ),
      node( _node ),
      schainIndex( _schainIndex ) {
    ASSERT( schainIndex > 0 );

    try {
        lastCommittedBlockID.store( 0 );
        bootstrapBlockID.store( 0 );
        committedBlockTimeStamp.store( 0 );


        this->io = make_shared< IO >( this );


        ASSERT( getNode()->getNodeInfosByIndex()->size() > 0 );

        for ( auto const& iterator : *getNode()->getNodeInfosByIndex() ) {
            if ( iterator.second->getNodeID() == getNode()->getNodeID() ) {
                ASSERT( thisNodeInfo == nullptr && iterator.second != nullptr );
                thisNodeInfo = iterator.second;
            }
        }

        if ( thisNodeInfo == nullptr ) {
            BOOST_THROW_EXCEPTION(EngineInitException( "Schain: " + to_string( ( uint64_t ) getSchainID() ) +
                                           " does not include current node with IP " +
                                           *getNode()->getBindIP() + "and node id " +
                                           to_string( getNode()->getNodeID() ),
                __CLASS_NAME__ ));
        }

        ASSERT( getNodeCount() > 0 );

        constructChildAgents();

        string x = SchainTest::NONE;

        blockProposerTest = make_shared< string >( x );


        getNode()->registerAgent( this );

    } catch ( ... ) {
        throw_with_nested( FatalError( __FUNCTION__, __CLASS_NAME__ ) );
    }
}

const ptr< IO > Schain::getIo() const {
    return io;
}


void Schain::constructChildAgents() {
    try {
        std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );
        pendingTransactionsAgent = make_shared< PendingTransactionsAgent >( *this );
        blockProposalClient = make_shared< BlockProposalClientAgent >( *this );
        catchupClientAgent = make_shared< CatchupClientAgent >( *this );
        blockConsensusInstance = make_shared< BlockConsensusAgent >( *this );
        blockProposalsDatabase = make_shared< ReceivedBlockProposalsDatabase >( *this );
        blockSigSharesDatabase = make_shared< ReceivedBlockSigSharesDatabase >( *this );
        testMessageGeneratorAgent = make_shared< TestMessageGeneratorAgent >( *this );
        pricingAgent = make_shared< PricingAgent >( *this );

    } catch ( ... ) {
        throw_with_nested( FatalError( __FUNCTION__, __CLASS_NAME__ ) );
    }
}


void Schain::blockCommitsArrivedThroughCatchup( ptr< CommittedBlockList > _blocks ) {
    ASSERT( _blocks );

    auto b = _blocks->getBlocks();

    ASSERT( b );

    if ( b->size() == 0 ) {
        return;
    }


    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );


    atomic< uint64_t > committedIDOld( lastCommittedBlockID.load() );

    uint64_t previosBlockTimeStamp = 0;
    uint64_t previosBlockTimeStampMs = 0;


    ASSERT( b->at( 0 )->getBlockID() <= ( uint64_t ) lastCommittedBlockID + 1 );

    for ( size_t i = 0; i < b->size(); i++ ) {
        auto t = b->at( i );

        if ( t->getBlockID() > lastCommittedBlockID.load() ) {
            lastCommittedBlockID++;
            processCommittedBlock( t );
            previosBlockTimeStamp = t->getTimeStamp();
            previosBlockTimeStampMs = t->getTimeStampMs();
        }
    }

    if ( committedIDOld < lastCommittedBlockID ) {
        LOG( info,
            "BLOCK_CATCHUP: " + to_string( lastCommittedBlockID - committedIDOld ) + " BLOCKS" );
        proposeNextBlock( previosBlockTimeStamp, previosBlockTimeStampMs );
    }
}


void Schain::blockCommitArrived( bool bootstrap, block_id _committedBlockID,
    schain_index _proposerIndex, uint64_t _committedTimeStamp ) {

    checkForExit();

    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

    ASSERT( _committedTimeStamp < ( uint64_t ) 2 * MODERN_TIME );

    if ( _committedBlockID <= lastCommittedBlockID && !bootstrap )
        return;


    ASSERT( _committedBlockID == ( lastCommittedBlockID + 1 ) || lastCommittedBlockID == 0 );


    lastCommittedBlockID.store( ( uint64_t ) _committedBlockID );
    committedBlockTimeStamp = _committedTimeStamp;


    uint64_t previousBlockTimeStamp = 0;
    uint64_t previousBlockTimeStampMs = 0;

    ptr< BlockProposal > committedProposal = nullptr;


    if ( !bootstrap ) {
        committedProposal =
            blockProposalsDatabase->getBlockProposal( _committedBlockID, _proposerIndex );

        ASSERT( committedProposal );

        auto newCommittedBlock = make_shared< CommittedBlock >(committedProposal );

        processCommittedBlock( newCommittedBlock );

        previousBlockTimeStamp = newCommittedBlock->getTimeStamp();
        previousBlockTimeStampMs = newCommittedBlock->getTimeStampMs();


    } else {
        LOG( info, "Jump starting the system with block" + to_string( _committedBlockID ) );
        if(_committedBlockID == 0)
            this->pricingAgent->calculatePrice(
                ConsensusExtFace::transactions_vector(), 0, 0, 0 );
    }


    proposeNextBlock( previousBlockTimeStamp, previousBlockTimeStampMs );
}


void Schain::checkForExit() {
    if (getNode()->isExitRequested()) {
        BOOST_THROW_EXCEPTION(ExitRequestedException(__CLASS_NAME__));
    }
}

void Schain::proposeNextBlock(
    uint64_t _previousBlockTimeStamp, uint32_t _previousBlockTimeStampMs ) {

    checkForExit();

    block_id _proposedBlockID( ( uint64_t ) lastCommittedBlockID + 1 );

    ASSERT( pushedBlockProposals.count( _proposedBlockID ) == 0 );

    auto myProposal = pendingTransactionsAgent->buildBlockProposal(
        _proposedBlockID, _previousBlockTimeStamp, _previousBlockTimeStampMs );

    ASSERT( myProposal->getProposerIndex() == getSchainIndex() );

    if ( blockProposalsDatabase->addBlockProposal( myProposal ) ) {
        startConsensus( _proposedBlockID );
    }

    LOG( debug, "PROPOSING BLOCK NUMBER:" + to_string( _proposedBlockID ) );

    blockProposalClient->enqueueItem( myProposal );


    pushedBlockProposals.insert( _proposedBlockID );
}

void Schain::processCommittedBlock( ptr< CommittedBlock > _block ) {

    checkForExit();

    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );


    ASSERT( lastCommittedBlockID == _block->getBlockID() );

    totalTransactions += _block->getTransactionList()->size();

    auto h = _block->getHash()->toHex()->substr( 0, 8 );
    LOG( info, "BLOCK_COMMIT: PRPSR:" + to_string( _block->getProposerIndex() ) +
                   ":BID: " + to_string( _block->getBlockID() ) + ":HASH:" + h +
                   ":BLOCK_TXS:" + to_string( _block->getTransactionCount() ) +
                   ":DMSG:" + to_string( getMessagesCount() ) +
                   ":MPRPS:" + to_string( MyBlockProposal::getTotalObjects() ) +
                   ":RPRPS:" + to_string( ReceivedBlockProposal::getTotalObjects() ) +
                   ":TXNS:" + to_string( Transaction::getTotalObjects() ) +
                   //              ":PNDG:" +
                   //              to_string(pendingTransactionsAgent->getPendingTransactionsSize())
                   //              +
                   ":KNWN:" + to_string( pendingTransactionsAgent->getKnownTransactionsSize() ) +
                   ":CMT:" + to_string( pendingTransactionsAgent->getCommittedTransactionsSize() ) +
                   ":MGS:" + to_string( Message::getTotalObjects() ) +
                   ":INSTS:" + to_string( ProtocolInstance::getTotalObjects() ) +
                   ":BPS:" + to_string( BlockProposalSet::getTotalObjects() ) +
                   ":TLS:" + to_string( TransactionList::getTotalObjects() ) +
                   ":HDRS:" + to_string( Header::getTotalObjects() ) );


    saveBlock( _block );

    blockProposalsDatabase->cleanOldBlockProposals( _block->getBlockID() );

    pushBlockToExtFace( _block );
}

void Schain::saveBlock( ptr< CommittedBlock >& _block ) {
    checkForExit();

    saveBlockToBlockCache( _block );
    getNode()->getBlockDB()->saveBlock( _block );
}

void Schain::saveBlockToBlockCache( ptr< CommittedBlock >& _block ) {
    CHECK_ARGUMENT( _block != nullptr );

    auto blockID = _block->getBlockID();

    ASSERT( blocks.count( blockID ) == 0 );

    blocks[blockID] = _block;

    auto storageSize = getNode()->getCommittedBlockStorageSize();

    if ( blockID > storageSize && blocks.count( blockID - storageSize ) > 0 ) {
        blocks.erase( lastCommittedBlockID - storageSize );
    };


    ASSERT( blocks.size() <= storageSize );
}


void Schain::pushBlockToExtFace( ptr< CommittedBlock >& _block ) {

    checkForExit();

    auto blockID = _block->getBlockID();


    ASSERT( ( returnedBlock + 1 == blockID ) || returnedBlock == 0 );

    returnedBlock = ( uint64_t ) blockID;

    auto tv = _block->getTransactionList()->createTransactionVector();

    auto next_price = this->pricingAgent->calculatePrice(
        *tv, _block->getTimeStamp(), _block->getTimeStampMs(), _block->getBlockID() );
    auto cur_price = this->pricingAgent->readPrice(_block->getBlockID() - 1);


    if ( extFace ) {
        extFace->createBlock( *tv, _block->getTimeStamp(), _block->getTimeStampMs(),
            ( __uint64_t ) _block->getBlockID(), cur_price );
    }
}


void Schain::startConsensus( const block_id _blockID ) {
    {
        checkForExit();

        std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

        LOG( debug, "Got proposed block set for block:" + to_string( _blockID ) );

        ASSERT( blockProposalsDatabase->isTwoThird( _blockID ) );

        LOG( debug, "StartConsensusIfNeeded BLOCK NUMBER:" + to_string( ( _blockID ) ) );

        if ( _blockID <= lastCommittedBlockID ) {
            LOG( debug, "Too late to start consensus: already committed " +
                            to_string( lastCommittedBlockID ) );
            return;
        }

        if ( _blockID > lastCommittedBlockID + 1 ) {
            LOG( debug, "Consensus is in the future" + to_string( lastCommittedBlockID ) );
            return;
        }

        if ( startedConsensuses.count( _blockID ) > 0 ) {
            LOG( debug, "already started consensus for this block id" );
            return;
        }

        startedConsensuses.insert( _blockID );
    }


    auto proposalVector = blockProposalsDatabase->getBooleanProposalsVector( _blockID );

    ASSERT( blockConsensusInstance != nullptr && proposalVector != nullptr );


    auto message = make_shared< ConsensusProposalMessage >( *this, _blockID, proposalVector );

    auto envelope = make_shared< InternalMessageEnvelope >( ORIGIN_EXTERNAL, message, *this );


    LOG( debug, "Starting consensus for block id:" + to_string( _blockID ) );

    postMessage( envelope );
}


void Schain::proposedBlockArrived( ptr< BlockProposal > pbm ) {
    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

    if ( blockProposalsDatabase->addBlockProposal( pbm ) ) {
        startConsensus( pbm->getBlockID() );
    }
}


ptr< PendingTransactionsAgent > Schain::getPendingTransactionsAgent() const {
    return pendingTransactionsAgent;
}

chrono::milliseconds Schain::getStartTime() const {
    return startTime;
}

const block_id Schain::getLastCommittedBlockID() const {
    return block_id( lastCommittedBlockID.load() );
}

ptr< BlockProposal > Schain::getBlockProposal( block_id _blockID, schain_index _schainIndex) {

    return blockProposalsDatabase->getBlockProposal(_blockID, _schainIndex);

}

ptr< CommittedBlock > Schain::getCachedBlock( block_id _blockID ) {
    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

    if ( blocks.count( _blockID > 0 ) ) {
        return blocks.at( _blockID );
    } else {
        return nullptr;
    }
}

ptr< CommittedBlock > Schain::getBlock( block_id _blockID ) {
    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

    auto block = getCachedBlock( _blockID );

    if ( block )
        return block;


    auto serializedBlock = getNode()->getBlockDB()->getSerializedBlock( _blockID );

    if ( serializedBlock == nullptr ) {
        return nullptr;
    }

    return CommittedBlock::deserialize( serializedBlock );
}


ptr< vector< uint8_t > > Schain::getSerializedBlock( uint64_t i ) const {
    auto block = sChain->getCachedBlock( i );


    if ( block ) {
        return block->getSerialized();
    } else {
        return getNode()->getBlockDB()->getSerializedBlock( i );
    }
}


schain_index Schain::getSchainIndex() const {
    return schainIndex;
}


Node* Schain::getNode() const {
    return node;
}


node_count Schain::getNodeCount() {
    auto count = node_count( getNode()->getNodeInfosByIndex()->size() );
    ASSERT( count > 0 );
    return count;
}


transaction_count Schain::getMessagesCount() {
    std::lock_guard< std::recursive_mutex > aLock( getMainMutex() );

    return transaction_count( messageQueue.size() );
}


schain_id Schain::getSchainID() {
    return schainID;
}

node_id Schain::getNodeIDByIndex( schain_index _index ) {
    if ( ( ( uint64_t ) _index ) > ( uint64_t ) this->getNodeCount() ) {
        BOOST_THROW_EXCEPTION(
            InvalidArgumentException( "Index exceeds node count", __CLASS_NAME__ ) );
    }

    auto nodeInfo = this->getNode()->getNodeInfoByIndex( _index );

    return nodeInfo->getNodeID();
}


ptr< BlockConsensusAgent > Schain::getBlockConsensusInstance() {
    return blockConsensusInstance;
}


ptr< NodeInfo > Schain::getThisNodeInfo() const {
    return thisNodeInfo;
}


ptr< TestMessageGeneratorAgent > Schain::getTestMessageGeneratorAgent() const {
    return testMessageGeneratorAgent;
}

void Schain::setBlockProposerTest( const string& blockProposerTest ) {
    Schain::blockProposerTest = make_shared< string >( blockProposerTest );
}

void Schain::bootstrap( block_id _lastCommittedBlockID, uint64_t _lastCommittedBlockTimeStamp ) {
    try {
        ASSERT( bootStrapped == false );
        bootStrapped = true;
        bootstrapBlockID.store( ( uint64_t ) _lastCommittedBlockID );
        blockCommitArrived(
            true, _lastCommittedBlockID, schain_index( 0 ), _lastCommittedBlockTimeStamp );
    } catch ( Exception& e ) {
        Exception::logNested( e );
        return;
    }
}


schain_id Schain::getSchainID() const {
    return schainID;
}


uint64_t Schain::getTotalTransactions() const {
    return totalTransactions;
}

uint64_t Schain::getLastCommittedBlockTimeStamp() {
    return committedBlockTimeStamp;
}


block_id Schain::getBootstrapBlockID() const {
    return bootstrapBlockID.load();
}


void Schain::setHealthCheckFile( uint64_t status ) {
    string fileName = Log::getDataDir()->append( "/HEALTH_CHECK" );


    ofstream f;
    f.open( fileName, ios::trunc );
    f << status;
    f.close();
}


void Schain::healthCheck() {
    std::unordered_set< uint64_t > connections;

    setHealthCheckFile( 1 );

    auto beginTime = getCurrentTimeSec();

    LOG( info, "Waiting to connect to peers" );

    while ( 3 * ( connections.size() + 1 ) < 2 * getNodeCount() ) {
        if ( getCurrentTimeSec() - beginTime > 6000 ) {
            setHealthCheckFile( 0 );
            LOG( err, "Coult not connect to 2/3 of peers" );
            exit( 110 );
        }

        for ( int i = 1; i <= getNodeCount(); i++ ) {
            if ( i != ( getSchainIndex() ) && !connections.count( i ) ) {
                try {
                    if (getNode()->isExitRequested()) {
                        BOOST_THROW_EXCEPTION(ExitRequestedException(__CLASS_NAME__));
                    }
                    auto x = make_shared< ClientSocket >(
                        *this, schain_index( i ), port_type::PROPOSAL );
                    LOG( debug, "Health check: connected to peer" );
                    getIo()->writeMagic( x, true );
                    x->closeSocket();
                    connections.insert( i );



                } catch ( ExitRequestedException& ) {
                    throw;
                } catch ( std::exception& e ) {
                    usleep( 1000000 );
                }
            }
        }
    }

    setHealthCheckFile( 2 );
}

void Schain::sigShareArrived( ptr< ConsensusBLSSigShare > _sigShare ) {

    checkForExit();

    if ( blockSigSharesDatabase->addSigShare( _sigShare ) ) {
        auto blockId = _sigShare->getBlockId();
        auto mySig = sign( getBlock( blockId )->getHash(), blockId );
        blockSigSharesDatabase->addSigShare( mySig );
        ASSERT( blockSigSharesDatabase->isTwoThird( blockId ) );
        blockSigSharesDatabase->mergeAndSaveBLSSignature( blockId );
    };
}


ptr< ConsensusBLSSigShare > Schain::sign( ptr< SHAHash > _hash, block_id _blockId ) {
    auto blsShare =
        getNode()->getBlsPrivateKey()->sign( _hash->toHex(), ( uint64_t ) getSchainIndex() );

    return make_shared< ConsensusBLSSigShare >(
        blsShare, getSchainID(), _blockId, getNode()->getNodeID() );
}

void Schain::constructServers( ptr< Sockets > _sockets ) {
    blockProposalServerAgent =
        make_shared< BlockProposalServerAgent >( *this, _sockets->blockProposalSocket );

    catchupServerAgent = make_shared< CatchupServerAgent >( *this, _sockets->catchupSocket );
}


size_t Schain::getTotalSignersCount() {
    return ( size_t )( uint64_t ) getNodeCount();
}
size_t Schain::getRequiredSignersCount() {
    auto count = getNodeCount();

    if ( count <= 2 ) {
        return ( uint64_t ) count;
    }

    else {
        return 2 * ( uint64_t ) count / 3 + 1;
    }
}

u256 Schain::getPriceForBlockId(uint64_t _blockId){
    ASSERT(pricingAgent != nullptr);
    return pricingAgent->readPrice(_blockId);
}

ptr< Transaction > Transaction::createRandomSample( uint64_t _size, boost::random::mt19937& _gen,
    boost::random::uniform_int_distribution<>& _ubyte ) {
    auto sample = make_shared< vector< uint8_t > >( _size, 0 );


    for ( uint32_t j = 0; j < sample->size(); j++ ) {
        sample->at( j ) = _ubyte( _gen );
    }


    return Transaction::deserialize( sample, 0, sample->size(), false );
};

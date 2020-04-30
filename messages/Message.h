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

    @file Message.h
    @author Stan Kladko
    @date 2018
*/

#pragma once


enum MsgType {CHILD_COMPLETED, PARENT_COMPLETED,

    MSG_BVB_BROADCAST, MSG_AUX_BROADCAST, BIN_CONSENSUS_COMMIT, BIN_CONSENSUS_HISTORY_DECIDE,
    BIN_CONSENSUS_HISTORY_CC, BIN_CONSENSUS_HISTORY_BVSELF, BIN_CONSENSUS_HISTORY_AUXSELF, BIN_CONSENSUS_HISTORY_NEW_ROUND,
    MSG_BLOCK_CONSENSUS_INIT, MSG_CONSENSUS_PROPOSAL, MSG_BLOCK_SIGN_BROADCAST };

class ProtocolInstance;
class ProtocolKey;

class Message {

private:

    static atomic<int64_t>  totalObjects;

protected:

    schain_id schainID;
    block_id  blockID;
    schain_index blockProposerIndex;
    MsgType msgType;
    msg_id msgID;
    node_id srcNodeID;

    ptr<ProtocolKey> protocolKey;

public:
    Message(const schain_id &schainID, MsgType msgType, const msg_id &msgID, const node_id &srcNodeID,
            const block_id &blockID, const schain_index &blockProposerIndex);

    node_id getSrcNodeID() const;


    MsgType getMessageType() const;

    const block_id getBlockId() const;

    schain_index getBlockProposerIndex() const ;

    schain_id getSchainID() const;

    ptr<ProtocolKey> createDestinationProtocolKey();

    const block_id &getBlockID() const;

    MsgType getMsgType() const;

    const msg_id &getMsgID() const;

    virtual ~Message();

    static int64_t getTotalObjects() {
        return totalObjects;
    }

};

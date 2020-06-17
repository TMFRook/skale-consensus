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

    @file InternalMessage.cpp
    @author Stan Kladko
    @date 2018
*/

#include "SkaleCommon.h"
#include "Log.h"
#include "exceptions/FatalError.h"
#include "thirdparty/json.hpp"

#include "chains/Schain.h"
#include "protocols/ProtocolInstance.h"
#include "protocols/ProtocolKey.h"
#include "node/Node.h"
#include "InternalMessage.h"

using namespace std;


InternalMessage::InternalMessage(MsgType msgType,
                                 ProtocolInstance &srcProtocolInstance,
                                 const ptr<ProtocolKey> &protocolKey) :

        Message(srcProtocolInstance.getSchain()->getSchainID(),
                msgType, srcProtocolInstance.createNetworkMessageID(),
                srcProtocolInstance.getSchain()->getNode()->getNodeID(), protocolKey->getBlockID(),
                protocolKey->getBlockProposerIndex()) {
    ASSERT(protocolKey != nullptr);
    ASSERT((uint64_t)protocolKey->getBlockID() != 0);

}

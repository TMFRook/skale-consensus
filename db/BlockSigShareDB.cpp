/*
    Copyright (C) 2019 SKALE Labs

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

    @file BlockSigShareDB.cpp
    @author Stan Kladko
    @date 2019
*/


#include "SkaleCommon.h"
#include "Log.h"
#include "chains/Schain.h"
#include "crypto/CryptoManager.h"
#include "crypto/BLAKE3Hash.h"
#include "crypto/ThresholdSigShare.h"
#include "crypto/ThresholdSigShareSet.h"
#include "crypto/ThresholdSignature.h"
#include "datastructures/CommittedBlock.h"
#include "exceptions/ExitRequestedException.h"
#include "exceptions/InvalidStateException.h"
#include "node/Node.h"
#include "node/NodeInfo.h"
#include "thirdparty/json.hpp"

#include "BlockSigShareDB.h"


BlockSigShareDB::BlockSigShareDB(Schain *_sChain, string &_dirName, string &_prefix, node_id _nodeId,
                                 uint64_t _maxDBSize)
        : CacheLevelDB(_sChain, _dirName, _prefix, _nodeId, _maxDBSize, false) {
}


ptr<ThresholdSignature>
BlockSigShareDB::checkAndSaveShare(const ptr<ThresholdSigShare>& _sigShare, const ptr<CryptoManager>& _cryptoManager) {
    try {
        CHECK_ARGUMENT(_sigShare)
        CHECK_ARGUMENT(_cryptoManager)

        auto sigShareString = _sigShare->toString();
        CHECK_STATE(!sigShareString.empty())

        LOCK(sigShareMutex)

        auto enoughSet = writeStringToSet(sigShareString, _sigShare->getBlockId(),
                                          _sigShare->getSignerIndex());
        if (enoughSet == nullptr)
            return nullptr;

        auto _sigShareSet = _cryptoManager->createSigShareSet(_sigShare->getBlockId());
        CHECK_STATE(_sigShareSet)

        for (auto &&item : *enoughSet) {

            auto nodeInfo = sChain->getNode()->getNodeInfoByIndex(item.first);
            CHECK_STATE(nodeInfo)
            CHECK_STATE(!item.second.empty())
            auto sigShare = _cryptoManager->createSigShare(item.second, sChain->getSchainID(),
                                                           _sigShare->getBlockId(), item.first, false);
            CHECK_STATE(sigShare)
            _sigShareSet->addSigShare(sigShare);
        }

        CHECK_STATE(_sigShareSet->isEnough())
        auto signature = _sigShareSet->mergeSignature();
        CHECK_STATE(signature)
        return signature;
    } catch (ExitRequestedException &) { throw; } catch (...) {
        throw_with_nested(InvalidStateException(__FUNCTION__, __CLASS_NAME__));
    }
}


const string& BlockSigShareDB::getFormatVersion() {
    static const string version = "1.0";
    return version;
}







/*
  Copyright (C) 2018- SKALE Labs

  This file is part of libBLS.

  libBLS is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  libBLS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with libBLS.  If not, see <https://www.gnu.org/licenses/>.

  @file ECDSAReqSignMessage.h
  @author Stan Kladko
  @date 2020
*/

#ifndef SGXWALLET_ECDSASIGNREQMESSAGE_H
#define SGXWALLET_ECDSASIGNREQMESSAGE_H

#include "ZMQMessage.h"

class ECDSASignReqMessage : public ZMQMessage {
public:

    ECDSASignReqMessage(shared_ptr <rapidjson::Document> &_d) : ZMQMessage(_d) {};

    virtual Json::Value process();


};


#endif //SGXWALLET_ECDSASIGNREQMESSAGE_H

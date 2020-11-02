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

    @file Header.h
    @author Stan Kladko
    @date 2018
*/

#pragma once






class Buffer;
class ServerConnection;
class SHAHash;

#include "abstracttcpserver/ConnectionStatus.h"
#include "BasicHeaderJson.h"



class HeaderJson : public BasicHeaderJson {

private:

    ConnectionStatus status = CONNECTION_ERROR;
    ConnectionSubStatus substatus = CONNECTION_ERROR_UNKNOWN_SERVER_ERROR;

public:

    HeaderJson(const char *_type);

    virtual ~HeaderJson();

    virtual void addFields(nlohmann::json & j);


    pair<ConnectionStatus, ConnectionSubStatus> getStatusSubStatus() { return {status,
                                                                          substatus};};


    void setStatusSubStatus( ConnectionStatus _status, ConnectionSubStatus _substatus ) {
        this->status = _status;
        this->substatus = _substatus;
    }

};




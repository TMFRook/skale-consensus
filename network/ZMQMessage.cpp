/*
    Copyright (C) 2020 SKALE Labs

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

    @file ZMQMessage.cpp
    @author Stan Kladko
    @date 2020
*/

#include <third_party/cryptlite/sha256.h>
#include <iostream>

#include "BLSSignRspMessage.h"
#include "ECDSASignRspMessage.h"
#include "ZMQMessage.h"


uint64_t ZMQMessage::getUint64Rapid( const char* _name ) {
    CHECK_STATE( _name );
    CHECK_STATE( d->HasMember( _name ) );
    const rapidjson::Value& a = ( *d )[_name];
    CHECK_STATE( a.IsUint64() );
    return a.GetUint64();
};

string ZMQMessage::getStringRapid( const char* _name ) {
    CHECK_STATE( _name );
    CHECK_STATE( d->HasMember( _name ) );
    CHECK_STATE( ( *d )[_name].IsString() );
    return ( *d )[_name].GetString();
};


shared_ptr< ZMQMessage > ZMQMessage::parse( const char* _msg, size_t _size) {
    CHECK_STATE( _msg );
    CHECK_STATE( _size > 5 );
    // CHECK NULL TERMINATED
    CHECK_STATE( _msg[_size] == 0 );
    CHECK_STATE( _msg[_size - 1] == '}' );
    CHECK_STATE( _msg[0] == '{' );

    auto d = make_shared< rapidjson::Document >();

    d->Parse( _msg );

    CHECK_STATE( !d->HasParseError() );
    CHECK_STATE( d->IsObject() );

    CHECK_STATE( d->HasMember( "type" ) );
    CHECK_STATE( ( *d )["type"].IsString() );
    string type = ( *d )["type"].GetString();

    shared_ptr< ZMQMessage > result;

    return buildResponse( type, d );
}



shared_ptr< ZMQMessage > ZMQMessage::buildResponse(
    string& _type, shared_ptr< rapidjson::Document > _d ) {
    if ( _type == ZMQMessage::BLS_SIGN_RSP ) {
        return make_shared< BLSSignRspMessage >( _d );
    } else if ( _type == ZMQMessage::ECDSA_SIGN_RSP ) {
        return make_shared< ECDSASignRspMessage >( _d );
    } else {
        BOOST_THROW_EXCEPTION( InvalidStateException(
            "Incorrect zmq message request type: " + string( _type ), __CLASS_NAME__ ) );
    }
}
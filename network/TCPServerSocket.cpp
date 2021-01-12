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

    @file TCPServerSocket.cpp
    @author Stan Kladko
    @date 2018
*/

#include <boost/asio.hpp>

#include "Log.h"
#include "SkaleCommon.h"


#include "exceptions/FatalError.h"

#include "Network.h"
#include "Sockets.h"
#include "TCPServerSocket.h"

int TCPServerSocket::createAndBindTCPSocket() {
    LOG( debug, "Creating TCP listen socket" );
    int s;

    if ( ( s = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 ) {
        BOOST_THROW_EXCEPTION( FatalError( "Could not create read socket" ) );
    }

    int iSetOption = 1;

    setsockopt( s, SOL_SOCKET, SO_REUSEADDR, ( char* ) &iSetOption, sizeof( iSetOption ) );

    if ( ::bind( s, ( struct sockaddr* ) socketaddr.get(), sizeof( sockaddr_in ) ) < 0 ) {
        BOOST_THROW_EXCEPTION(
            FatalError( "Could not bind the TCP socket: error " + to_string( errno ) ) );
    }

    // Init the connection
    listen( s, SOCKET_BACKLOG );

    LOG( debug, "Successfully created TCP listen socket" );

    return s;
}


TCPServerSocket::TCPServerSocket(const string& _bindIP, uint16_t _basePort, port_type _portType )
    : ServerSocket( _bindIP, _basePort, _portType ) {

    socketaddr = Sockets::createSocketAddress( bindIP, bindPort );
    CHECK_STATE(socketaddr);
    descriptor = createAndBindTCPSocket();
    CHECK_STATE( descriptor > 0 );
}


TCPServerSocket::~TCPServerSocket() {
    if ( descriptor != 0 )
        close( descriptor );
}

void TCPServerSocket::touch() {
    try {
        using namespace boost::asio;
        ip::tcp::endpoint ep( ip::address::from_string( "123.123.123.125" ), bindPort );
        io_context io;

        ip::tcp::socket sock( io );
        steady_timer timer(io, ::std::chrono::seconds(2));

        sock.async_connect( ep, [&](const boost::system::error_code& err ){
            std::cout << "timer cancel " << err << std::endl;
            timer.cancel();
        });

        timer.async_wait( [&](const boost::system::error_code& err ){
            std::cout << "sock close " << err << std::endl;
            sock.close();
        });

        std::cout << "before run" << std::endl;
        io.run();
        std::cout << "after run" << std::endl;
    } catch (...) {};    
}

int TCPServerSocket::getDescriptor() {
    return descriptor;
}


void TCPServerSocket::closeAndCleanupAll() {
    LOCK( m )
    if ( descriptor != 0 ) {
        close( descriptor );
        descriptor = 0;
    }
}

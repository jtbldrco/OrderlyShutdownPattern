/**************************************************************************
 * MIT License                                                            *
 * Copyright (c) 2018  iWay Technology LLC                                *
 *                                                                        *
 * Permission is hereby granted, free of charge, to any person obtaining  *
 * a copy of this software and associated documentation files (the        *
 * "Software"), to deal in the Software without restriction, including    *
 * without limitation the rights to use, copy, modify, merge, publish,    *
 * distribute, sublicense, and/or sell copies of the Software, and to     *
 * permit persons to whom the Software is furnished to do so, subject to  *
 * the following conditions:                                              *
 *                                                                        *
 * The above copyright notice and this permission notice shall be         *
 * included in all copies or substantial portions of the Software.        *
 *                                                                        *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        *
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     *
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. *
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   *
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   *
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      *
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 *
 **************************************************************************/

/*
 * MsgCommHdlrTestSender.cpp - utilizes, and demos use of,
 * class MsgCommHdlr.
 */

#include "MsgCommHdlr.h"

#include <iostream>
#include <stdio.h>
#include <time.h>

#define HOST "localhost"
#define PORT 16273

#define TEN_SECONDS 10

#define SEND_MSG_LEN 256

int main( int argc, char *argv[] ) {

#ifdef DEBUG_THREADEDWORKER
    std::cout << "\nFunction main(), main thread: " << MY_TID << std::endl;
#endif

    char send_msg[SEND_MSG_LEN] = { 0 };
    strcpy( send_msg, "This is the first string to be x-ferred!!! " );

    if( argc > 1 ) {
        memset( send_msg, 0, SEND_MSG_LEN );
        strcpy( send_msg, argv[1] );
    }

    time_t utctime;
    struct tm *timeinfo;
    time( &utctime );
    timeinfo = localtime( &utctime );
 
    printf( "asctime(timeinfo): %s\n", asctime(timeinfo) );
    strcat( send_msg, asctime(timeinfo) );

    int connectTimeout = 10;
    int clientTimeout = 10;

    // Object will do its work on a separate native thread -
    MsgCommHdlr msgCommHdlrSender( string( "msgCommHdlrSender" ), sender,
                                   HOST, PORT, connectTimeout, clientTimeout );

    // Internally, calls ThreadedWorker.startWorker();
    if( ! msgCommHdlrSender.go() ) {
        printf( "MsgCommHdlrTestSender failed to launch Msg Comm Hdlr.  Exiting.\n" );
        return 1;
    }

    std::string *pString1 = new std::string( send_msg );

    cout << "MsgCommHdlrTestSender ready to send message - enqueuing ... \n" << *pString1 << std::endl;

    msgCommHdlrSender.enqueueMessage( pString1 );    
    
//    std::cout << "\nSleeping main thread for 5 seconds.\n" << std::endl;
//    std::this_thread::sleep_for( std::chrono::milliseconds( 5000 ) );

    // (Yawn ...) now, direct one Worker to wrap it up
//    std::cout << "\nFunction main() shutting down MsgCommHdlr's.\n" << std::endl;
    msgCommHdlrSender.signalShutdown( true );
    msgCommHdlrSender.join();
//    msgCommHdlrReceiver.signalShutdown( true );
//    msgCommHdlrReceiver.join();

    // One more nap for good measure ...
    std::cout << "\nSleeping main thread for 1 more second.\n" << std::endl;
    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

    return 0;

} // End main(...)


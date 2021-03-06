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

/**************************************************************************
 * msg_sock_hdlr.c
 * 
 * PARTIALLY updated for timeout behavior on connections.
 */

#include "msg_sock_hdlr.h"

#include "iway_logger.h"

// Busier servers may require greater backlog size - this defines the
// number of queued client connect requests that the server can accommodate,
// not the total number of concurrent client connections.
#define BACKLOG 2

// These must appear WITH QUOTES
#define LISTENER_INTERFACES_IPV6 "::"
#define LISTENER_INTERFACES_IPV4 "0.0.0.0"

#define ACK_MSG_BUF_LEN 32
char full_log_msg[1024]; // Socket api-generated errors; generously sized!

static bool SIG_IGN_SET = false;

/**************************************************************************
 * Full-featured timeout receiver with designated shutdown flag checked
 * upon each timeout.  To return unconditionally after the first timeout,
 * just set shutdownFlag=1.
 *
 * The timeout feature actually does two things - first, the server
 * (listener) is capable of listening for a client connection with
 * periodic checks to see if there has been an externally set flag
 * indicating that this server 'listening' should be shut down (and
 * call return).  This periodic check will occur every socket listen
 * timeout seconds.  Second, once a client connects, if its message
 * send gets delayed, the timeout setting will end that read operation
 * and return with appropriate error code.  Each of these two timeout
 * durations can be individually set (in units of seconds). 
 *
 * Finally, if the client read times out, the state of the receive buffer
 * is undefined.  Check the function return value as defined above.
 *
 * Structure *sock_struct must be constructed valid prior to either
 * msg_sock_hdlr_open_for_xxx call.  See in msg_sock_hdlr.h these:
 * 
 *    sock_struct_init_send(...)
 *    sock_struct_init_recv(...)
 *
 * Note that each of the above returns an initialized structure. That
 * returned structure must be destroy at the end of its life (at the
 * point determined by caller's usage) using the following function:
 *
 *    sock_struct_destroy( sock_struct_t *s )
 *
 * See other utilities in msg_sock_hdlr.h.
 **************************************************************************/
sock_struct_t *msg_sock_hdlr_open_for_recv( sock_struct_t *sock_struct )
{
    // First, do some simple validation on the input structure - 
    sock_struct->valid = 1; // Innocent until proven otherwise 
    if( sock_struct->lsd != 0 ) sock_struct->valid = 0;
    if( sock_struct->csd != 0 ) sock_struct->valid = 0;
    if( sock_struct->lto < 0 ) sock_struct->valid = 0;
    if( sock_struct->cto < 0 ) sock_struct->valid = 0;

    // Judgment - is timeout 'too large' (more than one day)?
    // This will catch badly formed timeouts.
    if( sock_struct->lto > 86400 ) sock_struct->valid = 0;
    if( sock_struct->cto > 86400 ) sock_struct->valid = 0;
    
    if( ! sock_struct->valid ) {
        sprintf( full_log_msg,
                 "MSH Err %d; invalid sock_struct on listener acquire",
                 MSH_INVALID_SOCKSTRUCT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_INVALID_SOCKSTRUCT;
        return sock_struct;
    }
    
    if( ! SIG_IGN_SET ) {
        set_sigaction_ign_sigpipe();
        // Call once per process invocation
        SIG_IGN_SET = true;
    }

    int local_listener_sd; // local listening socket descriptor

    struct sockaddr_storage; // client's address information
    struct addrinfo *servinfo, *ptr_addrinfo;
    int return_val;
    int REUSE_ADDRS = 1; // => true

    struct timeval ltmout;

    bool set_listen_timeout = ( sock_struct->lto > 0 );

#ifdef DEBUG_MSH
    printf( "set_listen_timeout: %d\n", sock_struct->lto);
#endif

    ltmout.tv_sec = sock_struct->lto;
    ltmout.tv_usec=0;

    struct addrinfo hints;
    memset( &hints, 0, sizeof hints );

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET6;

    // const char listener_ip[] = LISTENER_INTERFACES_IPV4;
    const char listener_ip[] = LISTENER_INTERFACES_IPV6;

    // Port is supposed to be a 16-bit int; this is big enough for an input error
    char port[12] = { 0 };
    sprintf( port, "%d", sock_struct->port );

    if ( ( return_val = getaddrinfo( listener_ip, port, &hints, &servinfo ) ) != 0 ) {
        sprintf( full_log_msg, "MSH Err %d; getaddrinfo: %s", 
                 MSH_ERROR_GETADDRINFO, gai_strerror( return_val ) );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_ERROR_GETADDRINFO;
        return sock_struct;
    }

    // Loop, binding to first interface we're able to
    for( ptr_addrinfo = servinfo; ptr_addrinfo != NULL; ptr_addrinfo = ptr_addrinfo->ai_next ) {
        if( ( local_listener_sd =
              socket( ptr_addrinfo->ai_family, ptr_addrinfo->ai_socktype,
                      ptr_addrinfo->ai_protocol ) ) == -1 ) {

            sprintf( full_log_msg,
                     "MSH Info %d; unable to create listener sock desc for this addrinfo: %d",
                     MSH_ERROR_SETSOCKET, ptr_addrinfo->ai_family );
            IWAY_LOG( IWAY_LOG_INFO, full_log_msg );
            continue;
        }
        if ( setsockopt( local_listener_sd, SOL_SOCKET, SO_REUSEADDR, &REUSE_ADDRS,
                         sizeof( int ) ) == -1 ) {
            sprintf( full_log_msg, "MSH Err %d; setsockopt", MSH_ERROR_SETSOCKOPT );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
            sock_struct->result = MSH_ERROR_SETSOCKOPT;
            return sock_struct;
        }
        if ( bind( local_listener_sd, ptr_addrinfo->ai_addr, ptr_addrinfo->ai_addrlen ) == -1 ) {
            close( local_listener_sd );
            sprintf( full_log_msg, "MSH Info %d; unable to bind to this listener sock desc",
                     MSH_ERROR_SOCKBIND );
            IWAY_LOG( IWAY_LOG_INFO, full_log_msg );
            continue;
        }
        break;
    }

    freeaddrinfo( servinfo ); // clean-up

    // Make sure we DID bind to something above
    if( ptr_addrinfo == NULL )  {
        sprintf( full_log_msg, "MSH Err %d; unable to bind to any listener sock desc",
                 MSH_ERROR_SOCKBIND );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_ERROR_SOCKBIND;
        return sock_struct;
    }

    // Are we told to enable timeouts during the effort of
    // listenting for client connects?
    if( set_listen_timeout ) {

#ifdef DEBUG_MSH
            printf( "Setting Listen Timeouts\n" );
#endif

        if( setsockopt( local_listener_sd, SOL_SOCKET, SO_RCVTIMEO, &ltmout, sizeof ltmout ) < 0 ) {
            sprintf( full_log_msg,
                     "MSH Err %d; unable to set listener sock timeout",
                     MSH_ERROR_SETSOCKOPT );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
            sock_struct->result = MSH_ERROR_SETSOCKOPT;
            return sock_struct;
        }
        if( setsockopt( local_listener_sd, SOL_SOCKET, SO_SNDTIMEO, &ltmout, sizeof ltmout) < 0 ) {
            sprintf( full_log_msg,
                     "MSH Err %d; unable to set listener sock timeout",
                     MSH_ERROR_SETSOCKOPT );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
            sock_struct->result = MSH_ERROR_SETSOCKOPT;
            return sock_struct;
        }
    }
    // Write the newly acquired listener socket to the struct
    sock_struct->result = MSH_LISTENER_CREATED;
    sock_struct->lsd = local_listener_sd;
    return sock_struct;

} // End msg_sock_hdlr_open_for_recv(...)


/**************************************************************************/
sock_struct_t *msg_sock_hdlr_listen( sock_struct_t *sock_struct,
                                     int *shutdownFlag )
{
    // First, do some simple validation on the input structure -
    if( sock_struct->lsd == 0 ) sock_struct->valid = 0;
    if( sock_struct->lto < 0 ) sock_struct->valid = 0;
    if( sock_struct->cto < 0 ) sock_struct->valid = 0;

    if( ! sock_struct->valid ) {
        sprintf( full_log_msg,
                 "MSH Err %d; invalid sock_struct on listen attempt",
                 MSH_INVALID_SOCKSTRUCT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_INVALID_SOCKSTRUCT;

#ifdef DEBUG_MSH
        printf( "msg_sock_hdlr_listen() sock_struct invalid, returning " );
#endif

        return sock_struct;
    }

    bool set_listen_timeout = ( sock_struct->lto > 0 );
    bool set_cli_timeout = ( sock_struct->cto > 0 );

    struct timeval tmrecv, tmsend;
    tmrecv.tv_sec = sock_struct->cto;
    tmrecv.tv_usec=0;
    tmsend.tv_sec = sock_struct->cto;
    tmsend.tv_usec=0;

    int local_listener_sd = sock_struct->lsd;
    int local_client_sd, their_sockaddr;

    // In the normal case (regardless of timeout settings) this
    // will return 0 -

#ifdef DEBUG_MSH
        printf( "calling listen() in msg_sock_hdlr_listen() " );
#endif

    int listen_result = listen( local_listener_sd, BACKLOG );

    if( listen_result == -1 ) {
        sprintf( full_log_msg, "MSH Err %d; listen() failed on sock desc",
                 MSH_ERROR_SOCKLISTEN );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_ERROR_SOCKLISTEN;

#ifdef DEBUG_MSH
        printf( "msg_sock_hdlr_listen() listener setup failed, returning " );
#endif

        return sock_struct;
    }

    // This loop will accept one connection - this is not intended as a
    // multiple, concurrent access function - for that capability, call
    // this function multiple times (or, see related github.com project -
    // ../sample_socket_comm/).
    // This while loop continues, based up timeout settings,  while
    // there is no client connection AND the flag shutdownFlag is zero.
    socklen_t sin_size = sizeof their_sockaddr;
    while( true ) {
        local_client_sd = accept( local_listener_sd,
                                  (struct sockaddr *)&their_sockaddr, &sin_size );

        if( local_client_sd == -1 ) {
            if( errno == EWOULDBLOCK || errno == EAGAIN ) {

#ifdef DEBUG_MSH
                printf( "Fn msg_sock_hdlr_listen() timed out after %d sec wait for client "
                        "connection.\nFn msg_sock_hdlr_listen(), is shutdown signaled? %s\n",
                        sock_struct->lto, (*shutdownFlag ? "Yes" : "No") );
#endif

                // The client connect accept timed out, is a return/shutdown signaled?
                if( set_listen_timeout ) {
                    if( *shutdownFlag ) {
                        sprintf( full_log_msg,
                                 "MSH Info %d; accept() timed out and shutdown signaled",
                                 MSH_CONNECT_TIMEOUT );
                        IWAY_LOG( IWAY_LOG_INFO, full_log_msg );
                        sock_struct->result = MSH_CONNECT_TIMEOUT;

#ifdef DEBUG_MSH
                        printf( "msg_sock_hdlr_listen shudown signaled, returning " );
#endif

                        return sock_struct;
                    }

#ifdef DEBUG_MSH
                    printf( "msg_sock_hdlr_listen accept() timed out, try again.\n" );
#endif

                    continue;
                }
            }
            sprintf( full_log_msg, "MSH Info %d; accept() failed for this client, looping",
                     MSH_ERROR_SOCKACCEPT );
            IWAY_LOG( IWAY_LOG_INFO, full_log_msg );
            continue;
        }

#ifdef DEBUG_MSH
        printf( "Listener received a connection from client (%d) ...\n", local_client_sd );
#endif
 
        if( set_cli_timeout ) {

#ifdef DEBUG_MSH
            printf( "msg_sock_hdlr_listen setting client timeouts\n" );
#endif

            if( setsockopt( local_client_sd, SOL_SOCKET, SO_SNDTIMEO, &tmsend, sizeof tmsend) < 0 ) {
                sprintf( full_log_msg,
                         "MSH Err %d; unable to set client sock send timeout",
                         MSH_ERROR_SETSOCKOPT );
                IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
                sock_struct->result = MSH_ERROR_SETSOCKOPT;
                return sock_struct;
            }
            if( setsockopt( local_client_sd, SOL_SOCKET, SO_RCVTIMEO, &tmrecv, sizeof tmrecv ) < 0 ) {
                sprintf( full_log_msg,
                         "MSH Err %d; unable to set client sock receive timeout",
                         MSH_ERROR_SETSOCKOPT );
                IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
                sock_struct->result = MSH_ERROR_SETSOCKOPT;
                return sock_struct;
            }
        }

        sock_struct->result = MSH_CLIENT_CONNECTED;
        sock_struct->csd = local_client_sd;


#ifdef DEBUG_MSH
            printf( "msg_sock_hdlr_listen returning with client successfully connected \n" );
            printf( "msg_sock_hdlr_listen  dumping sock_struct:\n" );
            sock_struct_dump( sock_struct );
#endif

        return sock_struct;

    } // End while(...)
    
    sock_struct->result = MSH_ERROR_NOCONNECT;
    sock_struct->csd = 0;

    return sock_struct;

} // End msg_sock_hdlr_listen(...)


/**************************************************************************/
sock_struct_t *msg_sock_hdlr_recv( sock_struct_t *sock_struct, 
                                   char *message_buf,
                                   const int message_buf_len,
                                   int *shutdownFlag,
                                   const bool sendAck )
{
    // First, do some simple validation on the input structure -
    if( sock_struct->lsd == 0 ) sock_struct->valid = 0;
    if( sock_struct->csd == 0 ) sock_struct->valid = 0;
    if( sock_struct->lto < 0 ) sock_struct->valid = 0;
    if( sock_struct->cto < 0 ) sock_struct->valid = 0;

    if( ! sock_struct->valid ) {
        sprintf( full_log_msg,
                 "MSH Err %d; invalid sock_struct on read attempt",
                 MSH_INVALID_SOCKSTRUCT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_INVALID_SOCKSTRUCT;

#ifdef DEBUG_MSH
        printf( "msg_sock_hdlr_recv invalid struct, returning \n" );
#endif

        return sock_struct;
    }

    int local_client_sd = sock_struct->csd;
    bool set_cli_timeout = ( sock_struct->cto > 0 );

    // Note to Developer - set rd_buf to 4 chars long and monitor debug
    // output (requires Makefile CPPFLAGS += -DDEBUG_MSH) for insight
    // into following read-loop logic, especially when running
    // msg_sock_hdlr_test in boundary condition test.
    // Why 1024?  It really doesn't matter - it will loop to get all
    // the input, up to the size of the provided message_buf.
    char rd_buf[1024] = {0};

    int bytes_read = 0; // Each read
    int message_size = 0; // Total message
    int trailing_null_byte = 1; // self-documenting, eh?
    int add_missing_trailing_null_byte = 0;

    // Fall through while loop => connection lost during read op
    int return_code = MSH_MESSAGE_NOT_RECVD;

#ifdef DEBUG_MSH
    printf( "msg_sock_hdlr_recv recvg msg ... first, dumping sock_struct:\n" );
    sock_struct_dump( sock_struct );
#endif

    // There is a tricky circumstance that must be accounted for in this
    // logic.  IF the sent message length is an even multiple of the
    // byte-length of rd_buf (in this example purposely made small to
    // illustrate multiple read calls), then the loop would hang -
    // proof is left as an exercise for the skeptic - it's been proven!
    // So, what must be done?  The client and server must agree to the
    // following contract - every message sent will be terminated with
    // a null byte.  For strings, that means sending strlen(msg)+1.
    // A null byte will be searched for here; look in msg_sock_hdlr_send()
    // for it to be sent.
    while( ( bytes_read = read( local_client_sd , rd_buf, sizeof( rd_buf ) ) ) > 0 ) {

#ifdef DEBUG_MSH
        printf( "msg_sock_hdlr_recv read-loop, bytes_read: %d\n", bytes_read );
#endif

        // COULD check if the read contents IS null-byte terminated, and, if so,
        // relax this test by one byte ... ya, let's do that ...
        add_missing_trailing_null_byte = trailing_null_byte;
        if( rd_buf[ bytes_read-1 ] == '\0' ) add_missing_trailing_null_byte = 0;
        if( message_size + bytes_read + add_missing_trailing_null_byte > message_buf_len ) {

            // Note, re: above comment, what we HAVE NOT done is detect
            // an overflow situation and 'try to salvage' all that we
            // could from the last rd_buf load and put that in the 
            // message_buf -- please note: we DO have overflow so ...
            // let's stop there and report the error (best practice)
            // of course, returning what we did handle from the recv
            // as previously copied to message_buff.
#ifdef DEBUG_MSH
            printf( "msg_sock_hdlr_recv buffer (message_buf) overflow.\n" );
            printf( "Rejecting last-read input (rd_buf).  Exiting.\n" );
#endif

            sprintf( full_log_msg,
                     "MSH Err %d; msg_sock_hdlr_recv buf overflow",
                     MSH_MESSAGE_RECVD_OVERFLOW );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );

            message_size = -1 * message_size; // message_buf is essentially bad
            return_code = MSH_MESSAGE_RECVD_OVERFLOW;
            break; // jump out of while loop
        }

        // Dereferencing message_buf to write into caller-alloc'd memory
        memcpy( message_buf + message_size, rd_buf, bytes_read );

        return_code = MSH_MESSAGE_RECVD;

#ifdef DEBUG_MSH
        printf( "msg_sock_hdlr_recv current message_buf: %s\n", message_buf );
#endif
        message_size += bytes_read;

#ifdef DEBUG_MSH
        printf( "Now, msg_sock_hdlr_recv message_size: %d\n", message_size ); 
#endif

        if( bytes_read < sizeof( rd_buf ) ) {

#ifdef DEBUG_MSH_RETIRED
            printf( "Found a rec'd byte count less than sizeof rd_buf -\n"
                    "interpret that to mean 'end of send'.\n" );
#endif

            break; // That was the last read - good job!  Work here is done.
        }

        // Now test for that terminating null byte - another 'done' case.
        // Oh, and the careful reader may be thinking 'Gee, you could use
        // this test for every case, not JUST the even-multiple circumstance,
        // and skip the above test for end of send, right?' ...
        // To which I'd say 'RIGHT, but then I could not illustrate this
        // circumstance with different sleep-second-values in the client
        // startup, and the lesson would be overlooked (and maybe a bug
        // left unfound for someone).  So, I'll leave both tests in.'
        if( message_buf[ message_size - 1] == '\0' ) {

#ifdef DEBUG_MSH_RETIRED
            printf( "Found that pesky even-multiple case, but the null byte\n"
                    "rec'd indicates we're done - by contract (see 'end of send'\n"
                    "comments in source code above).\n" );
#endif

            break;
        }
    } // End while( read > 0 )

#ifdef DEBUG_MSH
    printf( "at msg_sock_hdlr_recv bytes_read < 1. Specifically: %d.\n", bytes_read );
#endif


    // Finding is that a timeout is bytes_read == -1 and a lost connection
    // is bytes_read == 0
    if( bytes_read == 0 ) {
        // Assume we lost the connection - sender will reconnect if necessary

        // This call may change sock_struct->result again
        bool wasEpipe = check_for_broken_socket( sock_struct );
        if( wasEpipe ) return_code = sock_struct->result;

#ifdef DEBUG_MSH
        printf( "Send ack failure was EPIPE? %s\n", (wasEpipe ? "Yes" : "No") );
#endif

    }

    if( bytes_read < 0 ) {
        // Do we have a timeout situation?  If so, we are NOT going to read
        // more but we WILL change the return code
        if( set_cli_timeout && ( errno == EWOULDBLOCK || errno == EAGAIN ) ) { 

#ifdef DEBUG_MSH
            printf( "msg_sock_hdlr_recv IS timeout (bytes_read<0).\n" );
#endif

            return_code = MSH_MESSAGE_RECV_TIMEOUT;
        }

#ifdef DEBUG_MSH
        else {
            // OK, bytes_read < 0, but NOT a timeout - no use case presently
            printf( "***************************************************\n" );
            printf( "at msg_sock_hdlr_recv bytes_read < 1. Specifically: %d.\n", bytes_read );
            printf( "***************************************************\n" );
        }
#endif

    }
    if( bytes_read > 0 && sendAck ) {

#ifdef DEBUG_MSH_RETIRED
        printf( "ACK will be sent next\n" );
#endif

        // Send an ACK
        char ack_response[ACK_MSG_BUF_LEN] = { 0 };
        sprintf( ack_response, ":ACK:ByteCount:%d", message_size );

        if( send( local_client_sd, ack_response, strlen( ack_response ), 0 ) == -1 ) {
            return_code = MSH_ERROR_ACK_SEND_FAIL;
 
            // This call may change sock_struct->result again
            bool wasEpipe = check_for_broken_socket( sock_struct );

#ifdef DEBUG_MSH
            printf( "Send ack failure was EPIPE? %s\n", (wasEpipe ? "Yes" : "No") );
#endif

        }

#ifdef DEBUG_MSH_RETIRED
        printf( "ACK msg sent: %s\n", (bytes_read > 0 ? ack_response : "no ack sent") );
#endif

    }

#ifdef DEBUG_MSH_RETIRED
    printf( "End of while( 'read > 0' ) - bytes read: <%d>\n", bytes_read );
    printf( "Received <%s>\n", message_buf );
    printf( "Return code: %s.\n", MSH_DEFINE_NAME( return_code ) );
#endif

    sock_struct->result = return_code;
    return sock_struct;

} /* End msg_sock_hdlr_recv() */


/**************************************************************************/
sock_struct_t *msg_sock_hdlr_open_for_send( sock_struct_t *sock_struct )
{
    // First, do some simple validation on the input structure -
    if( ! sock_struct->valid ) {
        sprintf( full_log_msg,
                 "MSH Err %d; invalid sock_struct on send attempt",
                 MSH_INVALID_SOCKSTRUCT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_INVALID_SOCKSTRUCT;
        return sock_struct;
    }
    
    if( ! SIG_IGN_SET ) {
        set_sigaction_ign_sigpipe();
        // Call once per process invocation
        SIG_IGN_SET = true;
    }

    bool set_cli_timeout = ( sock_struct->cto > 0 );

    struct timeval tmrecv, tmsend;
    tmrecv.tv_sec = sock_struct->cto;
    tmrecv.tv_usec=0;
    tmsend.tv_sec = sock_struct->cto;
    tmsend.tv_usec=0;

    int local_client_sd, numbytes;
    struct addrinfo hints, *servinfo, *ptr_addrinfo;
    int return_val;

    memset( &hints, 0, sizeof hints );
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port[6] = { 0 }; // Port is a 16-bit int; big enough
    sprintf( port, "%d", sock_struct->port );
    if( ( return_val = getaddrinfo( sock_struct->host, port, &hints, &servinfo ) ) != 0 ) {

#ifdef DEBUG_MSH
        fprintf( stderr, "Call getaddrinfo failed: %s\n", gai_strerror( return_val ) );
#endif

        sock_struct->result = MSH_ERROR_GETADDRINFO;
        return sock_struct;
    }

    // loop through all the results and connect to the first we can
    for( ptr_addrinfo = servinfo; ptr_addrinfo != NULL; ptr_addrinfo = ptr_addrinfo->ai_next ) {
        if( ( local_client_sd =
              socket( ptr_addrinfo->ai_family, ptr_addrinfo->ai_socktype, ptr_addrinfo->ai_protocol ) ) == -1 ) {
            // DEBUG perror( "Client: socket" );
            continue;
        }
        if( connect( local_client_sd, ptr_addrinfo->ai_addr, ptr_addrinfo->ai_addrlen ) == -1 ) {
            // DEBUG perror( "Client: connect" );
            close( local_client_sd );
            continue;
        }
        break;
    }

    if( ptr_addrinfo == NULL ) {

#ifdef DEBUG_MSH
        fprintf( stderr, "Failed to connect to service at this time.  Exiting.\n" );
#endif

        sock_struct->result = MSH_ERROR_NOCONNECT;
        return sock_struct;
    }

    freeaddrinfo( servinfo ); // all done with this structure

 
    if( set_cli_timeout ) {

#ifdef DEBUG_MSH
        printf( "Setting Client Timeouts\n" );
#endif

        if( setsockopt( local_client_sd, SOL_SOCKET, SO_SNDTIMEO, &tmsend, sizeof tmsend) < 0 ) {
            sprintf( full_log_msg,
                     "MSH Err %d; unable to set client sock send timeout",
                     MSH_ERROR_SETSOCKOPT );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
            sock_struct->result = MSH_ERROR_SETSOCKOPT;
            return sock_struct;
        }
        if( setsockopt( local_client_sd, SOL_SOCKET, SO_RCVTIMEO, &tmrecv, sizeof tmrecv ) < 0 ) {
            sprintf( full_log_msg,
                     "MSH Err %d; unable to set client sock receive timeout",
                     MSH_ERROR_SETSOCKOPT );
            IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
            sock_struct->result = MSH_ERROR_SETSOCKOPT;
            return sock_struct;
        }
    }

    sock_struct->result = MSH_CLIENT_CONNECTED;
    // NOTE on terminology - assigning this server socket descriptor
    // to the 'client socket descriptor' ... MEANING that AS A
    // CLIENT, we communicate over this socket (to server).
    sock_struct->csd = local_client_sd;
    return sock_struct;

} /* End msg_sock_hdlr_open_for_send(...) */


/**************************************************************************/
sock_struct_t *msg_sock_hdlr_send( sock_struct_t *sock_struct,
                                   const char *message_buf,
                                   const bool awaitAck )
{
    // First, do some simple validation on the input structure -
    if( sock_struct->csd == 0 ) sock_struct->valid = 0;

    if( ! sock_struct->valid ) {
        sprintf( full_log_msg,
                 "MSH Err %d; invalid sock_struct on send attempt",
                  MSH_INVALID_SOCKSTRUCT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct->result = MSH_INVALID_SOCKSTRUCT;
        return sock_struct;
    }

#ifdef DEBUG_MSH_DELAY_CLIENT_SEND___WARNING_ADDS_WAITS_IN_SEND
    // Special test case to insert sleep BETWEEN client socket
    // connect and socket send (enabled in Makefile)
    printf( "Delaying client send after connect by 15 secs.\n" );
    sleep( 15 );
#endif

    // NOTE on terminology - retreiving this socket descriptor (to the
    // server) from the opened 'client socket descriptor' ... MEANING
    // that AS A CLIENT, we communicate over this socket (to server).
    int local_client_sd = sock_struct->csd;


#ifdef DEBUG_MSH
    printf( "msg_sock_hdlr_send send msg ... first, dumping sock_struct:\n" );
    sock_struct_dump( sock_struct );
#endif

    int msgSendResult =
        send( local_client_sd, message_buf, strlen( message_buf )+1, 0 );

#ifdef DEBUG_MSH
        printf( "In msg_sock_hdlr_send(...), msgSendResult: %d.\n", msgSendResult );
#endif

    if( msgSendResult == -1 ) {

#ifdef DEBUG_MSH
        printf( "Message send failed.  Returning.\n" );
#endif

        sock_struct->result = MSH_MESSAGE_NOT_SENT;

        // This call may change sock_struct->result again
        bool wasEpipe = check_for_broken_socket( sock_struct );

#ifdef DEBUG_MSH
        printf( "Send failure was EPIPE? %s\n", (wasEpipe ? "Yes" : "No") );
#endif

        return sock_struct;
    }

    sock_struct->result = MSH_MESSAGE_SENT;

    // Recv an ACK
    char ack_response[ACK_MSG_BUF_LEN] = { 0 };
    int bytes_read;

    if( awaitAck ) {
#ifdef DEBUG_MSH_RETIRED
        printf( "ACK recv is next\n" );
#endif

        memset( ack_response, 0, ACK_MSG_BUF_LEN );
        if( ( bytes_read = read( local_client_sd, ack_response, ACK_MSG_BUF_LEN ) ) == -1 ) {
            sock_struct->result = MSH_ERROR_ACK_RECV_FAIL;

            // This call may change sock_struct->result again
            bool wasEpipe = check_for_broken_socket( sock_struct );

#ifdef DEBUG_MSH
            printf( "Read ack failure was EPIPE? %s\n", (wasEpipe ? "Yes" : "No") );
#endif

        }

#ifdef DEBUG_MSH_RETIRED
        printf( "ACK bytes_read: %d\n", bytes_read );
        printf( "ACK rec'd: %s\n", (bytes_read > 0 ? ack_response : "no ack rcv'd") );
#endif
    }

#ifdef DEBUG_MSH
    printf( "msg_sock_hdlr_send sent, returning.\n" );
#endif

    return sock_struct;

} /* End msg_sock_hdlr_send(...) */


/**************************************************************************/
bool check_for_broken_socket( sock_struct_t * sock_struct )
{
    if( errno == EPIPE ) {
        // This socket connection is broken - take action
        sprintf( full_log_msg,
                 "MSH Err %d; send() failure; closing client send socket",
                 MSH_MESSAGE_NOT_SENT );
        IWAY_LOG( IWAY_LOG_ERROR, full_log_msg );
        sock_struct_close_client( sock_struct );
        sock_struct->result = MSH_CLIENT_DISCONNECTED;
        return true;
    }
    return false;
}

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#include "process.h"

// messaging config
int WINDOW_SIZE;
int MAX_DELAY;
int TIMEOUT;
int DROP_RATE;

// process information
process_t myinfo;

int mailbox_id;
// a message id is used by the receiver to distinguish a message from other messages
// you can simply increment the message id once the message is completed
int message_id = 0;

// the message status is used by the sender to monitor the status of a message
message_status_t message_stats;
// the message is used by the receiver to store the actual content of a message
message_t message;

int num_available_packets;  // number of packets that can be sent (0 <= n <= WINDOW_SIZE)
int is_receiving = 0;   // a helper varibale may be used to handle multiple senders
int timeoutNum;
int msqid;      // sender message queue id

int previous_sender = 0;    // Use to find out last role. 

/*
    When hdlNum is 0, blockSig will block SIGIO and SIGALRM
    When hdlNum is 1, blockSig will unblock SIGIO and SIGALRM
*/
int blockSig( int hdlNum ) {
    sigset_t newsigset;
    sigset_t newsigset1; 
    if ( ( sigemptyset( &newsigset ) == -1 ) || ( sigaddset( &newsigset, SIGIO ) == -1 ) ) {
        perror( "Failed to initialize the signal set" );
        return -1;
    }
    if ( ( sigemptyset( &newsigset1 ) == -1 ) || ( sigaddset( &newsigset1, SIGALRM ) == -1 ) ) {
        perror( "Failed to initialize the signal set" );
        return -1;
    }
    if ( hdlNum == 1 ) {    // Unblock signals 
        if ( sigprocmask( SIG_UNBLOCK, &newsigset, NULL ) == -1 ) {
            perror( "Failed to unblock SIGIO" );
            return -1; 
        }
        if ( sigprocmask( SIG_UNBLOCK, &newsigset1, NULL ) == -1 ) {
            perror( "Failed to unblock SIGALARM" );
            return -1; 
        }
    } else if ( hdlNum == 0 ) {     // Block signals
        if ( sigprocmask( SIG_BLOCK, &newsigset, NULL ) == -1 ) {
            perror( "Failed to block SIGIO" );
            return -1; 
        }
        if ( sigprocmask( SIG_BLOCK, &newsigset1, NULL ) == -1 ) {
            perror( "Failed to block SIGALARM" );
            return -1; 
        }
    }
    return 0;
}


/**
 * TODO complete the definition of the function
 * 1. Save the process information to a file and a process structure for future use.
 * 2. Setup a message queue with a given key.
 * 3. Setup the signal handlers (SIGIO for handling packet, SIGALRM for timeout).
 * Return 0 if success, -1 otherwise.
 */
int init( char *process_name, key_t key, int wsize, int delay, int to, int drop ) {
    myinfo.pid = getpid();
    strcpy( myinfo.process_name, process_name );
    myinfo.key = key;

    // open the file
    FILE* fp = fopen( myinfo.process_name, "wb" );
    if ( fp == NULL ) {
        printf( "Failed opening file: %s\n", myinfo.process_name );
        return -1;
    }
    // write the process_name and its message keys to the file
    if ( fprintf( fp, "pid:%d\nprocess_name:%s\nkey:%d\n", myinfo.pid, myinfo.process_name, myinfo.key ) < 0 ) {
        printf( "Failed writing to the file\n" );
        return -1;
    }
    fclose( fp );

    WINDOW_SIZE = wsize;
    MAX_DELAY = delay;
    TIMEOUT = to;
    DROP_RATE = drop;

    printf( "[%s] pid: %d, key: %d\n", myinfo.process_name, myinfo.pid, myinfo.key );
    printf( "window_size: %d, max delay: %d, timeout: %d, drop rate: %d%%\n", WINDOW_SIZE, MAX_DELAY, TIMEOUT, DROP_RATE );

    // TODO setup a message queue and save the id to the mailbox_id
    int msqid;
    if ( ( msqid = msgget( key, 0666 | IPC_CREAT ) ) == -1 ) {  
        perror( "msgget" );
        return -1;
    }
    mailbox_id = msqid;

    // TODO set the signal handler for receiving packets
    struct sigaction newact;
    struct sigaction newact1;
    
    // Choose appropriate handler based on the input
    newact.sa_handler = receive_packet;
    newact1.sa_handler = timeout_handler;

    // set signal handler for SIGIO
    newact.sa_flags = 0;
    if ( ( sigfillset( &newact.sa_mask ) == -1 ) || ( sigaction( SIGIO, &newact, NULL ) == -1 ) ) {
        perror( "Failed to install SIGIO signal handler" );
        return -1;    
    }
    // set signal handler for SIGALARM
    newact1.sa_flags = 0;
    if ( ( sigfillset( &newact1.sa_mask) == -1 ) || ( sigaction( SIGALRM, &newact1, NULL ) == -1 ) ) {
        perror( "Failed to install SIGALRM signal handler" );
        return -1;    
    }
    blockSig( 0 );      // Block signals
    return 0;
}

/**
 * Get a process' information and save it to the process_t struct.
 * Return 0 if success, -1 otherwise.
 */
int get_process_info( char *process_name, process_t *info ) {
    char buffer[ MAX_SIZE ];
    char *token;

    // open the file for reading
    FILE* fp = fopen( process_name, "r" );
    if ( fp == NULL ) {
        return -1;
    }
    // parse the information and save it to a process_info struct
    while ( fgets( buffer, MAX_SIZE, fp ) != NULL ) {
        token = strtok( buffer, ":" );
        if ( strcmp( token, "pid" ) == 0 ) {
            token = strtok( NULL, ":" );
            info->pid = atoi( token );
        } else if ( strcmp( token, "process_name" ) == 0 ) {
            token = strtok( NULL, ":" );
            strcpy( info->process_name, token );
        } else if ( strcmp( token, "key" ) == 0 ) {
            token = strtok( NULL, ":" );
            info->key = atoi( token );
        }
    }
    fclose( fp );
    return 0;
}

/**
 * TODO Send a packet to a mailbox identified by the mailbox_id, and send a SIGIO to the pid.
 * Return 0 if success, -1 otherwise.
 */
int send_packet( packet_t *packet, int mailbox_id, int pid ) {
    // Send a packet 
    if ( msgsnd( mailbox_id, (void *) packet, sizeof( packet_t ) - sizeof( long ), 0 ) == -1 ) {
        perror( "msgsnd" );
        return -1;
    }
    // send out SIGIO
    if ( kill( pid , SIGIO ) == -1 ) {
        perror( "Failed sending signal SIGIO" );
        return -1;
    }
    return 0;
}

/**
 * Get the number of packets needed to send a data, given a packet size.
 * Return the number of packets if success, -1 otherwise.
 */
int get_num_packets( char *data, int packet_size ) {
    if ( data == NULL ) {
        return -1;
    }
    if ( strlen( data ) % packet_size == 0 ) {
        return strlen( data ) / packet_size;
    } else {
        return ( strlen( data ) / packet_size ) + 1;
    }
}

/**
 * Create packets for the corresponding data and save it to the message_stats.
 * Return 0 if success, -1 otherwise.
 */
int create_packets( char *data, message_status_t *message_stats ) {
    if ( data == NULL || message_stats == NULL ) {
        return -1;
    }
    int i, len;
    for ( i = 0; i < message_stats -> num_packets; i++ ) {
        if ( i == message_stats -> num_packets - 1 ) {
            len = strlen( data ) - ( i * ( PACKET_SIZE - 1 ) );
        } else {
            len = PACKET_SIZE - 1;
        }
        message_stats -> packet_status[ i ].is_sent = 0;
        message_stats -> packet_status[ i ].ACK_received = 0;
        message_stats -> packet_status[ i ].packet.message_id = -1;
        message_stats -> packet_status[ i ].packet.mtype = DATA;
        message_stats -> packet_status[ i ].packet.pid = myinfo.pid;
        strcpy( message_stats -> packet_status[ i ].packet.process_name, myinfo.process_name );
        message_stats -> packet_status[ i ].packet.num_packets = message_stats -> num_packets;
        message_stats -> packet_status[ i ].packet.packet_num = i;
        message_stats -> packet_status[ i ].packet.is_firstPacket = 0;
        message_stats -> packet_status[ i ].packet.total_size = strlen( data );
        memcpy( message_stats -> packet_status[ i ].packet.data, data + ( i * ( PACKET_SIZE - 1 ) ), len );
        message_stats -> packet_status[ i ].packet.data[ len ] = '\0';
    }
    return 0;
}

/**
 * Get the index of the next packet to be sent.
 * Return the index of the packet if success, -1 otherwise.
 */
int get_next_packet( int num_packets ) {
    int packet_idx = rand() % num_packets;
    int i = 0;

    i = 0;
    while ( i < num_packets ) {
        if ( message_stats.packet_status[ packet_idx ].is_sent == 0 ) {
            // found a packet that has not been sent
            return packet_idx;
        } else if ( packet_idx == num_packets - 1 ) {
            packet_idx = 0;
        } else {
            packet_idx++;
        }
        i++;
    }
    // all packets have been sent
    return -1;
}

/**
 * Use probability to simulate packet loss.
 * Return 1 if the packet should be dropped, 0 otherwise.
 */
int drop_packet() {
    if ( rand() % 100 > DROP_RATE ) {
        return 0;
    }
    return 1;
}

/**
 * TODO Send a message (broken down into multiple packets) to another process.
 * We first need to get the receiver's information and construct the status of
 * each of the packet.
 * Return 0 if success, -1 otherwise.
 */
int send_message( char *receiver, char* content ) {
    blockSig( 1 );  // Unblock signals
    previous_sender = 1;

    // Check during role switching, how many message coming in the message queue.
    struct msqid_ds buf;
    if ( msgctl( mailbox_id, IPC_STAT, &buf ) == -1 ) {
        return -1;
    }
    int num_msg_left = buf.msg_qnum;

    // Read all old messages in the queue (clean up the queue). 
    while ( num_msg_left > 0 ) {
        packet_t pkt;
        if ( msgrcv( mailbox_id, &pkt, sizeof( packet_t ) - sizeof( long ), 0, 0 ) == -1 ) {
            perror( "msgrcv" );
            exit( 0 );
        }
        num_msg_left--; 
    }

    if ( receiver == NULL || content == NULL ) {
        printf( "Receiver or content is NULL\n" );
        blockSig( 0 );  // Block signals
        return -1;
    }
    // get the receiver's information
    if ( get_process_info( receiver, &message_stats.receiver_info ) < 0 ) {
        printf( "Failed getting %s's information.\n", receiver );
        blockSig( 0 );  // Block signals
        return -1;
    }
    // get the receiver's mailbox id
    message_stats.mailbox_id = msgget( message_stats.receiver_info.key, 0666 );
    if ( message_stats.mailbox_id == -1 ) {
        printf( "Failed getting the receiver's mailbox.\n" );
        blockSig( 0 );  // Block signals
        return -1;
    }
    // get the number of packets
    int num_packets = get_num_packets( content, PACKET_SIZE - 1 );
    if ( num_packets < 0 ) {
        printf( "Failed getting the number of packets.\n" );
        blockSig( 0 );  // Block signals       
        return -1;
    }
    // set the number of available packets
    if ( num_packets > WINDOW_SIZE ) {
        num_available_packets = WINDOW_SIZE;
    } else {
        num_available_packets = num_packets;
    }
    // setup the information of the message
    message_stats.is_sending = 1;
    message_stats.num_packets_received = 0;
    message_stats.num_packets = num_packets;
    message_stats.packet_status = malloc( num_packets * sizeof( packet_status_t ) );
    if ( message_stats.packet_status == NULL ) {
        blockSig( 0 );  // Block signals
        return -1;
    }
    // parition the message into packets
    if ( create_packets( content, &message_stats ) < 0 ) {
        printf( "Failed paritioning data into packets.\n" );
        message_stats.is_sending = 0;
        free( message_stats.packet_status );
        blockSig( 0 );  // Block signals
        return -1;
    }

    // TODO send packets to the receiver
    // the number of packets sent at a time depends on the WINDOW_SIZE.
    // you need to change the message_id of each packet (initialized to -1)
    // with the message_id included in the ACK packet sent by the receiver
    int i = 0, index = 0, msgid, rcvCount = 1;
    timeoutNum = 0;

    // Sent out the first packet to get message_id. 
    if ( ( index = get_next_packet( num_packets ) ) == -1 ) { // return -1 when all packets has been sent   
        blockSig( 0 );  // Block signals
        return -1;
    }    
    message_stats.packet_status[ index ].packet.is_firstPacket = 1;
    send_packet( &message_stats.packet_status[ index ].packet, message_stats.mailbox_id, message_stats.receiver_info.pid );
    printf( "Send a packet [%d] to pid %d\n", index, message_stats.receiver_info.pid );
    message_stats.packet_status[ index ].is_sent = 1;
    alarm( TIMEOUT );
    while ( 1 ) {   //  Keep sending first packet until MAX_TIMEOUT or receive ACK
        pause();
        if ( message_stats.packet_status[ index ].ACK_received == 1 )
            break;
        if ( timeoutNum >= MAX_TIMEOUT ) {    // Reach MAX_TIMEOUT, receiver is receiving other senders. 
            alarm( 0 );
            blockSig( 0 );  // Block signals
            return -1;
        }    
    }
    timeoutNum = 0;    
    msgid = message_stats.packet_status[ index ].packet.message_id;
    while ( message_stats.num_packets_received != message_stats.num_packets ) { 
        while ( num_available_packets > 0 ) {       // Can not exceed window size
            if ( ( index = get_next_packet( num_packets ) ) == -1 ) 
                break;      
            message_stats.packet_status[ index ].packet.message_id = msgid;     // Update message id.   
            send_packet(  &message_stats.packet_status[ index ].packet, message_stats.mailbox_id, message_stats.receiver_info.pid );
            printf( "Send a packet [%d] to pid %d\n", index, message_stats.receiver_info.pid );
            message_stats.packet_status[ index ].is_sent = 1;
            num_available_packets--;  
        }
        alarm( TIMEOUT );
        pause();
        if ( rcvCount != message_stats.num_packets_received ) {   // Timeout is not successive, one ACK is received.
            timeoutNum = 0;
            rcvCount = message_stats.num_packets_received;
        }    
        if ( timeoutNum >= MAX_TIMEOUT ) {    // MAX_TIMEOUT of successive timeout, receiver is listening to other sender
            alarm( 0 );
            blockSig( 0 );  // Block signals
            printf("Reach max timeout\n");
            return -1; 
        }
    }
    // Cancel previous alarm
    alarm( 0 );
    message_stats.is_sending = 0;
    printf( "All packets sent.\n" );

    blockSig( 0 );  // Block signals

    return 0;
}

/**
 * TODO Handle TIMEOUT. Resend previously sent packets whose ACKs have not been
 * received yet. Reset the TIMEOUT.
 */
void timeout_handler( int sig ) {
    int i = 0;
    for ( ; i < message_stats.num_packets; i++ ) {
        if ( message_stats.packet_status[ i ].is_sent == 1 && message_stats.packet_status[ i ].ACK_received == 0 ) {
            send_packet(  &message_stats.packet_status[ i ].packet, message_stats.mailbox_id, message_stats.receiver_info.pid );
            printf( "TIMEOUT! Send a packet [%d] to pid %d\n", i, message_stats.receiver_info.pid );
        }
    }
    timeoutNum++;
    alarm( TIMEOUT );
}

/**
 * TODO Send an ACK to the sender's mailbox.
 * The message id is determined by the receiver and has to be included in the ACK packet.
 * Return 0 if success, -1 otherwise.
 */
int send_ACK( int mailbox_id, pid_t pid, int packet_num, int old_msgid ) {
    // TODO construct an ACK packet
    packet_t ack;
    ack.mtype = 2;
    if ( old_msgid != -2 ) 
        ack.message_id = old_msgid;
    else
        ack.message_id = message_id;
    ack.pid = myinfo.pid;
    strcpy( ack.process_name, myinfo.process_name );
    ack.num_packets = 1;
    ack.packet_num = packet_num;
    strcpy( ack.data, "ACK" );
    ack.total_size = strlen( ack.data );

    int delay = rand() % MAX_DELAY;
    sleep( delay );

    // TODO send an ACK for the packet it received
    if ( send_packet( &ack, mailbox_id, pid ) == -1 ) 
        return -1;
    return 0;
}

/**
 * TODO Handle DATA packet. Save the packet's data and send an ACK to the sender.
 * You should handle unexpected cases such as duplicate packet, packet for a different message,
 * packet from a different sender, etc.
 */
void handle_data( packet_t *packet, process_t *sender, int sender_mailbox_id ) {
    int packetIndex = packet -> packet_num;
    // New packet is not a duplicate packet.
    if ( message.is_received[ packetIndex ] == 0 ) {  
        message.is_received[ packetIndex ] = 1; 
        message.num_packets_received++;
        message.data[ packet -> packet_num ] = ( char * )malloc( strlen( packet -> data ) + 1 );
        strcpy( message.data[ packetIndex ], packet -> data ); 
        send_ACK( sender_mailbox_id, sender -> pid, packetIndex, -2 );
        printf("Send an ack for packet [%d] to pid %d\n", packetIndex, sender -> pid );
        if ( packet -> num_packets == message.num_packets_received ) {
            message.is_complete = 1;
        }
    } else if ( message.is_received[ packetIndex ] == 1 ) {     // Duplicate packet.
        send_ACK( sender_mailbox_id, sender -> pid, packetIndex, -2 );
        printf("Send an ack for packet [%d] to pid %d\n", packetIndex, sender -> pid );
    }
}

/**
 * TODO Handle ACK packet. Update the status of the packet to indicate that the packet
 * has been successfully received and reset the TIMEOUT.
 * You should handle unexpected cases such as duplicate ACKs, ACK for completed message, etc.
 */
void handle_ACK( packet_t *packet ) {
    int packetIndex = packet -> packet_num;
    //printf("data is: %s\n", packet -> data);
    // First packet
    if ( message_stats.packet_status[ packetIndex ].ACK_received == 0 && \
        message_stats.packet_status[ packetIndex ].packet.is_firstPacket == 1 ) {
        //&& message_stats.receiver_info.pid == packet -> pid ) {   
        message_stats.packet_status[ packetIndex ].packet.message_id = packet -> message_id;
        message_stats.packet_status[ packetIndex ].ACK_received = 1;
        message_stats.num_packets_received++;
        printf( "Receive an ack for packet [%d]\n", packet -> packet_num );
    }
    // ACK for uncompleted message
    else if ( packet -> message_id == message_stats.packet_status[ packetIndex ].packet.message_id && \
        message_stats.packet_status[ packetIndex ].ACK_received == 0 ) {   
        message_stats.num_packets_received++;
        message_stats.packet_status[ packetIndex ].ACK_received = 1;
        num_available_packets++;
        printf( "Receive an ack for packet [%d]\n", packet -> packet_num );
        alarm( TIMEOUT );
    }
    // All other packets, duplicated ACKs will be ignored. 
}

/**
 * Get the next packet (if any) from a mailbox.
 * Return 0 (false) if there is no packet in the mailbox
 */
int get_packet_from_mailbox( int mailbox_id ) {
    struct msqid_ds buf;

    return ( msgctl( mailbox_id, IPC_STAT, &buf ) == 0 ) && ( buf.msg_qnum > 0 );
}

/**
 * TODO Receive a packet.
 * If the packet is DATA, send an ACK packet and SIGIO to the sender.
 * If the packet is ACK, update the status of the packet.
 */
void receive_packet( int sig ) {

    // TODO you have to call drop_packet function to drop a packet with some probability
    // if (drop_packet()) {
    //     ...
    // }
    packet_t pkt;
    if ( get_packet_from_mailbox( mailbox_id ) ) {  // There are at least a packet in the message queue
        if ( msgrcv( mailbox_id, &pkt, sizeof( packet_t ) - sizeof( long ), 0, 0 ) == -1 ) {
            perror( "msgrcv" );
            exit( 0 );
        }
    }
    if ( !drop_packet() ) {
        if ( message_stats.is_sending == 0 && pkt.mtype == 1 ) {      // Packet is DATA
            if ( !is_receiving && pkt.message_id == -1 ) {      // First packet in first message
                get_process_info( pkt.process_name, &message.sender );
                if ( ( msqid = msgget( message.sender.key, 0666 ) ) == -1 ) {  
                    perror( "msgget" );
                    exit( 0 );
                }
                is_receiving = 1;
                message.num_packets_received = 1;
                // Allocate memory and initialized all is_received to 0
                message.is_received = ( int * )calloc( pkt.num_packets, sizeof( int ) );
                message.is_received[ pkt.packet_num ] = 1;
                if ( pkt.num_packets == 1 )
                    message.is_complete = 1;
                message.data = ( char ** )malloc( pkt.num_packets * sizeof( char * ) );
                message.data[ pkt.packet_num ] = ( char * )malloc( strlen( pkt.data ) + 1 );
                strcpy( message.data[ pkt.packet_num ], pkt.data );
                send_ACK( msqid, message.sender.pid, pkt.packet_num, -2 );
                printf( "Send an ack for packet [%d] to pid %d\n", pkt.packet_num, message.sender.pid );
            } else if ( ( message_id == pkt.message_id && pkt.pid == message.sender.pid ) ||\
                        ( pkt.message_id == -1 && pkt.pid == message.sender.pid ) ) {    // Packet from the same sender and same message
                handle_data( &pkt, &message.sender, msqid );
            } else if ( pkt.message_id < message_id && pkt.message_id > -1 ) {     // Packet from the old message;
                process_t previous_sender;
                int previous_qid;
                get_process_info( pkt.process_name, &previous_sender );
                if ( ( previous_qid = msgget( previous_sender.key, 0666 ) ) == -1 ) {  
                    perror( "msgget" );
                    exit( 0 );
                }
                send_ACK( previous_qid, pkt.pid, pkt.packet_num, pkt.message_id );
                printf( "Send an ack for packet [%d] to pid %d\n", pkt.packet_num, pkt.pid );
            }  
            
        } else if ( message_stats.is_sending == 1 && pkt.mtype == 2 ) {      // packet is ACK
            handle_ACK( &pkt );  
        }
    }
}

/**
 * TODO Initialize the message structure and wait for a message from another process.
 * Save the message content to the data and return 0 if success, -1 otherwise
 */
int receive_message( char *data ) {
    blockSig( 1 );  // Unblock signals

    message_stats.is_sending = 0;
    message.is_complete = 0;

    // Check during role switching, how many message coming in the message queue.
    struct msqid_ds buf;
    if ( msgctl( mailbox_id, IPC_STAT, &buf ) == -1 ) {
        return -1;
    }
    int num_msg_left = buf.msg_qnum;

    if ( previous_sender == 0 ) {   // Last role is receiver, so handle old messages. 
        // Handle old messages in the queue and possible new messages. 
        while ( num_msg_left > 0 ) {
            raise( SIGIO );
            num_msg_left--; 
        }
    } else {    // Last role is a sender. So discard messages in the message queue.     
        // Read all old messages in the queue (clean up the queue). 
        while ( num_msg_left > 0 ) {
            packet_t pkt;
            if ( msgrcv( mailbox_id, &pkt, sizeof( packet_t ) - sizeof( long ), 0, 0 ) == -1 ) {
                perror( "msgrcv" );
                exit( 0 );
            }
            num_msg_left--; 
        }
    }    
    previous_sender = 0;
    
    if ( data == NULL ) {
        blockSig( 0 );  // Block signals
        return -1;
    }
    strcpy( data, "" );
    while ( !message.is_complete ) {
        pause();     
    }
    
    int i = 0;
    for ( ; i < message.num_packets_received; i++ ) {
        printf("data %d is %s\n", i,message.data[ i ]);
        strcat( data , message.data[ i ] );
    }
    message_id++;
    printf( "All packets received.\n" );
    
    is_receiving = 0;

    // Clean up allocated memory 
    message.num_packets_received = 0;
    free( message.is_received );
    for ( i = 0 ; i < message.num_packets_received; i++ ) {
        free( message.data[ i ] );
    }
    free( message.data );
    blockSig( 0 );  // Block signals
    return 0;
}



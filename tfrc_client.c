/*
 * Author : Anjan Rayamajhi & Derek Johnson
 * TFRC client code
 * calculates transmit throughput based on the TFRC equation 
 * adds loss rate as placed in call	
 * 
 * */


#include <sys/types.h>
#include <string.h>     /* for memset() */
#include <netinet/in.h> /* for in_addr */
#include <sys/socket.h> /* for socket(), connect(), sendto(), and recvfrom() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <unistd.h>     /* for close() */
#include <netdb.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>

#include "clientresources.h"

#define PACKETDROP(p) ((double)rand()*1.0/RAND_MAX) < p ? 0 : 1  //  p=0 would be taken for no packet drop

sem_t lock;

void printruntime(int ignored)
{	
    printf("%8.6f %8.6f %8.6f %8.6f %8.6f %1.6f\n",tfrc_client.X_trans,tfrc_client.X_calc,tfrc_client.X_recv,tfrc_client.R_rtt,tfrc_client.t_RTO,tfrc_client.p);
    
    ualarm(500000,0); // reset for next 0.5 second
}


void catchCntrlTimeout(int ignored) // to be used by Initializing
{	

    tfrc_client.alarmtimeout =true;
    if(tfrc_client.cntrlTimeoutCounter++>MAXINITTRY)
        DieWithError(" Client:  Server Initialize Failed, Server not responding");
    alarm(0);
}

void CNTCCatch(int ignored)
{	
	sleep(1);
    cstate = CLIENT_STOP;
    tfrc_client.feedbackRecvd=false;
    
    usec3 = 0;
    
}


void *thread_receive()
{
    long receivedStrLen;
    tfrc_client.ServAddrLen = sizeof(tfrc_client.ServAddr);

    while(1)
    {
        switch(cstate)
        {
        case CLIENT_INIT:
            if((receivedStrLen = recvfrom(tfrc_client.sock, cntrl.controlmessage, MSGMAX, 0,
                                          (struct sockaddr *) &(tfrc_client.ServAddr), &(tfrc_client.ServAddrLen))) != CNTRLMSGSIZE)
                    printf(" Receive Error from Server at CLIENT_INIT !!\n");

            else
            {
                // check for correctness of the received ACK.
                
                if(*cntrl.msgType == CONTROL && *cntrl.msgCode==OK) //  server responded
                {	
                    cstate = CLIENT_START; // state change
                    tfrc_client.sessionTime = get_time()*MEG;
                    
                    tfrc_client.feedbackRecvd = true; // to start packet transfer
                    
                    usec1 = 0; // start the transmission timer

                    /*** Recurring results Display Alarm timer setup *****/

                    tfrc_client.displaytimer.sa_handler = printruntime;
                    if (sigfillset(&tfrc_client.displaytimer.sa_mask) < 0)
                        DieWithError("sigfillset() failed");

                    tfrc_client.displaytimer.sa_flags = 0;

                    if (sigaction(SIGALRM, &tfrc_client.displaytimer, 0) < 0)
                        DieWithError("sigaction failed for sigalarm");
                    ualarm(500000,0); //  start the alarm with 2 seconds
                    
                    


                }
            }
            break;
        case CLIENT_START:
            if((receivedStrLen = recvfrom(tfrc_client.sock, ack.ackmessage, MSGMAX, 0,
                                          (struct sockaddr *) &(tfrc_client.ServAddr), &(tfrc_client.ServAddrLen))) != ACKMSGSIZE)
                    printf(" Receive Error from Server at CLIENT_START !!\n");
            else
            {		
				tfrc_client.numReceived++;
				
                if(*ack.msgType == ACK && *ack.msgCode == OK)
                {
                    sem_wait(&lock);
                    tfrc_client.lastAckreceived = ntohl(*(ack.ackNum)); // assuming receiver responds to most recent ACK
                    
                    
                    if(tfrc_client.lastAckreceived >= tfrc_client.expectedACK){
						tfrc_client.feedbackRecvd = true; 
						tfrc_client.expectedACK=tfrc_client.lastAckreceived+1;
						
					}
					else
					{
						
					}
                    sem_post(&lock);
						 
                    tfrc_client.t_now = get_time()*MEG;

                    tfrc_client.t_recvdata = tfrc_client.timestore[ntohl(*(ack.seqnumrecvd))%TIMESTAMPWINDOW];
                    tfrc_client.t_delay = (double)ntohl(*(ack.t_Delay)); //  CHECK is t_delay in microseconds
                    tfrc_client.X_recv = (double)(ntohl(*(ack.receiveRate))/1000.0);
                    tfrc_client.p = (float)ntohl(*(ack.lossEventRate))/1000.0; // server sets p in int
                    tfrc_client.R_sample = (tfrc_client.t_now-tfrc_client.t_recvdata) ; //  CHANGES :: twice then in theory
				
		    tfrc_client.lossEventCounter +=tfrc_client.p;
						
						
                    if(tfrc_client.R_rtt == 0.0) //  usually the case for the first feedback
                        tfrc_client.R_rtt = tfrc_client.R_sample;
                    else
                        tfrc_client.R_rtt = 0.9 * tfrc_client.R_rtt + 0.1 * tfrc_client.R_sample; // averaging funtion
 
                    tfrc_client.t_RTO = fmax(4 * tfrc_client.R_rtt,2*tfrc_client.s_msgSize*8.0/tfrc_client.X_trans);

                    newsendingrate(); //calculate new sending rate
                    
                    tfrc_client.t_RTO = fmax(4 * tfrc_client.R_rtt,2*tfrc_client.s_msgSize*8.0/tfrc_client.X_trans); //  recalculate
                    
                    tfrc_client.timebetnPackets = tfrc_client.s_msgSize * 8.0 / tfrc_client.X_trans;
                    
                }
            }
            break;
        case CLIENT_STOP:
            if((receivedStrLen = recvfrom(tfrc_client.sock, cntrl.controlmessage, MSGMAX, 0,
                                          (struct sockaddr *) &(tfrc_client.ServAddr), &(tfrc_client.ServAddrLen))) != CNTRLMSGSIZE)
                    printf(" Receive Error from Server from CLIENT_STOP !!\n");

            else
            {
                // check for correctness of the received ACK.
                if(*cntrl.msgType == CONTROL && *cntrl.msgCode==OK) //  server responded
                {
                    
                    tfrc_client.feedbackRecvd = true; // to start packet transfer


                    CNTCStop=true;
                    pthread_exit(NULL);

                    break;// break the while loop


                }
            }


        }
    }


}



void setuptcpconnection()
{
    memset(&(tfrc_client.ServAddr), 0, sizeof(tfrc_client.ServAddr));    /* Zero out structure */
    tfrc_client.ServAddr.sin_family = AF_INET;                 /* Internet addr family */
    tfrc_client.ServAddr.sin_addr.s_addr = inet_addr(tfrc_client.servIP);  /* Server IP address */

    /* If user gave a dotted decimal address, we need to resolve it  */
    if (tfrc_client.ServAddr.sin_addr.s_addr == -1)
    {
        tfrc_client.thehost = gethostbyname(tfrc_client.servIP);
        tfrc_client.ServAddr.sin_addr.s_addr = *((unsigned long *) tfrc_client.thehost->h_addr_list[0]);
    }
    tfrc_client.ServAddr.sin_port   = htons(tfrc_client.ServPort);     /* Server port */

    if ((tfrc_client.sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
        DieWithError("socket() failed"); //  Die with Error not developed yet.

}




int main(int argc, char *argv[])
{
    struct sigaction initialtimer;

    pthread_t *thread1;

    // check for proper argument list

    if(argc!=7)
    {
        fprintf(stderr,"USAGE : tfrc-client destinationAddress destinationPort messageSize connectionID simulatedLossRate maxAllowedThroughput\n");
        exit(1);
    }

    tfrc_client.servIP = argv[1];
    tfrc_client.ServPort = atoi(argv[2]);
    tfrc_client.s_msgSize = atoi(argv[3]);
    tfrc_client.connectionID = atoi(argv[4]);
    tfrc_client.simulatedLossRate = atof(argv[5]);
    tfrc_client.maxAllowedThroughput = atol(argv[6]);

    /* Setup Packets and sockets */
    initializeparameters();
    setuptcpconnection();
    tfrc_client.sequencenum = makecntrlmsg(tfrc_client.s_msgSize); // returns the randomly selected seq num
    tfrc_client.expectedACK = tfrc_client.sequencenum+1;
    initializedatamsg(tfrc_client.s_msgSize,tfrc_client.sequencenum);
    setupackmsg();

    //Initialize the semaphore
    sem_init(&lock, 0, 0);
    
    // setup the client initialize timeout


    /*** Initialization Alarm timer setup *****/

    initialtimer.sa_handler = catchCntrlTimeout;
    if (sigfillset(&initialtimer.sa_mask) < 0)
        DieWithError("sigfillset() failed");

    initialtimer.sa_flags = 0;

    if (sigaction(SIGALRM, &initialtimer, 0) < 0)
        DieWithError("sigaction failed for sigalarm");

    /***Ctrl+C interrupt setup ****/
    signal (SIGINT, CNTCCatch); //  setting  up tfrc_client CNTC catch


    // thread out the receive process..

    thread1 = (pthread_t*) calloc(1,sizeof(pthread_t));

    if(pthread_create(thread1,NULL,thread_receive,NULL))
        DieWithError("receive thread creation error!");


    // parse through states starting from CLIENT_INIT.

    tfrc_client.alarmtimeout = true;
    tfrc_client.feedbackRecvd= false;
    //usec1 = 15;


    while(1)
    {
        switch(cstate)
        {
        case CLIENT_INIT:
            if(tfrc_client.alarmtimeout)
            {
                alarm(0);
                if (sendto(tfrc_client.sock, cntrl.controlmessage, CNTRLMSGSIZE, 0, (struct sockaddr *)
                           &(tfrc_client.ServAddr), sizeof(tfrc_client.ServAddr)) != CNTRLMSGSIZE)
                    DieWithError("sendto() sent a different number of bytes than expected");
                tfrc_client.alarmtimeout = false;
                alarm(tfrc_client.cntrlTimeout); //start the timeout
            }
            break;
        case CLIENT_START:


            usec2 = get_time() * MEG; // returns double in seconds so times  MEGA


            if((usec2>=tfrc_client.noFeedbackTimer) || (usec2-usec1 >= tfrc_client.timebetnPackets*MEG))
            {
                
                if(usec2-usec1>= tfrc_client.timebetnPackets*MEG)
                {	// ready to send
                
					//if(tfrc_client.expectedACK+9 == tfrc_client.sequencenum)
					//continue;
					
                    sem_wait(&lock);
                    *(data.sequenceNum) = htonl(++tfrc_client.sequencenum); // increments seqnum before attaching
                    tfrc_client.latestPktTimestamp = get_time() * MEG;
                    *(data.timeStamp) = htond(tfrc_client.latestPktTimestamp); //  time now in usec
                    *(data.senderRttEst) = htonl(tfrc_client.R_rtt); //  add senders RTT estimate
                    tfrc_client.timestore[tfrc_client.sequencenum%TIMESTAMPWINDOW] = ntohd(*(data.timeStamp));
                    
                    if ( tfrc_client.feedbackRecvd == true) {
						tfrc_client.noFeedbackTimer = get_time() *MEG + tfrc_client.t_RTO; // reset the timer
						tfrc_client.feedbackRecvd = false;
					}
				
                    if(PACKETDROP(tfrc_client.simulatedLossRate)==1)
                    {
                        if (sendto(tfrc_client.sock, data.datamessage, data.dataMsgLen, 0, (struct sockaddr *)
                           &(tfrc_client.ServAddr), sizeof(tfrc_client.ServAddr)) != data.dataMsgLen)
                            DieWithError("sendto() sent a different number of bytes than expected");
                    }
                    else 
                    { 
						tfrc_client.numDropped++;
                    }
                    
                    tfrc_client.numSent++;
                    usec1 = get_time() *MEG;
                    sem_post(&lock);
                }
                else if(usec2>=tfrc_client.noFeedbackTimer && tfrc_client.feedbackRecvd ==false) // no feed back timer interrupts
                {	
					sem_wait(&lock);
					
                    if(tfrc_client.R_rtt>0.0) // if there has been feedback beforehand
                    {
                        if(tfrc_client.X_calc>=tfrc_client.X_recv*2)
                            tfrc_client.X_recv = fmax(tfrc_client.X_recv/2,tfrc_client.s_msgSize*8.0/(2*t_mbi));
                        else
                            tfrc_client.X_recv = tfrc_client.X_calc/4;

                        newsendingrate();
                    }
                    else
                    {
                        tfrc_client.X_trans = fmax(tfrc_client.X_trans/2,tfrc_client.s_msgSize*8.0/t_mbi);
                    }
                    
                    tfrc_client.sequencenum = tfrc_client.expectedACK-1; // look for the last ack received 

                    tfrc_client.timebetnPackets = tfrc_client.s_msgSize * 8.0 / tfrc_client.X_trans;
                    
                    tfrc_client.t_RTO = fmax(4 * tfrc_client.R_rtt,2*tfrc_client.s_msgSize*8.0/tfrc_client.X_trans);
                    tfrc_client.noFeedbackTimer = get_time() * MEG + tfrc_client.t_RTO; // update the nofeedbacktimer
                    tfrc_client.feedbackRecvd = true;
                    sem_post(&lock);

                }

            }
            break;
        case CLIENT_STOP:
        
			tfrc_client.sessionTime = get_time() *MEG - tfrc_client.sessionTime;
            // send out a CLIENT_STOP packet

            usec4 = get_time()*MEG;


            if(usec4-usec3  > tfrc_client.t_RTO)// || tfrc_client.sendSTOP == false) //  repeat the stop packet
            {
                tfrc_client.feedbackRecvd =false;
                *cntrl.msgCode=STOP;
                *cntrl.msgType=CONTROL;
                *cntrl.sequenceNum = htonl(tfrc_client.sequencenum);

                if (sendto(tfrc_client.sock, cntrl.controlmessage, CNTRLMSGSIZE, 0, (struct sockaddr *)
                           &(tfrc_client.ServAddr), sizeof(tfrc_client.ServAddr)) != CNTRLMSGSIZE)
                    DieWithError("sendto() sent a different number of bytes than expected");


                usec3 = get_time() *MEG;
                

                tfrc_client.sendSTOP = true;
                
                tfrc_client.avgThroughput = tfrc_client.numSent*tfrc_client.s_msgSize*8*1000000.0/tfrc_client.sessionTime;
                tfrc_client.avgLossEvents = tfrc_client.lossEventCounter/tfrc_client.numReceived;
                
                printf("\n Session Duration = %g uSec \n Total Data Sent = %g Packets (%g Bytes)\n Total Acks Received = %g \n Total Average Throughput = %g \n Average Loss Event = %g \n Total Pkt Droppped (dropped rate) = %g (%g)\n",tfrc_client.sessionTime,tfrc_client.numSent,tfrc_client.numSent*tfrc_client.s_msgSize*8,tfrc_client.numReceived,tfrc_client.avgThroughput,tfrc_client.avgLossEvents,tfrc_client.numDropped,tfrc_client.numDropped/tfrc_client.numSent); 
                
                exit(1) ; //  hard stop 
            }
            else if(tfrc_client.feedbackRecvd)
            {


                if(CNTCStop)
                    exit(1);

                break;
            }
        }

    }
    pthread_exit(NULL);

}

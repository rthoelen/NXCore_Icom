/*

Copyright (C) 2015 Robert Thoelen

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/


#include "boost/property_tree/ptree.hpp"
#include "boost/property_tree/ini_parser.hpp"
#include "boost/algorithm/string.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>

#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>

struct rpt {
	struct sockaddr_in rpt_addr_00; // socket address for 41300
	char *hostname;
	int time_since_rx;  // in seconds
	int time_since_tx;  // in seconds
	int hold_time;  // how long to hold talkgroup
	int tx_hold_time;  // how long to hold talkgroup after tx
	int rx_activity;     // flag to show activity
	int tx_busy;
	unsigned int busy_tg;
	unsigned char local_ran; // RAN for local operations
	unsigned char ran;	
	unsigned int active_tg; // talkgroup currently active
	unsigned int last_tg;  // used for talk group hold time
	unsigned int *tg_list;   // if a talkgroup isn't in this list, it isn't repeated
	unsigned int *tac_list; // list of tactical talkgroups
	int uid; // need this for Kenwood udp 64001 data
	int tx_uid; // UID during transmit
	int stealth; // true if keep alive needed
	int tx_otaa; // flag for sending OTAA or blocking

} *repeater;

char version[] = "NXCORE Manager, ICOM, version 1.1.1";
char copyright[] = "Copyright (C) Robert Thoelen, 2015";


void snd_packet(unsigned char [], int, int,int, int);
int tg_lookup(int, int);
int tac_lookup(int, int);


int repeater_count;

int socket_00;   // Sockets we use


// See if incoming packet address matches a repeater.  If it does,
// return the index to it.  Otherwise return -1 for no match

int get_repeater_id(struct sockaddr_in *addr)
{
	int i;

	for(i = 0; i < repeater_count; i++)
	{	
		if((in_addr_t)addr->sin_addr.s_addr == (in_addr_t)repeater[i].rpt_addr_00.sin_addr.s_addr)
		{
			return i;
		}
	}

	return(-1);
}

struct sockaddr_in myaddr_00;

void *listen_thread(void *thread_id)
{
        struct sockaddr_in remaddr;     /* remote address */
        socklen_t addrlen = sizeof(remaddr);            /* length of addresses */
        int recvlen;                    /* # bytes received */
        unsigned char buf[105];     /* receive buffer */
	struct hostent *he;
	int rpt_id;
	int strt_packet;

	struct sockaddr_in tport;
	int GID, UID;
	int i;

        /* create UDP socket for repeaters */

        if ((socket_00 = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("cannot create socket\n");
                return 0;
        }


        /* bind the socket to any valid IP address and a specific port */


        memset((char *)&myaddr_00, 0, sizeof(myaddr_00));
        myaddr_00.sin_family = AF_INET;
//	myaddr_00.sin_addr.s_addr = inet_addr("10.44.0.1");
        myaddr_00.sin_addr.s_addr = htonl(INADDR_ANY);
        myaddr_00.sin_port = htons(41300);

        if (bind(socket_00, (struct sockaddr *)&myaddr_00, sizeof(myaddr_00)) < 0) {
                perror("bind failed");
                return 0;
        }

	// Add a debug port
	memset((char *)&tport, 0, sizeof(tport));
	tport.sin_family = AF_INET;
	tport.sin_addr.s_addr = inet_addr("127.0.0.1");
	tport.sin_port = htons(50001);

        /* now loop, receiving data and printing what we received */
        for (;;) {

		recvlen = recvfrom(socket_00, buf, 103, 0, (struct sockaddr *)&remaddr, &addrlen);	

		// Look for I in Icom 
		if (buf[0] != 0x49)
			continue;


		// Linkup to connect Icom repeater
		if ((buf[4] == 0x01) && (buf[5] == 0x61))
		{
			buf[5]++;
			buf[37] = 0x02;
			buf[38] = 0x4f;
			buf[39] = 0x4b;
			std::cout << "Sending connect string to Repeater at IP:" << inet_ntoa(remaddr.sin_addr) << std::endl;
			
        		remaddr.sin_port = htons(41300);
			sendto(socket_00, buf, recvlen, 0, (struct sockaddr *)&remaddr,
		 		sizeof(remaddr));
			continue;
		}

	
		// Don't know what these are, but probably don't matter
		// if ((buf[38] == 0x00) && (buf[39] == 0x21))
	//		continue;
	
		if (recvlen != 102)
			continue;

                if ((buf[38] == 0x1c) && (buf[39] == 0x21)) {
                        buf[recvlen] = 0;

			rpt_id = get_repeater_id(&remaddr);
			if (rpt_id == -1)
			{
				std::cout << "Unauthorized repeater, " << inet_ntoa(remaddr.sin_addr) << ", dropping packet" << std::endl;
				continue;  // Throw out packet, not in our list
			}


			if(buf[45] == (char)1) // Beginning of packets
			{
				repeater[rpt_id].rx_activity = 1;
				GID = (buf[50] << 8) + buf[51];
				UID = (buf[48] << 8) + buf[49];
				repeater[rpt_id].uid = UID;
				repeater[rpt_id].active_tg = GID;
				repeater[rpt_id].busy_tg = GID;
				strt_packet=1;
				std::cout << "Repeater " << rpt_id << " receiving start from UID: " << UID << " from TG: " << GID << std::endl;
			}
		
			if(buf[45] == (char)8) // End, sent shutdown on 64001	
			{
				repeater[rpt_id].rx_activity = 0;    // Activity on channel is over
				repeater[rpt_id].last_tg = repeater[rpt_id].active_tg;
				GID = repeater[rpt_id].active_tg;
				std::cout << "Repeater " << rpt_id << " receiving stop from UID: " << UID << " from TG: " << GID << std::endl;	
			}	
				
			repeater[rpt_id].time_since_rx = 0;
			// send packet to repeaters
			snd_packet(buf, recvlen, GID, rpt_id, strt_packet);
			strt_packet = 0;
			sendto(socket_00, buf, recvlen, 0, (struct sockaddr *)&tport,
		 		sizeof(tport));
				
		}
               else
		if (((buf[38] == 0x00)||(buf[38] == 0x10)) && (buf[39] == 0x21)) { 

			rpt_id = get_repeater_id(&remaddr);
			if (rpt_id == -1)
				continue;  // Throw out packet, not in our list
			
			if (repeater[rpt_id].rx_activity == 0)
				continue;

			GID = repeater[rpt_id].active_tg;

			repeater[rpt_id].time_since_rx = 0;

			// send packet to repeaters that can receive it
			snd_packet(buf, recvlen, GID, rpt_id, 0);

			sendto(socket_00, buf, recvlen, 0, (struct sockaddr *)&tport,
		 		sizeof(tport));
		}
		
        }	
}

int tg_lookup(int GID, int i)
{
	int j;

	j = 0;
	while (repeater[i].tg_list[j] != 0)
	{
		if (repeater[i].tg_list[j++] == GID)
			return(j-1);
	}
	return(-1);
}

int tac_lookup(int GID, int i)
{
        int j;

        j = 0;

        while (repeater[i].tac_list[j] != 0)
        {
                if (repeater[i].tac_list[j++] == GID)
                        return(j-1);
        }
        return(-1);
}

	

void snd_packet(unsigned char buf[], int recvlen, int GID, int rpt_id, int strt_packet)
{
	int i, j;
	int tg;
	int tac_flag;
	in_addr_t tmp_addr;

	if (tg_lookup(GID, rpt_id) == -1)
	{
		std::cout << "Blocking TG: " << GID << " sent on Repeater " << rpt_id << ", unauthorized TG: " << GID << std::endl;
		return;
	}

	for(i = 0; i < repeater_count; i++)
	{

		if (rpt_id == i)
			continue;

		tg = tg_lookup(GID, i);
		tac_flag = tac_lookup(GID, i);

	 	if(tg != -1)
		{


			// Check if this is an OTAA packet

                	if ((buf[38] == 0x1c) && (buf[39] == 0x21) && (buf[40] == 0xa0) && (repeater[i].tx_otaa == 0)) {
				continue;
			}

                        // Process TAC groups first
                        // The only way to let the TAC group(s) through is if the RX active group matches


                        if(tac_flag != -1)
                        {
                                if((repeater[i].time_since_rx == repeater[i].hold_time) || (repeater[i].active_tg != GID))
                                        continue;
                        }

			// If there is RX activity on this repeater, don't send to the repeater

			if(repeater[i].rx_activity == 1)
				continue;


			// First, if this particular repeater just had RX activity, if the packet 
			// doesn't match the last talkgroup, drop it.  This should solve most contention
			// issues

	                if((repeater[i].last_tg != GID) && (tac_flag == -1))
                        {
                                if(repeater[i].time_since_rx < repeater[i].hold_time)
                                {
                                        std::cout << "Blocking TG: " << GID << " sent on Repeater " << i << " due to recent RX on TG: " << repeater[i].last_tg << std::endl;
                                        continue;
                                }
                        }

                        // Next, we need to determine if we need to preempt a talkgroup
                        // Talkgroups that are on the left in the NXCore.ini list get higher priority

                        if((tg_lookup(GID, i) < tg_lookup(repeater[i].busy_tg, i)) && (strt_packet ==1) &&
				 (repeater[i].tx_busy ==1) && (tac_flag == -1))
                        {
                                repeater[i].busy_tg = GID;
				std::cout << "Overriding TG: " << repeater[i].busy_tg << "with TG: " << GID << " on Repeater " << i << std::endl;
                        }


                        // Next, if repeater is considered busy, only send the talkgroup it has been assigned

                        if((repeater[i].tx_busy == 1) && (repeater[i].busy_tg!=GID) && (tac_flag == -1))
                        {
                                std::cout << " Repeater " << i << " not geting " << GID << "due to active TX on " << repeater[i].busy_tg << std::endl;
                                continue;
                        }


			// Drop in the ran code
			if ((buf[38] == 0x1c) && (buf[40] == 0x81))
			{
				buf[41] = (char)repeater[i].ran;
			}
			
			if (strt_packet)
			{
				repeater[i].tx_busy = 1;
				repeater[i].busy_tg = GID;
			}

			repeater[i].time_since_tx = 0;

			std::cout << "Sending size " << recvlen << " packet to Repeater " << i << " with TG: " << GID << std::endl;

			sendto(socket_00, buf, recvlen, 0, (struct sockaddr *)&repeater[i].rpt_addr_00,
		 		sizeof(repeater[i].rpt_addr_00));

		}
	}
}

void *timing_thread(void *t_id)
{
	int i;
	unsigned int seconds = 0;
	char h_buf[20];
	
	
        memset((char *)&h_buf[0], 0, sizeof(h_buf));
	h_buf[0] = 'N';
	h_buf[1] = 'X';
	h_buf[2] = 'D';
	h_buf[3] = 'N';
	h_buf[4] = 0x00;
	h_buf[5] = 0x99;
	h_buf[6] = 0x00;
	h_buf[7] = 'N';
	h_buf[8] = '1';
	h_buf[9] = 'X';
	h_buf[10] = 'D';
	h_buf[11] = 'N';

	for(;;)
	{
		for( i = 0; i < repeater_count; i++)
		{
			repeater[i].time_since_rx++;
			repeater[i].time_since_tx++;

			if(repeater[i].time_since_rx > repeater[i].hold_time)
			{
				repeater[i].time_since_rx = repeater[i].hold_time;
				repeater[i].rx_activity = 0;
			}

			if(repeater[i].time_since_tx > repeater[i].tx_hold_time)
			{
				repeater[i].time_since_tx = repeater[i].tx_hold_time;
				repeater[i].tx_busy = 0;
			}


		}	
		if((++seconds % 30) == 0)
		{
			for( i = 0; i < repeater_count; i++)
				{
					if(repeater[i].stealth)
						sendto(socket_00, h_buf, sizeof(h_buf), 0, (struct sockaddr *)&repeater[i].rpt_addr_00,
							sizeof(repeater[i].rpt_addr_00));
				}
		}


		sleep(1);
	}

	
}

void write_map(void)
{
        int i;

        std::ofstream out("/usr/local/www/nginx/status-ic.json");

        // Write the preamble

        out << " var json1 = [ " << std::endl;

        for( i = 0; i < repeater_count; i++)
        {
                out << " { " << std::endl;
                // Determine what state we are in:

                // Green: IDLE
                // Red: TX
                // Blue: RX

                if (repeater[i].rx_activity == 1)
                {
                        out << "\"icon\" : \"http://maps.google.com/mapfiles/ms/icons/blue-dot.png\"," << std::endl;

                        out << "\"contentstr\" : \"<p>RX TG: <b>" << repeater[i].active_tg << "</b><br/>RX Timer: <b>"
                                 << repeater[i].time_since_rx << "<b/><br/>UID: <b>" << repeater[i].uid << "</b></p>\" " << std::endl;

			continue;
                }

                if ((repeater[i].rx_activity == 0)&&(repeater[i].tx_busy == 0))
                {
                        out << "\"icon\" : \"http://maps.google.com/mapfiles/ms/icons/green-dot.png\"," << std::endl;

                        out << "\"contentstr\" : \"<p>Repeater IDLE</p>\"  " << std::endl;
			continue;	
                }

                if (repeater[i].tx_busy == 1)
                {

                        out << "\"icon\" : \"http://maps.google.com/mapfiles/ms/icons/red-dot.png\"," << std::endl;

                        out << "\"contentstr\" : \"<p>TX TG: <b>" << repeater[i].busy_tg << "</b><br/>TX Timer: <b>"
                                 << repeater[i].time_since_tx << "<br/><b/>UID: <b>" << repeater[i].tx_uid << "</b></p>\" " << std::endl;
			continue;

                }

                out << " }," << std::endl;
        }
        out << " ];" << std::endl;

        out.close();


}


int main(int argc, char *argv[])
{
    int i, j, len;

	struct addrinfo hints, *result;
	boost::property_tree::ptree pt;

	try {

	boost::property_tree::ini_parser::read_ini("NXCore.ini", pt);

	}
	catch(const boost::property_tree::ptree_error  &e)
	{
		std::cout << "Config file not found!" << std::endl << std::endl;
		exit(1);
	}


	// Get list of repeaters

	std::string s = pt.get<std::string>("repeaters");
	std::string tg;

	std::vector<std::string> elems;
	std::vector<std::string> tg_elems;

	std::cout << version << std::endl;
	std::cout << copyright << std::endl << std::endl;
	
	// Split by space or tab
	boost::split(elems, s, boost::is_any_of("\t "), boost::token_compress_on);

	repeater_count = elems.size();
	std::cout << "Repeater Count:  " << repeater_count << std::endl << std::endl;

	repeater = (struct rpt *)calloc(repeater_count, sizeof(struct rpt));

	/* For Icom we don't need this

	unsigned int tempaddr;

	tempaddr = inet_addr(pt.get<std::string>("nodeip").c_str());

	*/

	// Start populating structure
	std::string key;

	for(i = 0; i < elems.size(); i++)
	{

		key.assign(elems[i]);
		key.append(".address");
	
	        memset(&hints,0, sizeof(hints));
                hints.ai_family = AF_INET;

		len = pt.get<std::string>(key).size();
		repeater[i].hostname = (char *)calloc(1,len+1); 
		memcpy(repeater[i].hostname,(char *)pt.get<std::string>(key).c_str(),len+1);

                if(getaddrinfo(repeater[i].hostname, NULL, &hints, &result) == -1)
                {
                        std::cout << "Error resolving " << pt.get<std::string>(key) << ", exiting" << std::endl;
                        exit(1);
                }

                repeater[i].rpt_addr_00.sin_addr.s_addr = ((struct sockaddr_in *)(result->ai_addr))->sin_addr.s_addr;
		repeater[i].rpt_addr_00.sin_family = AF_INET;
		repeater[i].rpt_addr_00.sin_port = htons(41300);
		std::cout << "Repeater " << i << " address: " << inet_ntoa(repeater[i].rpt_addr_00.sin_addr) << std::endl << std::endl;

		// Parse out the talkgroups

		key.assign(elems[i]);
		key.append(".tg_list");
		tg = pt.get<std::string>(key).c_str();

		boost::split(tg_elems, tg, boost::is_any_of("\t "), boost::token_compress_on);
	
		repeater[i].tg_list = (unsigned int *)calloc(tg_elems.size()+1,sizeof(int));
		std::cout << "Talkgroups " << tg_elems.size() << std::endl; 
		std::cout << "Repeater " << i << "  Talkgroups: ";

		for(j = 0; j < tg_elems.size(); j++)
		{
			repeater[i].tg_list[j] = atoi(tg_elems[j].c_str());
			std::cout << " " << repeater[i].tg_list[j];

		}

	        // Do the same for the tactical list

                key.assign(elems[i]);
                key.append(".tac_list");
                tg = pt.get<std::string>(key).c_str();

                boost::split(tg_elems, tg, boost::is_any_of("\t ,"), boost::token_compress_on);

                repeater[i].tac_list = (unsigned int *)calloc(tg_elems.size()+1,sizeof(int));
                std::cout << "Tactical Talkgroups " << tg_elems.size() << std::endl;
                std::cout << "Repeater " << i << "  Tactical Talkgroup List: ";

                for(j = 0; j < tg_elems.size(); j++)
                {
                        repeater[i].tac_list[j] = atoi(tg_elems[j].c_str());
                        std::cout << " " << repeater[i].tac_list[j];

                }



		std::cout << std::endl << std::endl;
		repeater[i].ran = pt.get<int>(elems[i] + ".ran");
		repeater[i].hold_time = pt.get<int>(elems[i] + ".rx_hold_time");
		repeater[i].time_since_rx = repeater[i].hold_time;
		repeater[i].tx_hold_time = pt.get<int>(elems[i] + ".tx_hold_time");
		repeater[i].time_since_tx = repeater[i].tx_hold_time;
		repeater[i].stealth = pt.get<int>(elems[i] + ".stealth");
		repeater[i].tx_otaa = pt.get<int>(elems[i] + ".tx_otaa");

	}


	// start the thread

	pthread_t l_thread;
	pthread_t t_thread;

	if(pthread_create(&l_thread, NULL, listen_thread, (void *)0))  {
		fprintf(stderr, "Problem creating thread.  Exiting\n");
		return 1; 
	}
	
	if(pthread_create(&t_thread, NULL, timing_thread, (void *)0))  {
		fprintf(stderr, "Problem creating thread.  Exiting\n");
		return 1; 
	}

	int counter = 0;

	while(1==1)
	{
		sleep(10);
                counter++;

                // Write out the map json data

                write_map();

                if (counter > 90)
                {
                        for (i = 0; i < repeater_count; i++)
                        {
                                if(getaddrinfo(repeater[i].hostname, NULL, &hints, &result) == 0)
                                {
                                        repeater[i].rpt_addr_00.sin_addr.s_addr = ((struct sockaddr_in *)(result->ai_addr))->sin_addr.s_addr;
                                }
                        }
                counter = 0;
                }

	}
	pthread_exit(NULL);
	free(repeater);
}

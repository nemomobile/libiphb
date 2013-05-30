/**
   @brief Test utility #2 for IP Heartbeat service

   @file hbtest2.c

   This is the test utility for IP Heartbeat service.

   It tries to emulate fixed-sync applications.

  
   <p>
   Copyright (C) 2008-2011 Nokia Corporation.

   @author Raimo Vuonnala <raimo.vuonnala@nokia.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/



#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>
/* socket transport */
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>

#include "../src/libiphb.h"

static volatile int run = 1;

#define ME "hbtest2: "


static int debugmode = 0;

static void 
sig_handler(int signo)
{
  switch (signo) {
  case SIGQUIT:
  case SIGTERM:
  case SIGINT:
    run = 0;
    break;
  default:
    fprintf(stderr, ME "\aERROR, unknown signal %d\n", signo);
  }
}



int 
main (int argc, char *argv[])
{
  iphb_t 	      hb;
  int    	      hbsock = -1;
  int                 period;


  if (argc < 2) {
    printf("Usage: %s period_in_secs [-d]\n", argv[0]);
    exit(1);
  }

  period = atoi(argv[1]);

  if (argc >= 3 && strcmp(argv[2], "-d") == 0)
    debugmode = 1;
  

  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);
  signal(SIGPIPE, SIG_IGN);


  printf(ME "running\n");
  hb = iphb_open(0);
  if (!hb) {
	perror(ME "\aERROR, iphb_open()");
        run = 0;
  }
  else {
	struct iphb_stats stats;

	printf(ME "iphb service opened\n");

	if (iphb_get_stats(hb, &stats) == -1) 
	  fprintf(stderr, ME "\aERROR, iphb_get_stats() failed %s\n", strerror(errno));
	else
	  printf(ME "iphb_get_stats(): clients=%u, waiting=%u, next hb=%u secs\n", stats.clients, stats.waiting, stats.next_hb);

  }

  if (run) {
      hbsock = iphb_get_fd(hb);
      if (hbsock == -1) {
          perror(ME "\aERROR, iphb_get_fd()");
          run = 0;
      }
  }

  for (;run;) {
    time_t            	now;
    time_t              went_to_sleep;
    struct timeval    	timeout;
    fd_set 	      	readfds;
    int                 st;

    went_to_sleep = time(0);

    if (iphb_wait(hb, period, period, 0) != 0) {
	perror(ME "\aERROR, iphb_wait()");
        run = 0;
        continue;
    }

    printf(ME "waiting for iphbd wakeup...\n");

    timeout.tv_sec = period + 2;
    timeout.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(hbsock, &readfds);

    st = select(hbsock + 1, &readfds, NULL, NULL, &timeout);

    now = time(0);
    if (st == -1) {
      if (errno == EINTR)
	continue;  
      else {
	perror(ME "\aERROR, select()");
	run = 0;
      }
    }
    else
    if (st >= 0) {
      if (now - went_to_sleep > period + 1)  /* allow 1 sec slippage */
	fprintf(stderr, ME "\aERROR, select() did not fire as expected, took %d secs\n", 
		(int)(now - went_to_sleep));

      if (debugmode) printf(ME "slept %d secs\n", (int)(now - went_to_sleep));

      if (FD_ISSET(hbsock, &readfds)) {
	printf(ME "select() woken by iphbd, waited %d secs\n", 
               (int)(now - went_to_sleep));
      }


      if (st == 0) {
          fprintf(stderr, ME "\aERROR, select() did not fire at all!\n");
      }

      printf(ME "woke up, last heartbeat happened %d secs ago, now is %s", 
             (int)(now - went_to_sleep),
             ctime(&now));
    }
  }  
  printf(ME "bye\n");

  if (hb)
    iphb_close(hb);

  return 0;
}


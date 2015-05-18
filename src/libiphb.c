/**
   @brief Implementation of libiphb

   @file libiphb.c

   Implementation of libiphb (see libiphb.h)

   <p>
   Copyright (C) 2008-2011 Nokia Corporation.

   @author Raimo Vuonnala <raimo.vuonnala@nokia.com>
   @author Semi Malinen <semi.malinen@nokia.com>

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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
/* socket transport */
#include <sys/socket.h>
#include <sys/un.h>
/* --- */

#include "libiphb.h"
#include "iphb_internal.h"


/**@brief  Allocated structure for handle to iphbd */
struct _iphb_t {
  int fd;  		/*!< Unix domain socket handle */
};

#define HB_INST(x) ((struct _iphb_t *) (x))



static int
suck_data(int fd)
{
  int bytes = -1;
  int st;


  /* suck away unread messages */

  st = ioctl(fd, FIONREAD, &bytes);

  if (st != -1 && bytes >= 0) {
      if (bytes) {
          char *b = malloc(bytes);
          if (!b) {
              errno = ENOMEM;
              return -1;
          }
          (void)recv(fd, b, bytes, MSG_WAITALL);
          free(b);
      }
      return bytes;
    
  }
  else
      return -1;
}


int
iphb_I_woke_up(iphb_t iphbh)
{
  int st;
  struct _iphb_req_t  req = { .cmd = IPHB_WAIT, };

  if (!iphbh) {
    errno = EINVAL;
    return -1;
  }

  st = suck_data(HB_INST(iphbh)->fd);

  req.u.wait.pid = getpid();
  req.u.wait.mintime = 0;
  req.u.wait.maxtime = 0;

  if (send(HB_INST(iphbh)->fd, &req, sizeof(req), MSG_DONTWAIT|MSG_NOSIGNAL) == -1)
    return -1;    

  return st;

}




iphb_t 
iphb_open(int *dummy)
{
  int 		     	fd;
  struct sockaddr_un 	addr;
  iphb_t	       *iphbh;
  
  iphbh = (iphb_t) malloc(sizeof(struct _iphb_t));
  if (iphbh == NULL) {
    errno = ENOMEM;
    return 0;
  }
  fd = socket(PF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    free(iphbh);
    return 0;
  }
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, HB_SOCKET_PATH);
  if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
    close(fd);
    free(iphbh);
    return 0;
  }
  else {
    HB_INST(iphbh)->fd = fd;
    if (iphb_I_woke_up(iphbh)) {
      close(fd);
      free(iphbh);
      return 0;
    }
    else {
      if (dummy)
        *dummy = 30; // for compatibility
      return iphbh;
    }
  }
}


int 
iphb_get_fd(iphb_t iphbh)
{
  if (iphbh)
    return HB_INST(iphbh)->fd;
  else {
    errno = EINVAL;
    return -1;
  }
}




time_t
iphb_wait2(iphb_t iphbh, unsigned mintime, unsigned maxtime, int must_wait, int resume)
{
  /* Assume failure */
  time_t result = -1;

  /* Sanity check arguments */
  if( !iphbh || mintime > maxtime ) {
    errno = EINVAL;
    goto EXIT;
  }

  /* Clear any pending wakeups we might have available */
  (void)suck_data(HB_INST(iphbh)->fd);

  struct _iphb_req_t  req = { .cmd = IPHB_WAIT, };

  /* There are apps that contain out of date libiphb versions built
   * in to the application binaries and we need to at least attempt
   * not to break handling of iphb requests that used to be ok.
   *
   * Originally the version field did not exist, but the area now
   * occupied by it was initialized to zero. By setting it now to
   * a non-zero value, we can signal the server side that additional
   * fields are in use.
   *
   * Version 1 adds: mintime_hi, maxtime_hi and wakeup fields
   */
  req.u.wait.version    = 1;

  /* Originally mintime and maxtime were 16 bits wide. As we must
   * keep the structure layout compatible with it, the extension
   * to 32bit range is done by having upper halfs stored separately.
   * The Server side ignores upper parts unless version >= 1.  */
  req.u.wait.mintime    = (mintime >>  0) & 0xffff;
  req.u.wait.mintime_hi = (mintime >> 16) & 0xffff;
  req.u.wait.maxtime    = (maxtime >>  0) & 0xffff;
  req.u.wait.maxtime_hi = (maxtime >> 16) & 0xffff;

  /* Client process id */
  req.u.wait.pid        = getpid();

  /* The server side ignores this unless version >= 1 */
  req.u.wait.wakeup     = (resume != 0);

  int rc = send(HB_INST(iphbh)->fd, &req, sizeof req,
                MSG_DONTWAIT|MSG_NOSIGNAL);

  if( rc == -1 ) {
    /* Use errno from send() */
    goto EXIT;
  }

  if( !must_wait ) {
    /* Request succesfully sent */
    result = 0;
    goto EXIT;
  }

  /* Get current time at start of wait */
  struct timespec ts = { 0, 0 };
  clock_gettime(CLOCK_MONOTONIC, &ts);

  time_t t_beg = ts.tv_sec;

  for( ;; ) {
    fd_set         readfds;
    struct timeval timeout;

    time_t t_now = ts.tv_sec;

    /* We know time_t is signed. Assume it is integer and
     * cpu uses two's complement arithmetics, i.e.
     *
     * new_timestamp - old_timestamp of signed N-bit values
     * gives valid unsigned delta of N bits.
     *
     * Further assume that an unsigned int can hold at least
     * 32 bits of that delta, which should keep us from
     * hitting numerical overflows as long as wait times
     * stay under 68 years or so.
     */
    unsigned waited = (unsigned)(t_now - t_beg);

    if( waited >= maxtime ) {
      /* Make it look like we got wakeup from server
       * after the specified wait time is up.
       *
       * Note: The server side wakeups are likely to be sent
       *       anyway - which might cause extra wakeups if an
       *       input watch / similar is used for this socket.
       */
      result = (time_t)waited;
      goto EXIT;
    }

    /* Assume time_t can hold INT_MAX and cap individual
     * wait-for-input timeouts to that */
    unsigned waittime = maxtime - waited;

    if( waittime < INT_MAX )
      timeout.tv_sec  = (time_t)waittime;
    else
      timeout.tv_sec  = (time_t)INT_MAX;
    timeout.tv_usec = 0;

    FD_ZERO(&readfds);
    FD_SET(HB_INST(iphbh)->fd, &readfds);

    rc = select(HB_INST(iphbh)->fd + 1, &readfds, NULL, NULL, &timeout);

    if( rc > 0 ) {
      struct _iphb_wait_resp_t resp = { .waited = 0, };

      /* Got input, result is: wakeup or error */
      rc = recv(HB_INST(iphbh)->fd, &resp, sizeof resp, MSG_WAITALL);

      if( rc == (ssize_t)sizeof resp ) {
        /* Wakeup succesfully read */
        result = resp.waited;
      }
      else if( rc >= 0 ) {
        /* Unexpected EOF / partial read */
        errno = EIO;
      }
      else {
        /* Use errno from recv() */
      }

      goto EXIT;
    }

    if( rc == -1 && errno != EINTR ) {
      /* Use errno from select() */
      goto EXIT;
    }

    /* Update current time and try again */
    clock_gettime(CLOCK_MONOTONIC, &ts);
  }

EXIT:
  return result;
}

time_t
iphb_wait(iphb_t iphbh, unsigned short mintime, unsigned short maxtime, int must_wait)
{
  return iphb_wait2(iphbh, mintime, maxtime, must_wait, 1);
}


int
iphb_discard_wakeups(iphb_t iphbh)
{
  if (!iphbh) {
    errno = EINVAL;
    return (time_t)-1;
  }

  return suck_data(HB_INST(iphbh)->fd);
}



int iphb_get_stats(iphb_t iphbh, struct iphb_stats *stats)
{
  struct _iphb_req_t  req = { .cmd = IPHB_STAT, };
  int bytes = -1;

  if (!iphbh) {
    errno = EINVAL;
    return -1;
  }

  /* suck away unread messages */
  if (ioctl(HB_INST(iphbh)->fd, FIONREAD, &bytes) != -1 && bytes > 0) {
    char *b = malloc(bytes);
    if (!b) {
      errno = ENOMEM;
      return (time_t)-1;
    }
    (void)recv(HB_INST(iphbh)->fd, b, bytes, MSG_WAITALL);
    free(b);
    
  }

  if (send(HB_INST(iphbh)->fd, &req, sizeof(req), MSG_DONTWAIT|MSG_NOSIGNAL) <= 0)
    return -1;    

  if (recv(HB_INST(iphbh)->fd, stats, sizeof(*stats), MSG_WAITALL) > 0)
    return 0;
  else
    return (time_t)-1;
}



iphb_t 
iphb_close(iphb_t iphbh)
{
  if (iphbh) {
    close(HB_INST(iphbh)->fd);
    HB_INST(iphbh)->fd = 0;
    free(iphbh);
  }
  return 0;
}


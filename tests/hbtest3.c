/* ------------------------------------------------------------------------- *
 * Copyright (C) 2013 Jolla Ltd.
 * Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
 * License: LGPLv2
 * ------------------------------------------------------------------------- */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <getopt.h>

#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <mce/dbus-names.h>

#include "../src/libiphb.h"

#include <sys/time.h>

/* How strictly we want to interpret the maximum wakup time */
#define ALLOWED_DELAY 999 // ms

/* Should we use monotonic time source or system time for timing */
#define USE_MONOTONIC_TIME 0

typedef struct hbtimer_t hbtimer_t;

/* -- tv -- */
static void tv_get_monotime(struct timeval *tv);
static int  tv_diff_in_ms  (const struct timeval *tv1, const struct timeval *tv2);

/* -- log -- */
static void log_emit(const char *fmt, ...);

/* -- mainloop -- */
static void mainloop_stop(int exit_code);
static int  mainloop_run (void);

/* -- systembus -- */
static bool systembus_connect   (void);
static void systembus_disconnect(void);

/* -- xmce -- */
static int  xmce_cpu_keepalive_period(void);
static bool xmce_cpu_keepalive_start (void);
static bool xmce_cpu_keepalive_stop  (void);

/* -- hbtimer -- */
static void       hbtimer_show_stats             (const hbtimer_t *self);

static gboolean   hbtimer_handle_wakeup_renew_cb (gpointer aptr);
static gboolean   hbtimer_handle_wakeup_finish_cb(gpointer aptr);

static void       hbtimer_cancel_timers          (hbtimer_t *self);
static void       hbtimer_start_timers           (hbtimer_t *self);

static void       hbtimer_set_start              (hbtimer_t *self, void (*cb) (hbtimer_t*));
static void       hbtimer_set_finish             (hbtimer_t *self, void (*cb) (hbtimer_t*), int secs);
static void       hbtimer_set_renew              (hbtimer_t *self, void (*cb) (hbtimer_t*), int secs);

static bool       hbtimer_deactivate             (hbtimer_t *self);
static void       hbtimer_activate               (hbtimer_t *self);

static void       hbtimer_handle_wakeup_finish   (hbtimer_t *self);
static void       hbtimer_handle_wakeup_start    (hbtimer_t *self);

static gboolean   hbtimer_wakeup_cb              (GIOChannel *chan, GIOCondition condition, gpointer data);
static void       hbtimer_disconnect             (hbtimer_t *self);
static bool       hbtimer_connect                (hbtimer_t *self);

static bool       hbtimer_start                  (hbtimer_t *self);

static void       hbtimer_ctor                   (hbtimer_t *self);
static void       hbtimer_dtor                   (hbtimer_t *self);

static void       hbtimer_setup                  (hbtimer_t *self, int mintime, int maxtime, int repeats);
static hbtimer_t *hbtimer_create                 (int mintime, int maxtime, int repeats);
static void       hbtimer_delete                 (hbtimer_t *self);

static bool       hbtimer_woke_up_at             (const hbtimer_t *self, const struct timeval *tv);
static bool       hbtimer_is_aligned_with        (const hbtimer_t *self, const hbtimer_t *that);
static int        hbtimer_common_wakeups         (const hbtimer_t *self, const hbtimer_t *that);

/* -- req -- */
static void req_simultaneous(int *xc, const hbtimer_t *a, const hbtimer_t *b);
static void req_in_range    (int *xc, const hbtimer_t *a);
static void req_aligned     (int *xc, const hbtimer_t *a, const hbtimer_t *b);
static void req_common      (int *xc, const hbtimer_t *a, const hbtimer_t *b, int n);
static void req_period      (int *xc, const hbtimer_t *a, int n);
static void req_wakeups     (int *xc, const hbtimer_t *a);
static void req_finishes    (int *xc, const hbtimer_t *a, int n);

/* -- failure -- */
static gboolean failure_cb(gpointer user_data);

/* -- slots -- */
static void slots_test(int *xc);

/* -- range -- */
static void ranges_test(int *xc);

/* -- keepalive -- */
static void keepalive_start_cb(hbtimer_t *self);
static void keepalive_renew_cb(hbtimer_t *self);
static void keepalive_stop_cb (hbtimer_t *self);
static void keepalive_test    (int *xc);

/* ------------------------------------------------------------------------- *
 * struct timeval helpers
 * ------------------------------------------------------------------------- */

static void tv_get_monotime(struct timeval *tv)
{
#if USE_MONOTONIC_TIME
  /* CLOCK_MONOTONIC might not advance while device is suspended
   * which makes it not ideal for timing waking up from suspend ... */
  struct timespec ts;

  if( clock_gettime(CLOCK_MONOTONIC, &ts) < 0 )
  {
    abort();
  }

  TIMESPEC_TO_TIMEVAL(tv, &ts);
#else
  /* The system changes will cause tests to fail */
  gettimeofday(tv, 0);
#endif
}

static int tv_diff_in_ms(const struct timeval *tv1, const struct timeval *tv2)
{
    struct timeval tv;
    timersub(tv1, tv2, &tv);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------------- *
 * logging
 * ------------------------------------------------------------------------- */

static void log_emit(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));

static void log_emit(const char *fmt, ...)
{
  static struct timeval tv0 = {0,0};
  va_list va;
  struct timeval tv;

  tv_get_monotime(&tv);
  if( !timerisset(&tv0) )
  {
    tv0 = tv;
  }
  timersub(&tv,&tv0,&tv);

  fprintf(stderr, "<%03ld.%03ld> ", (long)tv.tv_sec, (long)(tv.tv_usec/1000));

  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

#define log_error(  FMT,ARGS...) log_emit("E: "FMT"\n", ## ARGS)
#define log_warning(FMT,ARGS...) log_emit("W: "FMT"\n", ## ARGS)

/* for test results */
#define log_notice( FMT,ARGS...) log_emit("N: "FMT"\n", ## ARGS)

/* for progress reporting */
#define log_info(   FMT,ARGS...) log_emit("I: "FMT"\n", ## ARGS)

/* normally we do not want to see these */
#if 0
# define log_debug( FMT,ARGS...) log_emit("D: "FMT"\n", ## ARGS)
#else
# define log_debug( FMT,ARGS...) do {} while(0)
#endif

/* ------------------------------------------------------------------------- *
 * mainloop
 * ------------------------------------------------------------------------- */

static GMainLoop *mainloop_handle = 0;
static int        mainloop_status = EXIT_SUCCESS;

static void mainloop_stop(int exit_code)
{
  log_info("@ %s(%d)", __FUNCTION__, exit_code);

  if( mainloop_status < exit_code )
  {
    mainloop_status = exit_code;
  }

  if( !mainloop_handle )
  {
    exit(mainloop_status);
  }

  g_main_loop_quit(mainloop_handle);
}

static int mainloop_run(void)
{
  log_info("@ %s()", __FUNCTION__);

  if( (mainloop_handle = g_main_loop_new(0, 0)) )
  {
    g_main_loop_run(mainloop_handle);
    g_main_loop_unref(mainloop_handle), mainloop_handle = 0;
  }

  log_info("@ %s() -> %d", __FUNCTION__, mainloop_status);

  return mainloop_status;
}

/* ------------------------------------------------------------------------- *
 * systembus
 * ------------------------------------------------------------------------- */

static DBusConnection *systembus = 0;

static bool systembus_connect(void)
{
  log_debug("@ %s()", __FUNCTION__);

  DBusError err = DBUS_ERROR_INIT;

  if( !(systembus = dbus_bus_get(DBUS_BUS_SYSTEM, &err)) )
  {
    log_error("can't connect to systembus: %s: %s",
              err.name, err.message);
    goto cleanup;
  }

  dbus_connection_setup_with_g_main(systembus, 0);

cleanup:

  dbus_error_free(&err);

  return systembus != 0;
}

static void systembus_disconnect(void)
{
  if( systembus ) {
    log_debug("@ %s()", __FUNCTION__);
    dbus_connection_unref(systembus), systembus = 0;
  }
}

/* ------------------------------------------------------------------------- *
 * ipc with mce
 * ------------------------------------------------------------------------- */

static int xmce_cpu_keepalive_period(void)
{
  log_debug("@ %s()", __FUNCTION__);

  int          res = -1;
  DBusMessage *req = 0;
  DBusMessage *rsp = 0;
  DBusError    err = DBUS_ERROR_INIT;
  dbus_int32_t dta = -1;

  if( !systembus )  goto cleanup;

  req = dbus_message_new_method_call(MCE_SERVICE,
                                     MCE_REQUEST_PATH,
                                     MCE_REQUEST_IF,
                                     MCE_CPU_KEEPALIVE_PERIOD_REQ);
  if( !req ) goto cleanup;

  dbus_message_set_auto_start(req, false);

  rsp = dbus_connection_send_with_reply_and_block(systembus, req, -1, &err);

  if( !rsp )
  {
    log_error("failed to call %s.%s", MCE_REQUEST_IF, MCE_CPU_KEEPALIVE_PERIOD_REQ);
    log_error("%s: %s", err.name, err.message);
    goto cleanup;
  }

  if( dbus_set_error_from_message(&err, rsp) ||
      !dbus_message_get_args(rsp, &err,
                             DBUS_TYPE_INT32, &dta,
                             DBUS_TYPE_INVALID) ) {

    log_error("error reply to %s.%s", MCE_REQUEST_IF, MCE_CPU_KEEPALIVE_PERIOD_REQ);
    log_error("%s: %s", err.name, err.message);
    goto cleanup;
  }

  res = dta;

cleanup:

  dbus_error_free(&err);
  if( rsp ) dbus_message_unref(rsp);
  if( req ) dbus_message_unref(req);

  return res;
}

static bool xmce_method_call(const char *method)
{
  log_debug("@ %s(%s)", __FUNCTION__, method);

  bool         res = false;
  DBusMessage *req = 0;

  if( !systembus )  goto cleanup;

  req = dbus_message_new_method_call(MCE_SERVICE,
                                     MCE_REQUEST_PATH,
                                     MCE_REQUEST_IF,
                                     method);
  if( !req ) goto cleanup;

  dbus_message_set_auto_start(req, false);
  dbus_message_set_no_reply(req, true);

  if( !dbus_connection_send(systembus, req, 0) ) {
    log_error("failed to send %s.%s", MCE_REQUEST_IF, method);
    goto cleanup;
  }

  res = true;

cleanup:

  if( req ) dbus_message_unref(req);

  return res;
}

static bool xmce_cpu_keepalive_start(void)
{
  log_info("@ %s()", __FUNCTION__);
  return xmce_method_call(MCE_CPU_KEEPALIVE_START_REQ);
}

static bool xmce_cpu_keepalive_stop(void)
{
  log_info("@ %s()", __FUNCTION__);
  return xmce_method_call(MCE_CPU_KEEPALIVE_STOP_REQ);
}

#ifdef DEAD_CODE
static bool xmce_display_on(void)
{
  log_debug("@ %s()", __FUNCTION__);
  return xmce_method_call(MCE_DISPLAY_ON_REQ);
}

static bool xmce_display_off(void)
{
  log_debug("@ %s()", __FUNCTION__);
  return xmce_method_call(MCE_DISPLAY_OFF_REQ);
}

#endif

/* ------------------------------------------------------------------------- *
 * structure for controlling iphb timer & collecting wakeup statistics
 * ------------------------------------------------------------------------- */

enum
{
  HBTIMER_MAX_WAKEUPS = 16,
};

struct hbtimer_t
{
  // unique identifier
  int            id;

  // timer create time
  struct timeval created;

  // wakeup range
  int    mintime;
  int    maxtime;

  // number of times to repeat sleep
  int    repeats;

  // is the timer active?
  bool   active;

  // connection to server side
  iphb_t iphb_hnd;

  // io watch for iphb_hnd
  guint  iphb_id;

  // wakeup started hook
  void (*start_cb) (hbtimer_t *self);

  // renew hook
  void (*renew_cb) (hbtimer_t *self);
  int    renew_time;
  guint  renew_id;

  // finish hook
  void (*finish_cb)(hbtimer_t *self);
  int    finish_time;
  guint  finish_id;

  // wakeup started stats
  int    wakeups;
  struct timeval wakeup[HBTIMER_MAX_WAKEUPS];

  // wakeup finished stats
  int    finishes;
  struct timeval finish[HBTIMER_MAX_WAKEUPS];

  // NB: if changed, adjust hbtimer_ctor() and hbtimer_dtor() too
};

static int active_count = 0;

static void hbtimer_show_stats(const hbtimer_t *self)
{
  log_notice("statistics for timer %d:", self->id);
  log_notice("  range    %d-%d", self->mintime, self->maxtime);
  log_notice("  wakeups  %d/%d", self->wakeups, self->repeats);
  log_notice("  finishes %d/%d", self->finishes, self->repeats);

  for( int i = 0; i < self->repeats; ++i )
  {
    struct timeval b = {0,0};
    struct timeval e = {0,0};
    struct timeval d = {0,0};

    if( i < self->wakeups )
    {
      timersub(&self->wakeup[i], &self->created, &b);

      if( i < self->finishes )
      {
        timersub(&self->finish[i], &self->created, &e);
        timersub(&e, &b, &d);
      }
    }

    log_notice("  %2d: %7.3f .. %7.3f = %7.3f", i+1,
               b.tv_sec + b.tv_usec * 1e-6,
               e.tv_sec + e.tv_usec * 1e-6,
               d.tv_sec + d.tv_usec * 1e-6);
  }
}

static gboolean hbtimer_handle_wakeup_renew_cb(gpointer aptr)
{
  hbtimer_t *self = aptr;

  if( self->renew_cb )
  {
    self->renew_cb(self);
  }

  return TRUE;
}

static gboolean hbtimer_handle_wakeup_finish_cb(gpointer aptr)
{
  hbtimer_t *self = aptr;

  self->finish_id = 0;

  if( self->finish_cb )
  {
    self->finish_cb(self);
  }

  hbtimer_handle_wakeup_finish(self);
  return FALSE;
}

static void hbtimer_cancel_timers(hbtimer_t *self)
{
  if( self->renew_id )
  {
    g_source_remove(self->renew_id), self->renew_id = 0;
  }
  if( self->finish_id )
  {
    g_source_remove(self->finish_id), self->finish_id = 0;
  }
}

static void hbtimer_start_timers(hbtimer_t *self)
{
  if( !self->finish_id && self->finish_time > 0 )
  {
    self->finish_id = g_timeout_add_seconds(self->finish_time,
                                            hbtimer_handle_wakeup_finish_cb,
                                            self);
  }

  if( !self->renew_id && self->renew_time > 0 )
  {
    self->renew_id = g_timeout_add_seconds(self->renew_time,
                                           hbtimer_handle_wakeup_renew_cb,
                                           self);
  }
}

static void hbtimer_set_start(hbtimer_t *self, void (*cb)(hbtimer_t *))
{
  self->start_cb   = cb;
}

static void hbtimer_set_finish(hbtimer_t *self, void (*cb)(hbtimer_t *), int secs)
{
  self->finish_cb   = cb;
  self->finish_time = secs;
}

static void hbtimer_set_renew(hbtimer_t *self, void (*cb)(hbtimer_t *), int secs)
{
  self->renew_cb   = cb;
  self->renew_time = secs;
}

static bool hbtimer_deactivate(hbtimer_t *self)
{
  bool was_last = false;
  if( self->active )
  {
    self->active = false;
    if( --active_count == 0 )
    {
      was_last = true;
    }
  }
  return was_last;
}

static void hbtimer_activate(hbtimer_t *self)
{
  if( !self->active )
  {
    self->active = true;
    ++active_count;
  }
}

static void hbtimer_handle_wakeup_finish(hbtimer_t *self)
{
  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  if( self->finishes == HBTIMER_MAX_WAKEUPS )
  {
    abort();
  }

  hbtimer_cancel_timers(self);

  tv_get_monotime(&self->finish[self->finishes++]);

  if( self->finishes < self->repeats )
  {
    // nop
  }
  else if( hbtimer_deactivate(self) )
  {
    mainloop_stop(0);
  }
}

static void hbtimer_handle_wakeup_start(hbtimer_t *self)
{
  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  if( self->wakeups == HBTIMER_MAX_WAKEUPS )
  {
    abort();
  }

  tv_get_monotime(&self->wakeup[self->wakeups++]);

  if( self->wakeups < self->repeats )
  {
    hbtimer_start(self);
  }

  if( self->start_cb )
  {
    self->start_cb(self);
    hbtimer_start_timers(self);

  }

  if( !self->finish_id )
  {
    hbtimer_handle_wakeup_finish(self);
  }
}

static
gboolean
hbtimer_wakeup_cb(GIOChannel* chan, GIOCondition condition, gpointer data)
{
  hbtimer_t *self = data;
  log_info("@ %s(%d)", __FUNCTION__, self->id);

  gboolean keep_going = TRUE;

  char buf[256];

  /* Abandon watch if we get abnormal conditions from glib */
  if (condition & ~(G_IO_IN | G_IO_PRI))
  {
    log_error("unexpected io watch condition");
    keep_going = FALSE;
  }

  /* Read the data to clear input available state */
  int fd = g_io_channel_unix_get_fd(chan);
  int rc = read(fd, buf, sizeof buf);

  if( rc < 0 )
  {
    switch( errno )
    {
    case EINTR:
    case EAGAIN:
      break;

    default:
      log_error("io watch read: %m");
      keep_going = FALSE;
      break;
    }
  }
  else if( rc == 0 )
  {
    log_error("io watch read: EOF");
    keep_going = FALSE;
  }
  else
  {
    hbtimer_handle_wakeup_start(self);
  }

  /* Clear the timer id if we're going to stop */
  if( !keep_going )
  {
    log_error("io failure, disabling io watch");
    self->iphb_id = 0;
  }

  return keep_going;
}

static void hbtimer_disconnect(hbtimer_t *self)
{
  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  if( self->iphb_id )
  {
    g_source_remove(self->iphb_id), self->iphb_id = 0;
  }

  if( self->iphb_hnd )
  {
    iphb_close(self->iphb_hnd), self->iphb_hnd = 0;
  }
}

static bool hbtimer_connect(hbtimer_t *self)
{
  int         file  = -1;
  GIOChannel *chan  = 0;
  GError     *err   = 0;

  if( self->iphb_hnd )
  {
    goto cleanup;
  }

  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  if( !(self->iphb_hnd = iphb_open(0)) )
  {
    goto cleanup;

  }

  if( (file = iphb_get_fd(self->iphb_hnd)) == -1 )
  {
    goto cleanup;
  }

  if( !(chan = g_io_channel_unix_new(file)) )
  {
    log_error("creating io channel failed");
    goto cleanup;
  }

  /* iphb_hnd "owns" the file descriptor */
  g_io_channel_set_close_on_unref(chan, false);

  /* Set to NULL encoding so that we can turn off the buffering */
  if( g_io_channel_set_encoding(chan, NULL, &err) != G_IO_STATUS_NORMAL )
  {
    log_warning("failed to set io channel encoding: %s",
                err ? err->message : "unknown reason");
    // try to continue anyway
  }
  g_io_channel_set_buffered(chan, false);

  self->iphb_id = g_io_add_watch(chan, G_IO_IN, hbtimer_wakeup_cb, self);
  if( !self->iphb_id )
  {
    log_error("failed to add io channel watch");
    goto cleanup;
  }

cleanup:

  g_clear_error(&err);

  if( chan ) g_io_channel_unref(chan);

  return self->iphb_hnd && self->iphb_id;
}

static bool hbtimer_start(hbtimer_t *self)
{
  log_debug("@ %s(%d)", __FUNCTION__, self->id);
  bool res = false;

  if( !hbtimer_connect(self) )
  {
    goto cleanup;
  }
  if( iphb_wait(self->iphb_hnd, self->mintime, self->maxtime, 0) < 0 )
  {
    goto cleanup;
  }

  res = true;

cleanup:

  return res;
}

static void hbtimer_ctor(hbtimer_t *self)
{
  static int id = 0;

  tv_get_monotime(&self->created);
  self->id = ++id;

  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  self->mintime     = 0;
  self->maxtime     = 0;
  self->repeats     = 0;

  self->active      = false;
  self->iphb_hnd    = 0;
  self->iphb_id     = 0;

  self->start_cb    = 0;
  self->renew_cb    = 0;
  self->finish_cb   = 0;

  self->renew_time  = 0;
  self->finish_time = 0;

  self->renew_id    = 0;
  self->finish_id   = 0;

  self->wakeups     = 0;
  self->finishes    = 0;
}

static void hbtimer_dtor(hbtimer_t *self)
{
  log_debug("@ %s(%d)", __FUNCTION__, self->id);

  hbtimer_cancel_timers(self);
  hbtimer_deactivate(self);
  hbtimer_disconnect(self);
}

static void hbtimer_delete(hbtimer_t *self)
{
  if( self != 0 )
  {
    hbtimer_dtor(self);
    free(self);
  }
}

static void hbtimer_setup(hbtimer_t *self, int mintime, int maxtime, int repeats)
{
  log_info("@ %s(%d, %d, %d, %d)", __FUNCTION__, self->id, mintime, maxtime, repeats);

  self->mintime = mintime;
  self->maxtime = maxtime;
  self->repeats = repeats;

  if( repeats > 0 )
  {
    hbtimer_activate(self);
    hbtimer_start(self);
  }
}

static hbtimer_t *hbtimer_create(int mintime, int maxtime, int repeats)
{
  hbtimer_t *self = calloc(1, sizeof *self);
  hbtimer_ctor(self);
  hbtimer_setup(self, mintime, maxtime, repeats);
  return self;
}

static bool hbtimer_woke_up_at(const hbtimer_t *self, const struct timeval *tv)
{
  for( int i = 0; i < self->wakeups; ++i )
  {
    int ms = tv_diff_in_ms(&self->wakeup[i], tv);

    if( abs(ms) < 100 )
    {
      return true;
    }
  }
  return false;
}

static bool hbtimer_is_aligned_with(const hbtimer_t *self, const hbtimer_t *that)
{
  for( int i = 0; i < self->wakeups; ++i )
  {
    if( !hbtimer_woke_up_at(that, &self->wakeup[i]) )
    {
      return false;
    }
  }
  return true;
}

static int hbtimer_common_wakeups(const hbtimer_t *self, const hbtimer_t *that)
{
  int res = 0;
  for( int i = 0; i < self->wakeups; ++i )
  {
    if( hbtimer_woke_up_at(that, &self->wakeup[i]) )
    {
      ++res;
    }
  }
  return res;
}

/* ------------------------------------------------------------------------- *
 * helpers for checking if requirements are met
 * ------------------------------------------------------------------------- */

static void req_simultaneous(int *xc, const hbtimer_t *a, const hbtimer_t *b)
{
  if( !hbtimer_is_aligned_with(a, b) || !hbtimer_is_aligned_with(b,a) )
  {
    log_error("timer %d and timer %d wakeups were not simultaneous", a->id, b->id);
    *xc = EXIT_FAILURE;
  }
}

static void req_in_range(int *xc, const hbtimer_t *a)
{
  if( a->wakeups )
  {
    int lo = a->mintime * 1000;
    int hi = a->maxtime * 1000;
    int ms = tv_diff_in_ms(&a->wakeup[0], &a->created);

    if( ms < lo || ms > hi + ALLOWED_DELAY )
    {
      log_error("timer %d wait time %d ms out of range %d - %d ms", a->id, ms, lo, hi);
      *xc = EXIT_FAILURE;
    }
  }
}

static void req_aligned(int *xc, const hbtimer_t *a, const hbtimer_t *b)
{
  if( !hbtimer_is_aligned_with(a, b) )
  {
    log_error("timer %d wakeups were not aligned with timer %d", a->id, b->id);
    *xc = EXIT_FAILURE;
  }
}

static void req_common(int *xc, const hbtimer_t *a, const hbtimer_t *b, int n)
{
  int m = hbtimer_common_wakeups(a, b);

  if( m != n )
  {
    log_error("common wakeups for %d and %d: %d, expected %d",
              a->id, b->id, m, n);
    *xc = EXIT_FAILURE;
  }
}

static void req_period(int *xc, const hbtimer_t *a, int n)
{
  n *= 1000;

  for( int i = 1; i < a->wakeups; ++i )
  {
    int ms = tv_diff_in_ms(&a->wakeup[i], &a->wakeup[i-1]);

    if( abs(ms - n) > ALLOWED_DELAY )
    {
      log_error("timer %d, wakeup %d is %d ms, expected %d", a->id, i+1, ms, n);
      *xc = EXIT_FAILURE;
    }
  }
}

static void req_wakeups(int *xc, const hbtimer_t *a)
{
  if( a->wakeups != a->repeats )
  {
    log_error("timer %d woke up %d times, expected %d times",
              a->id, a->wakeups, a->repeats);
    *xc = EXIT_FAILURE;
  }
}

static void req_finishes(int *xc, const hbtimer_t *a, int n)
{
  if( a->finishes != a->repeats )
  {
    log_error("worktime %d finished %d times, expected %d times",
              a->id, a->finishes, a->repeats);
    *xc = EXIT_FAILURE;
  }

  if( a->finishes != a->wakeups )
  {
    log_error("worktime %d started %d times, but finished %d times",
              a->id, a->wakeups, a->finishes);
    *xc = EXIT_FAILURE;
  }

  n *= 1000;

  for( int i = 0; i < a->finishes && i < a->wakeups; ++i )
  {
    int ms = tv_diff_in_ms(&a->finish[i], &a->wakeup[i]);

    if( abs(ms - n) > ALLOWED_DELAY )
    {
      log_error("timer %d, worktime %d is %d ms, expected %d", a->id, i+1, ms, n);
      *xc = EXIT_FAILURE;
    }
  }
}

/* ------------------------------------------------------------------------- *
 * test is taking too long timeout
 * ------------------------------------------------------------------------- */

static gboolean failure_cb(gpointer user_data)
{
  log_error("test case did not finish in time");
  mainloop_stop(EXIT_FAILURE);
  return *(guint *)user_data = 0, FALSE;
}

/* ------------------------------------------------------------------------- *
 * test global wakeup slot triggering
 * ------------------------------------------------------------------------- */

static void slots_test(int *xc)
{
  log_notice("testing global wakeup slots");

  hbtimer_t *timer[16] = {};
  int        timers    = 0;
  guint      timeout   = 0;

  /* start timers, expected wakeup pattern something like
   *
   *      30  60  90  120 150 180
   *    ---|---|---|---|---|---|---> [monotime]
   *       |   |   |   |   |   |
   * 30    W   W   W   W   W   W
   * 60    |   W   |   W   |   W
   * 90    |   |   W   |   |   W
   */
  timer[timers++] = hbtimer_create(30, 30, 6);
  timer[timers++] = hbtimer_create(60, 60, 3);
  timer[timers++] = hbtimer_create(90, 90, 2);

  /* fail if tests do not finish in time */
  timeout = g_timeout_add_seconds(180 + 10, failure_cb, &timeout);

  if( mainloop_run() )
  {
    *xc = EXIT_FAILURE;
  }

  /* show stats */
  for( int i = 0; i < timers; ++i )
  {
    hbtimer_show_stats(timer[i]);
  }

  /* expected number of wakeups met? */
  for( int i = 0; i < timers; ++i )
  {
    req_wakeups(xc, timer[i]);
  }

  /* check that wakeup periods are roughly as expected */
  req_period(xc, timer[0], 30);
  req_period(xc, timer[1], 60);
  req_period(xc, timer[2], 90);

  /* 60 & 90 s periods must be aligned with the 30 s one*/
  req_aligned(xc, timer[1], timer[0]);
  req_aligned(xc, timer[2], timer[0]);

  /* 60 s period should have 3 common wakeups with 30 s one */
  req_common(xc, timer[1], timer[0], 3);

  /* 90 s period should have 2 common wakeups with 30 s one */
  req_common(xc, timer[2], timer[0], 2);

  /* 90 s period should have 1 common wakeups with 60 s one */
  req_common(xc, timer[2], timer[1], 1);

  if( timeout )
  {
    g_source_remove(timeout);
  }

  for( int i = 0; i < timers; ++i )
  {
    hbtimer_delete(timer[i]);
  }
}

/* ------------------------------------------------------------------------- *
 * test mintime-maxtime wakeup triggering
 * ------------------------------------------------------------------------- */

static void ranges_test(int *xc)
{
  log_notice("testing ranged iphb wakeups");

  hbtimer_t *timer[16] = {};
  int        timers    = 0;
  guint      timeout   = 0;

  auto int  scale(int t);
  auto void start(int lo, int hi);

  auto int  scale(int t)
  {
    // NB: select such scaling factor that the server side
    //     does not modify the mintimes too much
    return 60 + t * 2;
  }

  auto void start(int lo, int hi)
  {
    timer[timers++] = hbtimer_create(scale(lo)+1, scale(hi), 1);
  }

  /* start timers, expected wakeup pattern something like */
  //                      0 1 2 3 4 5 6 7 8 9 0 1
  //                            |         |
  start(0, 9); // A        AAAAAAAAAAAAAAAAAA
  start(1, 7); // B          BBBBBBBBBBBB |
  start(2, 3); // C            CC         |
  //                            |         |
  start(5, 8); // D             |    DDDDDD
  start(6,11); // E             |      EEEEEEEEEE
  start(7,10); // F             |        FFFFFF
  //                            |         |
  //                      0 1 2 3 4 5 6 7 8 9 0 1
  //                            |         |
  //                          group1      group2

  /* fail if tests do not finish in time */
  timeout = g_timeout_add_seconds(scale(11) + 10, failure_cb, &timeout);

  if( mainloop_run() )
  {
    *xc = EXIT_FAILURE;
  }

  /* show stats */
  for( int i = 0; i < timers; ++i )
  {
    hbtimer_show_stats(timer[i]);
  }

  /* wakeups approximately as expected? */
  for( int i = 0; i < timers; ++i )
  {
    req_wakeups(xc, timer[i]);
    req_in_range(xc, timer[i]);
  }

  /* group 1 woke up simultaneously? */
  req_simultaneous(xc, timer[0], timer[1]);
  req_simultaneous(xc, timer[0], timer[2]);

  /* group 2 woke up simultaneously? */
  req_simultaneous(xc, timer[3], timer[4]);
  req_simultaneous(xc, timer[3], timer[5]);

  if( timeout )
  {
    g_source_remove(timeout);
  }

  for( int i = 0; i < timers; ++i )
  {
    hbtimer_delete(timer[i]);
  }
}

/* ------------------------------------------------------------------------- *
 * test resume from suspend + cpu keepalive wakeup
 * ------------------------------------------------------------------------- */

/** Issue cpu keepalive at start of wakeup */
static void keepalive_start_cb(hbtimer_t *self)
{
  log_debug("@ %s()", __FUNCTION__);
  xmce_cpu_keepalive_start();
}

/** ... renew it periodically */
static void keepalive_renew_cb(hbtimer_t *self)
{
  log_debug("@ %s()", __FUNCTION__);
  xmce_cpu_keepalive_start();
}

/** ... and terminate before going back to sleep */
static void keepalive_stop_cb(hbtimer_t *self)
{
  log_debug("@ %s()", __FUNCTION__);
  xmce_cpu_keepalive_stop();
}

static void keepalive_test(int *xc)
{
  log_notice("testing iphb wakeups with cpu keepalive");

  int res = EXIT_FAILURE;

  hbtimer_t *timer     = 0;
  guint      timeout   = 0;
  int        renew_max = 0;

  // TODO: the device should be in a such state that
  //       it will suspend during the 'sleep' periods

  /* --|----work----|--sleep---|----work----|--sleep-
   *   |            |          |            |
   *   WWWWWWWWWWWWWW          WWWWWWWWWWWWWW
   *   ^    ^    ^  ^          ^    ^    ^  ^
   *   |    |    |  |          |    |    |  |
   * -------------------------------------------------> t
   *   |    |    |  |          |    |    |  |
   *   |    |    |  finish     |    |    |  finish
   *   |    |    renew         |    |    renew
   *   |    renew              |    renew
   *   wakeup                  wakeup
   */

  int        slot      = 30;
  int        work      = 20;
  int        renew     =  8;
  int        repeats   =  3;

  if( !systembus_connect() )
  {
    goto cleanup;
  }

  renew_max = xmce_cpu_keepalive_period();
  log_info("keepalive period = %d s", renew_max);

  if( renew_max <= 0 )
  {
    goto cleanup;
  }

  if( renew > renew_max ) renew = renew_max;

  timer = hbtimer_create(slot, slot, repeats);

  hbtimer_set_start (timer, keepalive_start_cb);
  hbtimer_set_renew (timer, keepalive_renew_cb, renew);
  hbtimer_set_finish(timer, keepalive_stop_cb, work);

  /* fail if tests do not finish in time */
  timeout = g_timeout_add_seconds(slot*repeats + work + 10,
                                  failure_cb, &timeout);

  res = mainloop_run();

  /* show stats */
  hbtimer_show_stats(timer);

  /* wakeups approximately as expected? */
  req_wakeups(xc, timer);

  /* wakeup period as expected? */
  req_period(xc, timer, slot);

  /* wakeup lengths as expected? */
  req_finishes(xc, timer, work);

cleanup:

  if( timeout )
  {
    g_source_remove(timeout);
  }

  hbtimer_delete(timer);

  systembus_disconnect();

  if( *xc < res ) *xc = res;
}

/* ------------------------------------------------------------------------- *
 * main entry point
 * ------------------------------------------------------------------------- */

static struct option optL[] =
{
  {"help",      0, 0, 'h' },
  {"usage",     0, 0, 'h' },
  {"all",       0, 0, 'a' },
  {"slots",     0, 0, 's' },
  {"ranges",    0, 0, 'r' },
  {"keepalive", 0, 0, 'k' },

  {0,           0, 0,   0 }
};

static const char optS[] =
"h"
"a"
"s"
"r"
"k"
;

enum
{
  TEST_SLOTS     = (1<<0),
  TEST_RANGES    = (1<<1),
  TEST_KEEPALIVE = (1<<2),
  TEST_ALL       = ~0,
};

static void usage(const char *progname)
{
  printf("NAME\n"
         "  %s\n"
         "\n"
         "SYNOPSIS\n"
         "  %s <options>\n"
         "\n"
         "DESCRIPTION\n"
         "  Utility for testing libiphb timers\n"
         "\n"
         "OPTIONS\n"
         "  -h --help      This help text\n"
         "  -a --all       Do all tests\n"
         "  -s --slots     Test global wakeup slots\n"
         "  -r --ranges    Test ranged wakeups\n"
         "  -k --keepalive Test cpu keepalive wakeups\n"
         "\n",
         progname, progname);
}

int
main(int argc, char **argv)
{
  log_debug("@ %s()", __FUNCTION__);

  /* assume failure */
  int xc = EXIT_FAILURE;

  int tests = 0;

  /* process command line arguments */
  for( ;; )
  {
    int opt = getopt_long(argc, argv, optS, optL, 0);

    if( opt < 0 )
    {
      break;
    }

    switch( opt )
    {
    case 'h':
      usage(*argv);
      exit(EXIT_SUCCESS);

    case 'a':
      tests |= TEST_ALL;
      break;

    case 's':
      tests |= TEST_SLOTS;
      break;

    case 'r':
      tests |= TEST_RANGES;
      break;

    case 'k':
      tests |= TEST_KEEPALIVE;
      break;

    case '?':
      goto cleanup;

    default:
      log_error("getopt returned character code 0%o", opt);
      goto cleanup;
    }
  }

  if( optind < argc )
  {
    log_error("excess arguments");
    goto cleanup;
  }

  if( !tests )
  {
    log_error("no tests requested");
  }

  /* assume success */
  xc = EXIT_SUCCESS;

  /* run the requested tests */
  if( tests & TEST_SLOTS )
  {
    slots_test(&xc);
  }
  if( tests & TEST_RANGES )
  {
    ranges_test(&xc);
  }
  if( tests & TEST_KEEPALIVE )
  {
    keepalive_test(&xc);
  }

  cleanup:

  log_info("@ exit(%s)", xc ? "failure" : "success");

  return xc;
}

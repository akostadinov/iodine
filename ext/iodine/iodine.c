#include "iodine.h"
#include "iodine_http.h"
#include "iodine_protocol.h"
#include "iodine_websockets.h"
#include "rb-rack-io.h"
/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

VALUE Iodine;
VALUE IodineBase;
VALUE Iodine_Version;

ID iodine_fd_var_id;
ID iodine_timeout_var_id;
ID iodine_call_proc_id;
ID iodine_new_func_id;
ID iodine_on_open_func_id;
ID iodine_on_message_func_id;
ID iodine_on_data_func_id;
ID iodine_on_ready_func_id;
ID iodine_on_shutdown_func_id;
ID iodine_on_close_func_id;
ID iodine_ping_func_id;
ID iodine_buff_var_id;
ID iodine_to_s_method_id;
ID iodine_to_i_func_id;

rb_encoding *IodineBinaryEncoding;
rb_encoding *IodineUTF8Encoding;
int IodineBinaryEncodingIndex;
int IodineUTF8EncodingIndex;

/* *****************************************************************************
Internal helpers
***************************************************************************** */

static void iodine_run_task(void *block_) {
  RubyCaller.call((VALUE)block_, iodine_call_proc_id);
}
/* *****************************************************************************
Published functions
***************************************************************************** */

/** Returns the number of total connections managed by Iodine. */
static VALUE iodine_count(VALUE self) {
  size_t count = facil_count(NULL);
  return ULL2NUM(count);
  (void)self;
}

/**
Runs the required block after the specified number of milliseconds have passed.
Time is counted only once Iodine started running (using {Iodine.start}).

Tasks scheduled before calling {Iodine.start} will run once for every process.

Always returns a copy of the block object.
*/
static VALUE iodine_run_after(VALUE self, VALUE milliseconds) {
  (void)(self);
  if (TYPE(milliseconds) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "milliseconds must be a number");
    return Qnil;
  }
  size_t milli = FIX2UINT(milliseconds);
  // requires a block to be passed
  rb_need_block();
  VALUE block = rb_block_proc();
  if (block == Qnil)
    return Qfalse;
  Registry.add(block);
  facil_run_every(milli, 1, iodine_run_task, (void *)block,
                  (void (*)(void *))Registry.remove);
  return block;
}
/**
Runs the required block after the specified number of milliseconds have passed.
Time is counted only once Iodine started running (using {Iodine.start}).

Accepts:

milliseconds:: the number of milliseconds between event repetitions.

repetitions:: the number of event repetitions. Defaults to 0 (never ending).

block:: (required) a block is required, as otherwise there is nothing to
perform.

The event will repeat itself until the number of repetitions had been delpeted.

Always returns a copy of the block object.
*/
static VALUE iodine_run_every(int argc, VALUE *argv, VALUE self) {
  (void)(self);
  VALUE milliseconds, repetitions, block;

  rb_scan_args(argc, argv, "11&", &milliseconds, &repetitions, &block);

  if (TYPE(milliseconds) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "milliseconds must be a number.");
    return Qnil;
  }
  if (repetitions != Qnil && TYPE(repetitions) != T_FIXNUM) {
    rb_raise(rb_eTypeError, "repetitions must be a number or `nil`.");
    return Qnil;
  }

  size_t milli = FIX2UINT(milliseconds);
  size_t repeat = (repetitions == Qnil) ? 0 : FIX2UINT(repetitions);
  // requires a block to be passed
  rb_need_block();
  Registry.add(block);
  facil_run_every(milli, repeat, iodine_run_task, (void *)block,
                  (void (*)(void *))Registry.remove);
  return block;
}

/* *****************************************************************************
Running the server
***************************************************************************** */

#include "spnlock.inc"
#include <pthread.h>

static volatile int sock_io_thread = 0;
static pthread_t sock_io_pthread;

static void *iodine_io_thread(void *arg) {
  (void)arg;
  struct timespec tm;
  // static const struct timespec tm = {.tv_nsec = 524288UL};
  while (sock_io_thread) {
    sock_flush_all();
    tm = (struct timespec){.tv_nsec = 524288UL, .tv_sec = 1};
    nanosleep(&tm, NULL);
  }
  return NULL;
}
static void iodine_start_io_thread(void *a1, void *a2) {
  (void)a1;
  (void)a2;
  pthread_create(&sock_io_pthread, NULL, iodine_io_thread, NULL);
}
static void iodine_join_io_thread(void) {
  sock_io_thread = 0;
  pthread_join(sock_io_pthread, NULL);
}

static void *srv_start_no_gvl(void *_) {
  (void)(_);
  // collect requested settings
  VALUE rb_th_i = rb_iv_get(Iodine, "@threads");
  VALUE rb_pr_i = rb_iv_get(Iodine, "@processes");
  ssize_t threads = (TYPE(rb_th_i) == T_FIXNUM) ? FIX2LONG(rb_th_i) : 0;
  ssize_t processes = (TYPE(rb_pr_i) == T_FIXNUM) ? FIX2LONG(rb_pr_i) : 0;
// print a warnning if settings are sub optimal
#ifdef _SC_NPROCESSORS_ONLN
  size_t cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (processes <= 0)
    processes = 0;
  if (threads <= 0)
    threads = 0;

  if (processes && threads && cpu_count > 0 &&
      (((size_t)processes << 1) < cpu_count ||
       (size_t)processes > (cpu_count << 1)))
    fprintf(stderr,
            "* Performance warnning:\n"
            "  - This computer reports %lu available CPUs... "
            "they will not be fully utilized.\n",
            cpu_count);
#else
  if (processes <= 0)
    processes = 0;
  if (threads <= 0)
    threads = 0;
#endif
  sock_io_thread = 1;
  defer(iodine_start_io_thread, NULL, NULL);
  facil_run(.threads = threads, .processes = processes,
            .on_finish = iodine_join_io_thread);
  return NULL;
}

static void iodine_review_rack_app(void) {
  /* Check for Iodine::Rack.app and perform the C equivalent:
   *  Iodine::HTTP.listen app: @app, port: @port, address: @address, log: @log,
   *          max_msg: max_msg, max_body: max_body, public: @public, ping:
   *          @ws_timeout, timeout: @timeout
   */

  VALUE rack = rb_const_get(Iodine, rb_intern("Rack"));
  VALUE app = rb_ivar_get(rack, rb_intern("@app"));
  VALUE www = rb_ivar_get(rack, rb_intern("@public"));
  if ((app == Qnil || app == Qfalse) && (www == Qnil || www == Qfalse))
    return;
  VALUE opt = rb_hash_new();
  Registry.add(opt);

  rb_hash_aset(opt, ID2SYM(rb_intern("app")),
               rb_ivar_get(rack, rb_intern("@app")));
  rb_hash_aset(opt, ID2SYM(rb_intern("port")),
               rb_ivar_get(rack, rb_intern("@port")));
  rb_hash_aset(opt, ID2SYM(rb_intern("app")),
               rb_ivar_get(rack, rb_intern("@app")));
  rb_hash_aset(opt, ID2SYM(rb_intern("address")),
               rb_ivar_get(rack, rb_intern("@address")));
  rb_hash_aset(opt, ID2SYM(rb_intern("log")),
               rb_ivar_get(rack, rb_intern("@log")));
  rb_hash_aset(opt, ID2SYM(rb_intern("max_msg")),
               rb_ivar_get(rack, rb_intern("@max_msg")));
  rb_hash_aset(opt, ID2SYM(rb_intern("max_body")),
               rb_ivar_get(rack, rb_intern("@max_body")));
  rb_hash_aset(opt, ID2SYM(rb_intern("public")),
               rb_ivar_get(rack, rb_intern("@public")));
  rb_hash_aset(opt, ID2SYM(rb_intern("ping")),
               rb_ivar_get(rack, rb_intern("@ping")));
  rb_hash_aset(opt, ID2SYM(rb_intern("timeout")),
               rb_ivar_get(rack, rb_intern("@ws_timeout")));
  rb_funcall2(IodineHTTP, rb_intern("listen"), 1, &opt);
}

/**
Starts the Iodine event loop. This will hang the thread until an interrupt
(`^C`) signal is received.

Returns the Iodine module.
*/
static VALUE iodine_start(VALUE self) {
  /* for the special Iodine::Rack object and backwards compatibility */
  iodine_review_rack_app();
  rb_thread_call_without_gvl2(srv_start_no_gvl, (void *)self, NULL, NULL);
  return self;
}

/* *****************************************************************************
Library Initialization
***************************************************************************** */

////////////////////////////////////////////////////////////////////////
// Ruby loads the library and invokes the Init_<lib_name> function...
//
// Here we connect all the C code to the Ruby interface, completing the bridge
// between Lib-Server and Ruby.
void Init_iodine(void) {
  // initialize globally used IDs, for faster access to the Ruby layer.
  iodine_fd_var_id = rb_intern("scrtfd");
  iodine_call_proc_id = rb_intern("call");
  iodine_new_func_id = rb_intern("new");
  iodine_on_open_func_id = rb_intern("on_open");
  iodine_on_message_func_id = rb_intern("on_message");
  iodine_on_data_func_id = rb_intern("on_data");
  iodine_on_shutdown_func_id = rb_intern("on_shutdown");
  iodine_on_close_func_id = rb_intern("on_close");
  iodine_on_ready_func_id = rb_intern("on_ready");
  iodine_ping_func_id = rb_intern("ping");
  iodine_buff_var_id = rb_intern("scrtbuffer");
  iodine_timeout_var_id = rb_intern("@timeout");
  iodine_to_s_method_id = rb_intern("to_s");
  iodine_to_i_func_id = rb_intern("to_i");

  IodineBinaryEncodingIndex =
      rb_enc_find_index("binary"); // sets encoding for data
  IodineUTF8EncodingIndex =
      rb_enc_find_index("UTF-8");               // sets encoding for data
  IodineBinaryEncoding = rb_enc_find("binary"); // sets encoding for data
  IodineUTF8Encoding = rb_enc_find("UTF-8");    // sets encoding for data

  // The core Iodine module wraps libserver functionality and little more.
  Iodine = rb_define_module("Iodine");

  // Iodine_Version = rb_const_get(Iodine, rb_intern("VERSION"));

  // the Iodine singleton functions
  rb_define_module_function(Iodine, "start", iodine_start, 0);
  rb_define_module_function(Iodine, "count", iodine_count, 0);
  rb_define_module_function(Iodine, "run_after", iodine_run_after, 1);
  rb_define_module_function(Iodine, "run_every", iodine_run_every, -1);

  // Every Protocol (and Server?) instance will hold a reference to the server
  // define the Server Ruby class.
  IodineBase = rb_define_module_under(Iodine, "Base");

  // Initialize the registry under the Iodine core
  Registry.init(Iodine);

  /* Initialize the rest of the library. */
  Iodine_init_protocol();
  Iodine_init_http();
  Iodine_init_websocket();
}

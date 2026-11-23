/* Copyright (c) 2014, Ben Noordhuis <info@bnoordhuis.nl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "uv.h"
#include "task.h"

static void timer_cb(uv_timer_t* handle) {
  uv_close((uv_handle_t*) handle, NULL);
}


TEST_IMPL(loop_configure) {
  uv_timer_t timer_handle;
  uv_loop_t loop;
  ASSERT_OK(uv_loop_init(&loop));
#ifdef _WIN32
  ASSERT_EQ(UV_ENOSYS, uv_loop_configure(&loop, UV_LOOP_BLOCK_SIGNAL, 0));
#else
  ASSERT_OK(uv_loop_configure(&loop, UV_LOOP_BLOCK_SIGNAL, SIGPROF));
#endif
  ASSERT_OK(uv_timer_init(&loop, &timer_handle));
  ASSERT_OK(uv_timer_start(&timer_handle, timer_cb, 10, 0));
  ASSERT_OK(uv_run(&loop, UV_RUN_DEFAULT));
  ASSERT_OK(uv_loop_close(&loop));
  return 0;
}


#ifdef _WIN32
static struct {
  uv_async_t async;
  uv_timer_t timer;
  uv_sem_t did_begin_wait;
  int wakes_thread;
  int is_waiting;
  int waits;
  int callbacks;
  uint64_t wait_start;
  uint64_t time_waited;
} wait_test;


static void wait_cb(void* arg, int is_waiting) {
  ASSERT_PTR_EQ(arg, &wait_test);
  ASSERT_EQ(is_waiting, !wait_test.is_waiting);
  wait_test.is_waiting = is_waiting;

  if (is_waiting) {
    wait_test.waits++;
    wait_test.wait_start = uv_hrtime();
    if (wait_test.wakes_thread) {
      wait_test.wakes_thread = 0;
      uv_sem_post(&wait_test.did_begin_wait);
    }
  } else {
    wait_test.time_waited += uv_hrtime() - wait_test.wait_start;
  }

  /* The loop must go by the error of its own call. */
  SetLastError(ERROR_ACCESS_DENIED);
}


static void wait_timer_cb(uv_timer_t* handle) {
  ASSERT_OK(wait_test.is_waiting);
  wait_test.callbacks++;
}


static void wait_async_cb(uv_async_t* handle) {
  ASSERT_OK(wait_test.is_waiting);
  wait_test.callbacks++;
  uv_close((uv_handle_t*) handle, NULL);
}


static void wait_thread(void* arg) {
  uv_sem_wait(&wait_test.did_begin_wait);
  uv_sleep(100);
  ASSERT_OK(uv_async_send(&wait_test.async));
}


/* UV_RUN_ONCE does not update the time before it polls, so the wait is as long
 * as the timeout, however late it begins. */
static void wait_for_timer(uv_loop_t* loop) {
  ASSERT_OK(uv_timer_start(&wait_test.timer, wait_timer_cb, 100, 0));
  while (uv_run(loop, UV_RUN_ONCE));
}
#endif


TEST_IMPL(loop_wait_cb) {
#ifndef _WIN32
  RETURN_SKIP("Windows only.");
#else
  uv_thread_t thread;
  uv_loop_t loop;
  int waits;

  ASSERT_OK(uv_loop_init(&loop));
  ASSERT_OK(uv_timer_init(&loop, &wait_test.timer));
  uv_loop_set_wait_cb(&loop, wait_cb, &wait_test);

  /* A poll that cannot block is no wait. */
  ASSERT_OK(uv_timer_start(&wait_test.timer, wait_timer_cb, 60 * 1000, 0));
  ASSERT_NE(0, uv_run(&loop, UV_RUN_NOWAIT));
  ASSERT_OK(wait_test.waits);

  /* A wait that times out. */
  wait_for_timer(&loop);
  ASSERT_EQ(1, wait_test.callbacks);
  ASSERT_GE(wait_test.waits, 1);
  ASSERT_UINT64_GE(wait_test.time_waited, 50 * 1000 * 1000);

  /* A wait that ends in a completion is over when its callback runs. */
  waits = wait_test.waits;
  wait_test.time_waited = 0;
  wait_test.wakes_thread = 1;
  ASSERT_OK(uv_sem_init(&wait_test.did_begin_wait, 0));
  ASSERT_OK(uv_async_init(&loop, &wait_test.async, wait_async_cb));
  ASSERT_OK(uv_thread_create(&thread, wait_thread, NULL));
  ASSERT_OK(uv_run(&loop, UV_RUN_DEFAULT));
  ASSERT_OK(uv_thread_join(&thread));
  uv_sem_destroy(&wait_test.did_begin_wait);
  ASSERT_EQ(2, wait_test.callbacks);
  ASSERT_GT(wait_test.waits, waits);
  ASSERT_UINT64_GE(wait_test.time_waited, 50 * 1000 * 1000);

  /* This option puts a poll that cannot block before the one that can. */
  waits = wait_test.waits;
  wait_test.time_waited = 0;
  ASSERT_OK(uv_loop_configure(&loop, UV_METRICS_IDLE_TIME));
  wait_for_timer(&loop);
  ASSERT_EQ(3, wait_test.callbacks);
  ASSERT_GT(wait_test.waits, waits);
  ASSERT_UINT64_GE(wait_test.time_waited, 50 * 1000 * 1000);

  waits = wait_test.waits;
  uv_loop_set_wait_cb(&loop, NULL, NULL);
  wait_for_timer(&loop);
  ASSERT_EQ(4, wait_test.callbacks);
  ASSERT_EQ(waits, wait_test.waits);
  ASSERT_OK(wait_test.is_waiting);

  uv_close((uv_handle_t*) &wait_test.timer, NULL);
  ASSERT_OK(uv_run(&loop, UV_RUN_DEFAULT));
  ASSERT_OK(uv_loop_close(&loop));
  return 0;
#endif
}

/*
 * Copyright (c) 2016, Chris Fallin <cfallin@c1f.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "nk/msg.h"
#include "test.h"

struct msg_cross_messages_arg {
  nk_port *this_port, *other_port;
  int flag1, flag2;
  int value;
};

static void msg_cross_messages_thd(nk_thd *self, void *_arg) {
  struct msg_cross_messages_arg *arg = _arg;
  if (nk_msg_send(arg->other_port, arg->this_port, &arg->flag1, &arg->flag2) !=
      NK_OK) {
    return;
  }
  nk_msg *m;
  if (nk_msg_recv(arg->this_port, &m) != NK_OK) {
    return;
  }
  if (m->src != arg->other_port || m->dest != arg->this_port) {
    return;
  }
  int *other_flag1 = m->data1;
  int *other_flag2 = m->data2;
  *other_flag1 = arg->value;
  *other_flag2 = arg->value + 1;
  nk_msg_destroy(m);
}

NK_TEST(msg_cross_messages) {
  nk_host *h;
  NK_TEST_ASSERT(nk_host_create(&h) == NK_OK);

  nk_thd *thd1, *thd2;
  struct msg_cross_messages_arg arg1, arg2;
  arg1.flag1 = arg2.flag1 = 0;
  arg1.flag2 = arg2.flag2 = 0;
  arg1.value = 84;
  arg2.value = 42;

  NK_TEST_ASSERT(nk_port_create(h, &arg1.this_port, NK_PORT_THD) == NK_OK);
  NK_TEST_ASSERT(nk_port_create(h, &arg2.this_port, NK_PORT_THD) == NK_OK);
  arg1.other_port = arg2.this_port;
  arg2.other_port = arg1.this_port;

  NK_TEST_ASSERT(nk_thd_create_ext(h, &thd1, msg_cross_messages_thd, &arg1) ==
                 NK_OK);
  NK_TEST_ASSERT(nk_thd_create_ext(h, &thd2, msg_cross_messages_thd, &arg2) ==
                 NK_OK);
  nk_host_run(h, 2);
  nk_port_destroy(arg2.this_port);
  nk_port_destroy(arg1.this_port);
  nk_host_destroy(h);

  NK_TEST_ASSERT(arg1.flag1 == 42);
  NK_TEST_ASSERT(arg1.flag2 == 43);
  NK_TEST_ASSERT(arg2.flag1 == 84);
  NK_TEST_ASSERT(arg2.flag2 == 85);

  NK_TEST_OK();
}

struct msg_ring_arg {
  nk_port *this_port, *next_port;
  int done_flag;
  int is_last;
};

static void msg_ring_thd(nk_thd *self, void *_arg) {
  static const int kIters = 100;
  struct msg_ring_arg *arg = _arg;

  for (int i = 0; i < kIters; i++) {
    nk_msg *m;
    if (nk_msg_recv(arg->this_port, &m) != NK_OK) {
      return;
    }
    if (arg->is_last && i == (kIters - 1)) {
      nk_msg_destroy(m);
      break;
    }
    if (nk_msg_send(arg->next_port, arg->this_port, m->data1, m->data2) !=
        NK_OK) {
      return;
    }
    nk_msg_destroy(m);
  }
  arg->done_flag = 1;
}

static void msg_ring_start_dpc(void *arg) {
  nk_port *port = arg;
  nk_msg_send(port, /* from = */ NULL, /* data1 = */ NULL, /* data2 = */ NULL);
}

NK_TEST(msg_ring) {
  static const int kRingSize = 100;

  nk_host *h;
  NK_TEST_ASSERT(nk_host_create(&h) == NK_OK);

  nk_thd *thds[kRingSize];
  nk_port *ports[kRingSize];
  struct msg_ring_arg args[kRingSize];

  for (int i = 0; i < kRingSize; i++) {
    NK_TEST_ASSERT(nk_thd_create_ext(h, &thds[i], msg_ring_thd, &args[i]) ==
                   NK_OK);
    NK_TEST_ASSERT(nk_port_create(h, &ports[i], NK_PORT_THD) == NK_OK);
  }
  for (int i = 0; i < kRingSize; i++) {
    int next_i = (i + 1) % kRingSize;
    args[i].this_port = ports[i];
    args[i].next_port = ports[next_i];
    args[i].done_flag = 0;
    args[i].is_last = (i == (kRingSize - 1));
  }

  nk_dpc *startdpc;
  NK_TEST_ASSERT(
      nk_dpc_create_ext(h, &startdpc, msg_ring_start_dpc, ports[0]) == NK_OK);

  nk_host_run(h, 16);

  for (int i = 0; i < kRingSize; i++) {
    NK_TEST_ASSERT(args[i].done_flag == 1);
  }

  for (int i = 0; i < kRingSize; i++) {
    nk_port_destroy(ports[i]);
  }

  nk_host_destroy(h);

  NK_TEST_OK();
}

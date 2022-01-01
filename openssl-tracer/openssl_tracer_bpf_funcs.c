/*
 * Copyright 2018- The Pixie Authors.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <linux/ptrace.h>

#include "openssl_tracer_types.h"

BPF_PERF_OUTPUT(tls_events);

/***********************************************************
 * Internal structs and definitions
 ***********************************************************/

struct buffer{
  const char* content;
  size_t* len;
};

// Key is thread ID (from bpf_get_current_pid_tgid).
// Value is a pointer to the data buffer argument to SSL_write/SSL_read.
BPF_HASH(active_ssl_read_args_map, uint64_t, const char*);
BPF_HASH(active_ssl_write_args_map, uint64_t, const char*);
BPF_HASH(active_ssl_readex_buf_len_map, uint64_t, struct buffer);

// BPF_HASH(active_ssl_writeex_buf_len_args_map, uint64_t, size_t*);

// BPF programs are limited to a 512-byte stack. We store this value per CPU
// and use it as a heap allocated value.
BPF_PERCPU_ARRAY(data_buffer_heap, struct ssl_data_event_t, 1);
BPF_PERCPU_ARRAY(args_buffer_heap, struct buffer, 1);

/***********************************************************
 * General helper functions
 ***********************************************************/

static __inline struct ssl_data_event_t* create_ssl_data_event(uint64_t current_pid_tgid) {
  uint32_t kZero = 0;
  struct ssl_data_event_t* event = data_buffer_heap.lookup(&kZero);
  if (event == NULL) {
    return NULL;
  }

  const uint32_t kMask32b = 0xffffffff;
  event->timestamp_ns = bpf_ktime_get_ns();
  event->pid = current_pid_tgid >> 32;
  event->tid = current_pid_tgid & kMask32b;

  return event;
}
static __inline struct buffer* create_ssl_args_buffer() {
  uint32_t kZero = 0;
  struct buffer* _buf = args_buffer_heap.lookup(&kZero);
  if (_buf == NULL) {
    return NULL;
  }
  return _buf;
}

/***********************************************************
 * BPF syscall processing functions
 ***********************************************************/

static int process_SSL_data(struct pt_regs* ctx, uint64_t id, enum ssl_data_event_type type,
                            const char* buf) {
  int len = (int)PT_REGS_RC(ctx);
  if (len < 0) {
    return 0;
  }

  struct ssl_data_event_t* event = create_ssl_data_event(id);
  if (event == NULL) {
    return 0;
  }

  event->type = type;
  // This is a max function, but it is written in such a way to keep older BPF verifiers happy.
  event->data_len = (len < MAX_DATA_SIZE ? (len & (MAX_DATA_SIZE - 1)) : MAX_DATA_SIZE);
  bpf_probe_read(event->data, event->data_len, buf);
  tls_events.perf_submit(ctx, event, sizeof(struct ssl_data_event_t));

  return 0;
}
static int process_SSL_ex_data(struct pt_regs* ctx, uint64_t id, enum ssl_data_event_type type,
                            struct buffer* _buf) {
  int is_success = (int)PT_REGS_RC(ctx);
  if (is_success == 0) {
    return 0;
  }

  struct ssl_data_event_t* event = create_ssl_data_event(id);
  if (event == NULL) {
    return 0;
  }
  int length = (int) *(_buf->len);

  event->type = type;
  // This is a max function, but it is written in such a way to keep older BPF verifiers happy.
  event->data_len = (length < MAX_DATA_SIZE ? (length & (MAX_DATA_SIZE - 1)) : MAX_DATA_SIZE);
  bpf_probe_read(event->data, event->data_len, _buf->content);
  tls_events.perf_submit(ctx, event, sizeof(struct ssl_data_event_t));

  return 0;
}


/***********************************************************
 * BPF probe function entry-points
 ***********************************************************/

// Function signature being probed:
// int SSL_write(SSL *ssl, const void *buf, int num);
int probe_entry_SSL_write(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  const char* buf = (const char*)PT_REGS_PARM2(ctx);
  active_ssl_write_args_map.update(&current_pid_tgid, &buf);

  return 0;
}

int probe_ret_SSL_write(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  const char** buf = active_ssl_write_args_map.lookup(&current_pid_tgid);
  if (buf != NULL) {
    process_SSL_data(ctx, current_pid_tgid, kSSLWrite, *buf);
  }

  active_ssl_write_args_map.delete(&current_pid_tgid);
  return 0;
}

// Function signature being probed:
// int SSL_read(SSL *s, void *buf, int num)
int probe_entry_SSL_read(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  const char* buf = (const char*)PT_REGS_PARM2(ctx);

  active_ssl_read_args_map.update(&current_pid_tgid, &buf);
  return 0;
}

int probe_ret_SSL_read(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  const char** buf = active_ssl_read_args_map.lookup(&current_pid_tgid);
  if (buf != NULL) {
    process_SSL_data(ctx, current_pid_tgid, kSSLRead, *buf);
  }

  active_ssl_read_args_map.delete(&current_pid_tgid);
  return 0;
}
// int probe_entry_SSL_write_ex(struct pt_regs* ctx) {
//   uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
//   uint32_t pid = current_pid_tgid >> 32;

//   if (pid != TRACE_PID) {
//     return 0;
//   }

//   const char* buf = (const char*)PT_REGS_PARM2(ctx);
//   active_ssl_write_args_map.update(&current_pid_tgid, &buf);

//   return 0;
// }

// int probe_ret_SSL_write_ex(struct pt_regs* ctx) {
//   uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
//   uint32_t pid = current_pid_tgid >> 32;

//   if (pid != TRACE_PID) {
//     return 0;
//   }

//   const char** buf = active_ssl_write_args_map.lookup(&current_pid_tgid);
//   if (buf != NULL) {
//     process_SSL_data(ctx, current_pid_tgid, kSSLWrite, *buf);
//   }

//   active_ssl_write_args_map.delete(&current_pid_tgid);
//   return 0;
// }


// Function signature being probed:
// int SSL_read_ex(SSL *ssl, void *buf, size_t num, size_t *readbytes);
// SSL_read_ex() and SSL_read() try to read num bytes from the specified ssl into the buffer buf. On success SSL_read_ex() will store the number of bytes actually read in *readbytes.
int probe_entry_SSL_read_ex(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  const char* buf = (const char*)PT_REGS_PARM2(ctx);
  size_t* buf_len = (size_t *)PT_REGS_PARM4(ctx);
  struct buffer* _buf = create_ssl_args_buffer();
  if (_buf == NULL)
  {
    return 0;
  }
  _buf->content = buf;
  _buf->len = buf_len;


  active_ssl_readex_buf_len_map.update(&current_pid_tgid, _buf);
  return 0;
}

int probe_ret_SSL_read_ex(struct pt_regs* ctx) {
  uint64_t current_pid_tgid = bpf_get_current_pid_tgid();
  uint32_t pid = current_pid_tgid >> 32;

  if (pid != TRACE_PID) {
    return 0;
  }

  struct buffer* _buf = active_ssl_readex_buf_len_map.lookup(&current_pid_tgid);
  if (_buf != NULL) {
    process_SSL_ex_data(ctx, current_pid_tgid, kSSLRead, _buf);
  }

  active_ssl_read_args_map.delete(&current_pid_tgid);
  return 0;
}
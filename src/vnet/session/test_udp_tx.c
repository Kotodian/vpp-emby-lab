/* SPDX-License-Identifier: Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <svm/fifo_segment.h>
#include <vnet/session/session_tx.h>

static unsigned int checks;

#define CHECK(expr)                                                                                \
  do                                                                                               \
    {                                                                                              \
      checks++;                                                                                    \
      if (!(expr))                                                                                 \
	{                                                                                          \
	  fprintf (stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr);                       \
	  exit (1);                                                                                \
	}                                                                                          \
    }                                                                                              \
  while (0)

static void
enqueue (svm_fifo_t *f, u32 len, u16 gso, u32 offset, u16 port)
{
  session_dgram_hdr_t hdr = {
    .data_length = len, .data_offset = offset, .rmt_port = port, .gso_size = gso, .is_ip4 = 1
  };
  u8 data[1500];
  clib_memset (data, port & 0xff, len);
  CHECK (svm_fifo_enqueue (f, sizeof (hdr), (u8 *) &hdr) == sizeof (hdr));
  CHECK (svm_fifo_enqueue (f, len, data) == len);
}

static void
consume (svm_fifo_t *f, u32 len, u16 port)
{
  session_dgram_hdr_t hdr;
  u8 data[1500];
  CHECK (svm_fifo_dequeue (f, sizeof (hdr), (u8 *) &hdr) == sizeof (hdr));
  CHECK (hdr.data_length == len && hdr.rmt_port == port);
  CHECK (svm_fifo_dequeue (f, len, data) == len);
  for (u32 i = 0; i < len; i++)
    CHECK (data[i] == (port & 0xff));
}

int
main (void)
{
  fifo_segment_main_t sm = { 0 };
  fifo_segment_create_args_t a = { .segment_type = SSVM_SEGMENT_PRIVATE,
				   .segment_size = 2 << 20,
				   .segment_name = "udp-tx-test" };
  fifo_segment_t *fs;
  svm_fifo_t *f;
  u32 bytes, available;
  const u32 lengths[] = { 20, 1200, 28, 1400 };

  clib_mem_init (0, 32 << 20);
  fifo_segment_main_init (&sm, 0, 5);
  CHECK (fifo_segment_create (&sm, &a) == 0);
  fs = fifo_segment_get_segment (&sm, a.new_segment_indices[0]);
  f = fifo_segment_alloc_fifo_w_slice (fs, 0, 64 << 10, FIFO_SEGMENT_RX_FIFO);
  CHECK (f != 0);

  for (u32 i = 0; i < ARRAY_LEN (lengths); i++)
    enqueue (f, lengths[i], 0, 0, 4000 + i);
  available = svm_fifo_max_dequeue_cons (f);
  CHECK (session_tx_plan_dgrams (f, available, 16, 1472, ~0, &bytes) == 4);
  CHECK (bytes == 2648 && svm_fifo_max_dequeue_cons (f) == available);
  CHECK (session_tx_plan_dgrams (f, available, 2, 1472, ~0, &bytes) == 2 && bytes == 1220);
  CHECK (session_tx_plan_dgrams (f, available, 16, 1472, 1000, &bytes) == 1 && bytes == 20);
  CHECK (session_tx_plan_dgrams (f, available, 16, 1000, ~0, &bytes) == 1);
  CHECK (session_tx_plan_dgrams (f, sizeof (session_dgram_hdr_t) - 1, 16, 1472, ~0, &bytes) == 0 &&
	 bytes == 0);
  CHECK (session_tx_plan_dgrams (f, available, 16, 1472, 19, &bytes) == 0);
  for (u32 i = 0; i < ARRAY_LEN (lengths); i++)
    consume (f, lengths[i], 4000 + i);

  enqueue (f, 1200, 1200, 0, 5000);
  CHECK (session_tx_plan_dgrams (f, svm_fifo_max_dequeue_cons (f), 16, 1472, ~0, &bytes) == 0);
  consume (f, 1200, 5000);
  enqueue (f, 1200, 0, 100, 5001);
  CHECK (session_tx_plan_dgrams (f, svm_fifo_max_dequeue_cons (f), 16, 1472, ~0, &bytes) == 0);
  consume (f, 1200, 5001);

  session_dgram_hdr_t partial = { .data_length = 1200 };
  u8 data[1200] = { 0 };
  CHECK (svm_fifo_enqueue (f, sizeof (partial), (u8 *) &partial) == sizeof (partial));
  CHECK (svm_fifo_enqueue (f, 20, data) == 20);
  CHECK (session_tx_plan_dgrams (f, svm_fifo_max_dequeue_cons (f), 16, 1472, ~0, &bytes) == 0);
  CHECK (svm_fifo_enqueue (f, 1180, data) == 1180);
  CHECK (session_tx_plan_dgrams (f, svm_fifo_max_dequeue_cons (f), 16, 1472, ~0, &bytes) == 1 &&
	 bytes == 1200);
  consume (f, 1200, 0);

  /* Exercise recycled FIFO chunks with mixed lengths and distinct peers. */
  for (u32 round = 0; round < 32; round++)
    {
      for (u32 i = 0; i < 32; i++)
	enqueue (f, i & 1 ? 1200 : 20, 0, 0, 6000 + i);
      CHECK (session_tx_plan_dgrams (f, svm_fifo_max_dequeue_cons (f), 32, 1472, ~0, &bytes) ==
	       32 &&
	     bytes == 19520);
      for (u32 i = 0; i < 32; i++)
	consume (f, i & 1 ? 1200 : 20, 6000 + i);
    }
  printf ("PASS: %u mixed-length UDP TX checks\n", checks);
  return 0;
}

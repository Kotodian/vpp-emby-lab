/* SPDX-License-Identifier: Apache-2.0 */
#ifndef included_session_tx_h
#define included_session_tx_h

#include <vnet/session/session_types.h>

/* Plan a batch of complete, single-buffer datagrams without consuming it.
 * ACKs and payloads may have different lengths and endpoint metadata. GSO
 * and partially consumed datagrams retain the segmented transmit path. */
always_inline u32
session_tx_plan_dgrams (svm_fifo_t *f, u32 fifo_bytes, u32 max_packets, u32 max_dgram_bytes,
			u32 max_payload_bytes, u32 *payload_bytes)
{
  u32 offset = 0, count = 0, total = 0;
  session_dgram_hdr_t hdr;

  while (count < max_packets && fifo_bytes - offset >= sizeof (hdr))
    {
      if (svm_fifo_peek (f, offset, sizeof (hdr), (u8 *) &hdr) != sizeof (hdr))
	break;
      if (!hdr.data_length || hdr.data_offset || hdr.gso_size ||
	  hdr.data_length > max_dgram_bytes || hdr.data_length > max_payload_bytes - total ||
	  hdr.data_length > fifo_bytes - offset - sizeof (hdr))
	break;
      total += hdr.data_length;
      offset += sizeof (hdr) + hdr.data_length;
      count++;
    }
  *payload_bytes = total;
  return count;
}

#endif

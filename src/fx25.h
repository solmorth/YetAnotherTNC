#ifndef FX25_H_
#define FX25_H_

#include <stdint.h>
#include <stddef.h>

/*
 * FX.25: Forward Error Correction extension to AX.25
 * (Jim McGuire KB3MPL / Phil Karn KA9Q algorithm, as used by Direwolf and
 * other amateur packet TNCs - http://www.stensat.org/docs/FX-25_01_06.pdf)
 *
 * An AX.25 frame is HDLC bit-stuffed and flag-padded to fill a fixed
 * 239-byte block, a 16-byte Reed-Solomon RS(255,239) parity block is
 * computed over it, and an 8-byte correlation tag identifying the format
 * is prefixed on air. A receiver that recognizes the tag can correct up
 * to 8 corrupted bytes per block before even looking at the AX.25 FCS.
 *
 * Only the single "Tag_01" RS(255,239) format is implemented (the
 * mandatory/most common one - Direwolf falls back to it for any frame
 * that fits). Frames too large for one block (see fx25_build_block)
 * simply aren't eligible for FX.25 and the caller should fall back to
 * plain AX.25.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define FX25_TAG_01        0xB74DB7DF8A532F3EULL /* RS(255,239): 16 check bytes, 239 info bytes */
#define FX25_TAG_BYTES     8
#define FX25_BLOCK_SIZE    255
#define FX25_DATA_SIZE     239
#define FX25_CHECK_SIZE    16
#define FX25_TAG_MAX_HAMMING 8 /* bit errors tolerated in a tag match (Direwolf's tuned value) */

void fx25_init(void);

/*
 * Build a full FX.25 RS(255,239) codeword (out_block[FX25_BLOCK_SIZE])
 * from a raw AX.25 frame (addresses..info, no FCS - same convention as
 * the KISS payload elsewhere in this codebase). Appends the FCS,
 * HDLC-stuffs it between flags, pads to the fixed block size with more
 * flag bytes, then appends the 16 Reed-Solomon check bytes.
 *
 * Returns 0 on success, -1 if the frame (after stuffing) doesn't fit in
 * a single 239-byte block - caller should fall back to plain AX.25.
 */
int fx25_build_block(const uint8_t *payload, size_t len, uint8_t out_block[FX25_BLOCK_SIZE]);

/*
 * Reed-Solomon correct a received 255-byte block in place, then extract
 * and FCS-verify the embedded AX.25 frame (FCS stripped from the
 * output, matching fx25_build_block's input convention).
 *
 * Returns the number of byte errors corrected (>= 0) on success, or -1
 * if the block is uncorrectable or no valid frame was found inside it.
 */
int fx25_decode_block(uint8_t block[FX25_BLOCK_SIZE], uint8_t *out_payload, size_t out_max, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* FX25_H_ */

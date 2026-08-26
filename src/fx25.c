#include "fx25.h"
#include "ax25.h"
#include <string.h>
#include <stdbool.h>

/*
 * Reed-Solomon RS(255,239) over GF(256), poly 0x11D, fcr=1, prim=1.
 * Straight port of Phil Karn's classic encode_rs_char/decode_rs_char
 * (as used by Direwolf's fx25_encode.c/fx25_extract.c), specialized to
 * this single fixed configuration: no malloc, no erasure support (the
 * demodulator never knows erasure positions), one hardcoded code rate.
 */
#define NN 255
#define NROOTS FX25_CHECK_SIZE
#define A0 NN
#define FCR 1
#define PRIM 1
#define IPRIM 1
#define GF_POLY 0x11D

static uint8_t alpha_to[NN + 1];
static uint8_t index_of[NN + 1];
static uint8_t genpoly[NROOTS + 1];
static bool rs_ready;

static inline int modnn(int x)
{
	while (x >= NN) {
		x -= NN;
		x = (x >> 8) + (x & NN);
	}
	return x;
}

static void rs_init(void)
{
	if (rs_ready) {
		return;
	}

	index_of[0] = A0;
	alpha_to[A0] = 0;
	int sr = 1;
	for (int i = 0; i < NN; i++) {
		index_of[sr] = i;
		alpha_to[i] = (uint8_t)sr;
		sr <<= 1;
		if (sr & 0x100) {
			sr ^= GF_POLY;
		}
		sr &= NN;
	}

	genpoly[0] = 1;
	for (int i = 0, root = FCR * PRIM; i < NROOTS; i++, root += PRIM) {
		genpoly[i + 1] = 1;
		for (int j = i; j > 0; j--) {
			if (genpoly[j] != 0) {
				genpoly[j] = genpoly[j - 1] ^ alpha_to[modnn(index_of[genpoly[j]] + root)];
			} else {
				genpoly[j] = genpoly[j - 1];
			}
		}
		genpoly[0] = alpha_to[modnn(index_of[genpoly[0]] + root)];
	}
	for (int i = 0; i <= NROOTS; i++) {
		genpoly[i] = index_of[genpoly[i]];
	}

	rs_ready = true;
}

void fx25_init(void)
{
	rs_init();
}

/* data[NN-NROOTS] in, parity[NROOTS] out */
static void rs_encode(const uint8_t *data, uint8_t *parity)
{
	memset(parity, 0, NROOTS);

	for (int i = 0; i < NN - NROOTS; i++) {
		uint8_t feedback = index_of[data[i] ^ parity[0]];
		if (feedback != A0) {
			for (int j = 1; j < NROOTS; j++) {
				parity[j] ^= alpha_to[modnn(feedback + genpoly[NROOTS - j])];
			}
		}
		memmove(&parity[0], &parity[1], NROOTS - 1);
		parity[NROOTS - 1] = (feedback != A0) ? alpha_to[modnn(feedback + genpoly[0])] : 0;
	}
}

/* Corrects data[NN] in place. Returns number of corrected symbols, or -1. */
static int rs_decode(uint8_t *data)
{
	int deg_lambda, el, deg_omega;
	int i, j, r, k;
	uint8_t q, tmp, num1, num2, den, discr_r;
	uint8_t lambda[NROOTS + 1], s[NROOTS];
	uint8_t b[NROOTS + 1], t[NROOTS + 1], omega[NROOTS + 1];
	uint8_t root[NROOTS], reg[NROOTS + 1], loc[NROOTS];
	int syn_error, count;

	/* Form the syndromes: evaluate data(x) at roots of g(x) */
	for (i = 0; i < NROOTS; i++) {
		s[i] = data[0];
	}
	for (j = 1; j < NN; j++) {
		for (i = 0; i < NROOTS; i++) {
			if (s[i] == 0) {
				s[i] = data[j];
			} else {
				s[i] = data[j] ^ alpha_to[modnn(index_of[s[i]] + (FCR + i) * PRIM)];
			}
		}
	}

	syn_error = 0;
	for (i = 0; i < NROOTS; i++) {
		syn_error |= s[i];
		s[i] = index_of[s[i]];
	}
	if (!syn_error) {
		/* Codeword as received - no errors */
		return 0;
	}

	memset(&lambda[1], 0, NROOTS * sizeof(lambda[0]));
	lambda[0] = 1;
	for (i = 0; i < NROOTS + 1; i++) {
		b[i] = index_of[lambda[i]];
	}

	/* Berlekamp-Massey to find the error locator polynomial */
	r = 0;
	el = 0;
	while (++r <= NROOTS) {
		discr_r = 0;
		for (i = 0; i < r; i++) {
			if (lambda[i] != 0 && s[r - i - 1] != A0) {
				discr_r ^= alpha_to[modnn(index_of[lambda[i]] + s[r - i - 1])];
			}
		}
		discr_r = index_of[discr_r];
		if (discr_r == A0) {
			memmove(&b[1], b, NROOTS * sizeof(b[0]));
			b[0] = A0;
		} else {
			t[0] = lambda[0];
			for (i = 0; i < NROOTS; i++) {
				t[i + 1] = (b[i] != A0) ? lambda[i + 1] ^ alpha_to[modnn(discr_r + b[i])] : lambda[i + 1];
			}
			if (2 * el <= r - 1) {
				el = r - el;
				for (i = 0; i <= NROOTS; i++) {
					b[i] = (lambda[i] == 0) ? A0 : modnn(index_of[lambda[i]] - discr_r + NN);
				}
			} else {
				memmove(&b[1], b, NROOTS * sizeof(b[0]));
				b[0] = A0;
			}
			memcpy(lambda, t, (NROOTS + 1) * sizeof(t[0]));
		}
	}

	deg_lambda = 0;
	for (i = 0; i < NROOTS + 1; i++) {
		lambda[i] = index_of[lambda[i]];
		if (lambda[i] != A0) {
			deg_lambda = i;
		}
	}

	/* Chien search for roots of the error locator polynomial */
	memcpy(&reg[1], &lambda[1], NROOTS * sizeof(reg[0]));
	count = 0;
	for (i = 1, k = IPRIM - 1; i <= NN; i++, k = modnn(k + IPRIM)) {
		q = 1;
		for (j = deg_lambda; j > 0; j--) {
			if (reg[j] != A0) {
				reg[j] = modnn(reg[j] + j);
				q ^= alpha_to[reg[j]];
			}
		}
		if (q != 0) {
			continue;
		}
		root[count] = i;
		loc[count] = k;
		if (++count == deg_lambda) {
			break;
		}
	}
	if (deg_lambda != count) {
		/* Uncorrectable: degree doesn't match number of roots found */
		return -1;
	}

	/* Forney: compute error evaluator poly and error values */
	deg_omega = 0;
	for (i = 0; i < NROOTS; i++) {
		tmp = 0;
		j = (deg_lambda < i) ? deg_lambda : i;
		for (; j >= 0; j--) {
			if (s[i - j] != A0 && lambda[j] != A0) {
				tmp ^= alpha_to[modnn(s[i - j] + lambda[j])];
			}
		}
		if (tmp != 0) {
			deg_omega = i;
		}
		omega[i] = index_of[tmp];
	}
	omega[NROOTS] = A0;

	for (j = count - 1; j >= 0; j--) {
		num1 = 0;
		for (i = deg_omega; i >= 0; i--) {
			if (omega[i] != A0) {
				num1 ^= alpha_to[modnn(omega[i] + i * root[j])];
			}
		}
		num2 = alpha_to[modnn(root[j] * (FCR - 1) + NN)];
		den = 0;
		for (i = (deg_lambda < NROOTS - 1 ? deg_lambda : NROOTS - 1) & ~1; i >= 0; i -= 2) {
			if (lambda[i + 1] != A0) {
				den ^= alpha_to[modnn(lambda[i + 1] + i * root[j])];
			}
		}
		if (den == 0) {
			return -1;
		}
		if (num1 != 0) {
			data[loc[j]] ^= alpha_to[modnn(index_of[num1] + index_of[num2] + NN - index_of[den])];
		}
	}

	return count;
}

#define FX25_FLAG 0x7E

/* Bit-stuff `in` (ilen bytes, includes FCS) between two flags and pad the
 * remainder of a 239-byte block with a phase-rotated flag pattern, exactly
 * mirroring Direwolf's stuff_it(). Returns 0, or -1 if it doesn't fit.
 */
static int stuff_frame(const uint8_t *in, size_t ilen, uint8_t out[FX25_DATA_SIZE])
{
	memset(out, 0, FX25_DATA_SIZE);
	size_t olen = 0;
	const size_t osize_bits = (size_t)FX25_DATA_SIZE * 8;
	int ones = 0;

#define PUT_BIT(v)                                                  \
	do {                                                          \
		if (olen >= osize_bits) {                              \
			return -1;                                     \
		}                                                      \
		if (v) {                                               \
			out[olen >> 3] |= (uint8_t)(1u << (olen & 7)); \
		}                                                      \
		olen++;                                                \
	} while (0)

	for (int b = 0; b < 8; b++) {
		PUT_BIT((FX25_FLAG >> b) & 1);
	}

	for (size_t i = 0; i < ilen; i++) {
		for (int b = 0; b < 8; b++) {
			int v = (in[i] >> b) & 1;
			PUT_BIT(v);
			if (v) {
				if (++ones == 5) {
					PUT_BIT(0);
					ones = 0;
				}
			} else {
				ones = 0;
			}
		}
	}

	for (int b = 0; b < 8; b++) {
		PUT_BIT((FX25_FLAG >> b) & 1);
	}

	uint8_t imask = 1;
	while (olen < osize_bits) {
		PUT_BIT(FX25_FLAG & imask);
		imask = (uint8_t)((imask << 1) | (imask >> 7));
	}

#undef PUT_BIT
	return 0;
}

int fx25_build_block(const uint8_t *payload, size_t len, uint8_t out_block[FX25_BLOCK_SIZE])
{
	if (payload == NULL || out_block == NULL || len == 0) {
		return -1;
	}

	rs_init();

	uint8_t fbuf[512 + 2];
	if (len + 2 > sizeof(fbuf)) {
		return -1;
	}
	memcpy(fbuf, payload, len);
	uint16_t fcs = ax25_calc_crc16(payload, len);
	fbuf[len] = fcs & 0xFF;
	fbuf[len + 1] = (fcs >> 8) & 0xFF;

	if (stuff_frame(fbuf, len + 2, out_block) != 0) {
		return -1; /* Frame too large for a single RS(255,239) block */
	}

	rs_encode(out_block, out_block + FX25_DATA_SIZE);
	return 0;
}

int fx25_decode_block(uint8_t block[FX25_BLOCK_SIZE], uint8_t *out_payload, size_t out_max, size_t *out_len)
{
	if (block == NULL || out_payload == NULL || out_len == NULL) {
		return -1;
	}

	rs_init();

	int corrected = rs_decode(block);
	if (corrected < 0) {
		return -1;
	}

	/* Destuff the embedded AX.25 frame out of the corrected data area,
	 * bit-packed into a scratch bitmap (not a bit-per-byte array) to
	 * keep this off the RX thread's stack budget.
	 */
	uint8_t bitbuf[FX25_DATA_SIZE];
	memset(bitbuf, 0, sizeof(bitbuf));
	size_t nbits = 0;
	uint8_t shift_reg = 0;
	bool in_frame = false;
	bool found_end = false;
	int ones = 0;

	for (size_t i = 0; i < (size_t)FX25_DATA_SIZE * 8; i++) {
		uint8_t bit = (block[i >> 3] >> (i & 7)) & 1;
		shift_reg = (uint8_t)((shift_reg << 1) | bit);

		if (shift_reg == FX25_FLAG) {
			if (in_frame && nbits >= 7 + 16) {
				nbits -= 7; /* drop the closing flag's already-pushed leading bits */
				found_end = true;
				break;
			}
			in_frame = true;
			nbits = 0;
			ones = 0;
			memset(bitbuf, 0, sizeof(bitbuf));
			continue;
		}

		if (!in_frame) {
			continue;
		}

		if (bit) {
			if (++ones >= 7) {
				in_frame = false;
				nbits = 0;
				continue;
			}
			bitbuf[nbits >> 3] |= (uint8_t)(1u << (nbits & 7));
			nbits++;
		} else {
			if (ones == 5) {
				ones = 0;
				continue; /* stuffed zero - discard */
			}
			ones = 0;
			nbits++;
		}
	}

	if (!found_end) {
		return -1;
	}

	size_t nbytes = nbits / 8;
	if (nbytes < 3) {
		return -1;
	}

	uint8_t frame_bytes[FX25_DATA_SIZE];
	for (size_t i = 0; i < nbytes; i++) {
		uint8_t v = 0;
		for (int b = 0; b < 8; b++) {
			size_t bit_pos = i * 8 + (size_t)b;
			if (bitbuf[bit_pos >> 3] & (1u << (bit_pos & 7))) {
				v |= (uint8_t)(1u << b);
			}
		}
		frame_bytes[i] = v;
	}

	size_t payload_len = nbytes - 2;
	uint16_t rx_fcs = frame_bytes[payload_len] | ((uint16_t)frame_bytes[payload_len + 1] << 8);
	uint16_t calc_fcs = ax25_calc_crc16(frame_bytes, payload_len);
	if (rx_fcs != calc_fcs || payload_len > out_max) {
		return -1;
	}

	memcpy(out_payload, frame_bytes, payload_len);
	*out_len = payload_len;
	return corrected;
}

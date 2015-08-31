/*
 * hc_matchfinder.h
 *
 * Author:	Eric Biggers
 * Year:	2014, 2015
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 *
 * ---------------------------------------------------------------------------
 *
 *				   Algorithm
 *
 * This is a Hash Chains (hc) based matchfinder.
 *
 * The data structure is a hash table where each hash bucket contains a linked
 * list (or "chain") of sequences whose first 3 bytes share the same hash code.
 * Each sequence is identified by its starting position in the input buffer.
 *
 * The algorithm processes the input buffer sequentially.  At each byte
 * position, the hash code of the first 3 bytes of the sequence beginning at
 * that position (the sequence being matched against) is computed.  This
 * identifies the hash bucket to use for that position.  Then, this hash
 * bucket's linked list is searched for matches.  Then, a new linked list node
 * is created to represent the current sequence and is prepended to the list.
 *
 * This algorithm has several useful properties:
 *
 * - It only finds true Lempel-Ziv matches; i.e., those where the matching
 *   sequence occurs prior to the sequence being matched against.
 *
 * - The sequences in each linked list are always sorted by decreasing starting
 *   position.  Therefore, the closest (smallest offset) matches are found
 *   first, which in many compression formats tend to be the cheapest to encode.
 *
 * - Although fast running time is not guaranteed due to the possibility of the
 *   lists getting very long, the worst degenerate behavior can be easily
 *   prevented by capping the number of nodes searched at each position.
 *
 * - If the compressor decides not to search for matches at a certain position,
 *   then that position can be quickly inserted without searching the list.
 *
 * - The algorithm is adaptable to sliding windows: just store the positions
 *   relative to a "base" value that is updated from time to time, and stop
 *   searching each list when the sequences get too far away.
 *
 * ---------------------------------------------------------------------------
 *
 *				Notes on usage
 *
 * You must define MATCHFINDER_MAX_WINDOW_ORDER before including this header
 * because that determines which integer type to use for positions.  Since
 * 16-bit integers are faster than 32-bit integers due to reduced memory usage
 * (and therefore reduced cache pressure), the code only uses 32-bit integers if
 * they are needed to represent all possible positions.
 *
 * You must allocate the 'struct hc_matchfinder' on a
 * MATCHFINDER_ALIGNMENT-aligned boundary, and its necessary allocation size
 * must be gotten by calling hc_matchfinder_size().
 *
 * ----------------------------------------------------------------------------
 *
 *				 Optimizations
 *
 * The longest_match() and skip_positions() functions are inlined into the
 * compressors that use them.  This isn't just about saving the overhead of a
 * function call.  These functions are intended to be called from the inner
 * loops of compressors, where giving the compiler more control over register
 * allocation is very helpful.  There is also significant benefit to be gained
 * from allowing the CPU to predict branches independently at each call site.
 * For example, "lazy"-style compressors can be written with two calls to
 * longest_match(), each of which starts with a different 'best_len' and
 * therefore has significantly different performance characteristics.
 *
 * Although any hash function can be used, a multiplicative hash is fast and
 * works well.
 *
 * On some processors, it is significantly faster to extend matches by whole
 * words (32 or 64 bits) instead of by individual bytes.  For this to be the
 * case, the processor must implement unaligned memory accesses efficiently and
 * must have either a fast "find first set bit" instruction or a fast "find last
 * set bit" instruction, depending on the processor's endianness.
 *
 * The code uses one loop for finding the first match and one loop for finding a
 * longer match.  Each of these loops is tuned for its respective task and in
 * combination are faster than a single generalized loop that handles both
 * tasks.
 *
 * The code also uses a tight inner loop that only compares the last and first
 * bytes of a potential match.  It is only when these bytes match that a full
 * match extension is attempted.
 *
 * ----------------------------------------------------------------------------
 */

#ifndef _HC_MATCHFINDER_H
#define _HC_MATCHFINDER_H

#include "wimlib/lz_extend.h"
#include "wimlib/lz_hash.h"
#include "wimlib/matchfinder_common.h"
#include "wimlib/unaligned.h"

#define HC_MF_HASH3_ORDER	14
#define HC_MF_HASH4_ORDER	15

#define HC_MF_HASH3_BUCKETS	(1UL << HC_MF_HASH3_ORDER)
#define HC_MF_HASH4_BUCKETS	(1UL << HC_MF_HASH4_ORDER)

struct hc_matchfinder {
	pos_t hash3_tab[HC_MF_HASH3_BUCKETS];
	pos_t hash4_tab[HC_MF_HASH4_BUCKETS];
	pos_t next_tab[];
} _aligned_attribute(MATCHFINDER_ALIGNMENT);

/* Return the number of bytes that must be allocated for a 'hc_matchfinder' that
 * can work with buffers up to the specified size.  */
static inline size_t
hc_matchfinder_size(size_t max_bufsize)
{
	return sizeof(struct hc_matchfinder) + (max_bufsize * sizeof(pos_t));
}

/* Prepare the matchfinder for a new input buffer.  */
static inline void
hc_matchfinder_init(struct hc_matchfinder *mf)
{
	memset(mf, 0, sizeof(struct hc_matchfinder));
}

/*
 * Find the longest match longer than 'best_len' bytes.
 *
 * @mf
 *	The matchfinder structure.
 * @in_begin
 *	Pointer to the beginning of the input buffer.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.  This is the
 *	pointer to the sequence being matched against.
 * @best_len
 *	Require a match longer than this length.
 * @max_len
 *	The maximum permissible match length at this position.
 * @nice_len
 *	Stop searching if a match of at least this length is found.
 *	Must be <= @max_len.
 * @max_search_depth
 *	Limit on the number of potential matches to consider.  Must be >= 1.
 * @next_hashes
 *	The precomputed hashcodes for the sequences beginning at @in_next.  This
 *	will be updated with the precomputed hashcodes for the sequences
 *	beginning at @in_next + 1.
 * @offset_ret
 *	If a match is found, its offset is returned in this location.
 *
 * Return the length of the match found, or 'best_len' if no match longer than
 * 'best_len' was found.
 */
static inline u32
hc_matchfinder_longest_match(struct hc_matchfinder * const restrict mf,
			     const u8 * const restrict in_begin,
			     const ptrdiff_t cur_pos,
			     u32 best_len,
			     const u32 max_len,
			     const u32 nice_len,
			     const u32 max_search_depth,
			     u32 next_hashes[const restrict static 2],
			     u32 * const restrict offset_ret)
{
	/*compiler_hint(cur_pos >= 0 && cur_pos <= UINT32_MAX);*/
	/*compiler_hint(nice_len <= max_len);*/
	/*compiler_hint(max_search_depth > 0);*/

	const u8 *in_next = in_begin + cur_pos;
	u32 depth_remaining = max_search_depth;
	const u8 *best_matchptr = best_matchptr; /* uninitialized */
	u32 next_seq3, next_seq4;
	u32 hash3, hash4;
	u32 seq4;
	pos_t cur_node3;
	pos_t cur_node4;
	const u8 *matchptr;
	u32 len;

	if (unlikely(max_len < 5))
		goto out;

	hash3 = next_hashes[0];
	hash4 = next_hashes[1];

	cur_node3 = mf->hash3_tab[hash3];
	cur_node4 = mf->hash4_tab[hash4];

	mf->hash3_tab[hash3] = cur_pos;
	mf->hash4_tab[hash4] = cur_pos;
	mf->next_tab[cur_pos] = cur_node4;

	next_seq4 = load_u32_unaligned(in_next + 1);
	next_seq3 = loaded_u32_to_u24(next_seq4);
	next_hashes[0] = lz_hash(next_seq3, HC_MF_HASH3_ORDER);
	next_hashes[1] = lz_hash(next_seq4, HC_MF_HASH4_ORDER);
	prefetchw(&mf->hash3_tab[next_hashes[0]]);
	prefetchw(&mf->hash4_tab[next_hashes[1]]);

	if (best_len < 4) {

		if (!matchfinder_node_valid(cur_node3))
			goto out;

		seq4 = load_u32_unaligned(in_next);

		if (best_len < 3) {
			matchptr = &in_begin[cur_node3];
			if (load_u24_unaligned(matchptr) == loaded_u32_to_u24(seq4)) {
				best_len = 3;
				best_matchptr = matchptr;
			}
		}

		if (!matchfinder_node_valid(cur_node4))
			goto out;

		for (;;) {
			/* No length 4 match found yet.  Check the first 4 bytes.  */
			matchptr = &in_begin[cur_node4];

			if (load_u32_unaligned(matchptr) == seq4)
				break;

			/* The first 4 bytes did not match.  Keep trying.  */
			cur_node4 = mf->next_tab[cur_node4];
			if (!matchfinder_node_valid(cur_node4) || !--depth_remaining)
				goto out;
		}

		/* Found a match of length >= 4.  Extend it to its full length.  */
		best_matchptr = matchptr;
		best_len = lz_extend(in_next, best_matchptr, 4, max_len);
		if (best_len >= nice_len)
			goto out;
		cur_node4 = mf->next_tab[cur_node4];
		if (!matchfinder_node_valid(cur_node4) || !--depth_remaining)
			goto out;
	} else {
		if (!matchfinder_node_valid(cur_node4) || best_len >= nice_len)
			goto out;
	}

	for (;;) {
		for (;;) {
			matchptr = &in_begin[cur_node4];

			/* Already found a length 4 match.  Try for a longer
			 * match; start by checking either the last 4 bytes and
			 * the first 4 bytes, or the last byte.  (The last byte,
			 * the one which would extend the match length by 1, is
			 * the most important.)  */
		#if UNALIGNED_ACCESS_IS_FAST
			if ((load_u32_unaligned(matchptr + best_len - 3) ==
			     load_u32_unaligned(in_next + best_len - 3)) &&
			    (load_u32_unaligned(matchptr) ==
			     load_u32_unaligned(in_next)))
		#else
			if (matchptr[best_len] == in_next[best_len])
		#endif
				break;

			cur_node4 = mf->next_tab[cur_node4];
			if (!matchfinder_node_valid(cur_node4) || !--depth_remaining)
				goto out;
		}

	#if UNALIGNED_ACCESS_IS_FAST
		len = 4;
	#else
		len = 0;
	#endif
		len = lz_extend(in_next, matchptr, len, max_len);
		if (len > best_len) {
			best_len = len;
			best_matchptr = matchptr;
			if (best_len >= nice_len)
				goto out;
		}
		cur_node4 = mf->next_tab[cur_node4];
		if (!matchfinder_node_valid(cur_node4) || !--depth_remaining)
			goto out;
	}
out:
	*offset_ret = in_next - best_matchptr;
	return best_len;
}

/*
 * Advance the matchfinder, but don't search for matches.
 *
 * @mf
 *	The matchfinder structure.
 * @in_begin
 *	Pointer to the beginning of the input buffer.
 * @in_next
 *	Pointer to the next byte in the input buffer to process.
 * @in_end
 *	Pointer to the end of the input buffer.
 * @next_hashes
 *	The precomputed hashcodes for the sequences beginning at @in_next.  This
 *	will be updated with the precomputed hashcodes for the sequences
 *	beginning at @in_next + @count.
 * @count
 *	The number of bytes to advance.  Must be > 0.
 *
 * Returns @in_next + @count.
 */
static inline const u8 *
hc_matchfinder_skip_positions(struct hc_matchfinder * const restrict mf,
			      const u8 * const restrict in_begin,
			      const ptrdiff_t cur_pos,
			      const ptrdiff_t end_pos,
			      const u32 count,
			      u32 next_hashes[const restrict static 2])
{
	/*compiler_hint(cur_pos >= 0 && cur_pos <= UINT32_MAX);*/
	/*compiler_hint(end_pos >= 0 && end_pos <= UINT32_MAX);*/
	/*compiler_hint(cur_pos + count <= end_pos);*/
	/*compiler_hint(count > 0);*/

	const u8 *in_next = in_begin + cur_pos;
	const u8 * const stop_ptr = in_next + count;

	if (likely(stop_ptr <= in_begin + end_pos - 5)) {
		u32 hash3, hash4;
		u32 next_seq3, next_seq4;

		hash3 = next_hashes[0];
		hash4 = next_hashes[1];
		do {
			mf->hash3_tab[hash3] = in_next - in_begin;
			mf->next_tab[in_next - in_begin] = mf->hash4_tab[hash4];
			mf->hash4_tab[hash4] = in_next - in_begin;

			next_seq4 = load_u32_unaligned(in_next + 1);
			next_seq3 = loaded_u32_to_u24(next_seq4);
			hash3 = lz_hash(next_seq3, HC_MF_HASH3_ORDER);
			hash4 = lz_hash(next_seq4, HC_MF_HASH4_ORDER);

		} while (++in_next != stop_ptr);

		prefetchw(&mf->hash3_tab[hash3]);
		prefetchw(&mf->hash4_tab[hash4]);
		next_hashes[0] = hash3;
		next_hashes[1] = hash4;
	}

	return stop_ptr;
}

#endif /* _HC_MATCHFINDER_H */

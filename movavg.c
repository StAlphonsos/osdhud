/*
 * Copyright (C) 2015,2016 by attila <attila@stalphonsos.com>
 *
 * Permission to use, copy, modify, and distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Maintain moving averages.
 */

#include <assert.h>
#include <stdlib.h>
#include "movavg.h"

/*
 * Allocate a new moving average
 */
struct movavg *
movavg_new(int wsize)
{
	struct movavg *ma;

	assert((wsize > 1) || (wsize <= MAX_WSIZE));
	ma = malloc(sizeof(*ma));
	assert(ma);
	ma->window_size = wsize;
	ma->off = ma->count = 0;
	ma->sum = 0;
	ma->window = (float *)calloc(wsize,sizeof(float));
	assert(ma->window);
	return ma;
}

/*
 * Tear down a moving average
 */
void
movavg_free(struct movavg *ma)
{
	if (ma)
		free(ma->window);
	free(ma);
}

/*
 * Reset a moving average to its initial state (all zeroes)
 */
void
movavg_clear(struct movavg *ma)
{
	if (ma) {
		int i;

		for (i = 0; i < ma->window_size; i++)
			ma->window[i] = 0;
		ma->off = ma->count = 0;
		ma->sum = 0;
	}
}

/*
 * Add a new value to the moving average.  Returns the moving average
 * value after accounting for the new value.  If the window is full
 * the oldest value is (re)moved.
 */
float
movavg_add(struct movavg *ma, float val)
{
	float result = 0;

	if (ma) {
		if (ma->count < ma->window_size)
			ma->count++;
		else {
			ma->off %= ma->window_size;
			ma->sum -= ma->window[ma->off]; /* the "moving" part */
		}
		ma->sum += val;
		ma->window[ma->off++] = val;
		assert(ma->count);
		result = ma->sum / ma->count;
	}
	return result;
}

/*
 * Return the current value of the moving average.
 */
float
movavg_val(struct movavg *ma)
{
	return (ma && ma->count) ? (ma->sum / ma->count) : 0;
}

/*
 * Local variables:
 * mode: c
 * c-file-style: "bsd"
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 */

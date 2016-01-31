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
 * A moving average
 */
struct movavg {
	float	*window;		/* data window storage */
	int	 count;			/* #of valid entries in window */
	float	 sum;			/* up-to-date sum of valid entries */
	int	 off;			/* write offset into ->window[] */
	int	 window_size;		/* count must always be <= this */
};

#define MAX_WSIZE 10000	       /* max size of moving average window */

/*
 * API
 */
struct movavg *movavg_new(int);
void movavg_free(struct movavg *);
void movavg_clear(struct movavg *);
float movavg_add(struct movavg *, float);
float movavg_val(struct movavg *);

/*
 * Local variables:
 * mode: c
 * c-file-style: "bsd"
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 */

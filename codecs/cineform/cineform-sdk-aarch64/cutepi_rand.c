/* CuTePi: the SDK dithers its output with libc rand(), whose state is global and advances on every call: a frame's
 * dither (+-1 in ~1% of samples) depended on how many frames, and which threads, had drawn numbers before it, so
 * output varied from run to run and between decoders. The Makefile maps the SDK's rand() here (-Drand=cutepi_rand):
 * a per-thread generator reset at the start of every sample (cutepi_rand_reset in DecodeSample), so each frame's
 * dither depends only on that frame. Same LCG constants as glibc's TYPE_0 rand. */
static __thread unsigned int state = 1;

int cutepi_rand(void)
{
	state = state * 1103515245u + 12345u;
	return (int)((state >> 16) & 0x7fff);
}

void cutepi_rand_reset(void)
{
	state = 1;
}

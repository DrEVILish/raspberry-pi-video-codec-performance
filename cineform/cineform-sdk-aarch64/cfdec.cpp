// cfdec FILE.MOV OUT.yuy2 [frames] [threads]: decode a CineForm MOV with the CineForm SDK to raw YUY2 (full
// resolution) and print the average decode time. For checking a port against the reference x86 build.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "CFHDDecoder.h"
#include "CFHDMetadata.h"
#include "mp4reader.h"
#define MOV_TRAK_TYPE  MAKEID('v', 'i', 'd', 'e')
#define MOV_TRAK_SUBTYPE MAKEID('C', 'F', 'H', 'D')
static double now() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: cfdec FILE.MOV OUT.yuy2 [frames] [threads]\n"); return 2; }
	void *h = OpenMP4Source(argv[1], MOV_TRAK_TYPE, MOV_TRAK_SUBTYPE);
	uint32_t n = GetNumberPayloads(h), limit = argc > 3 ? atoi(argv[3]) : n; if (limit < n) n = limit;
	unsigned int cpus = argc > 4 ? atoi(argv[4]) : 16;
	CFHD_DecoderRef dec = NULL; CFHD_MetadataRef md = NULL;
	if (CFHD_OpenDecoder(&dec, NULL) || CFHD_OpenMetadata(&md)) { fprintf(stderr, "open failed\n"); return 1; }
	FILE *out = fopen(argv[2], "wb");
	uint32_t *payload = NULL; void *buf = NULL; int w = 0, hgt = 0, pitch = 0; CFHD_PixelFormat fmt;
	double tot = 0; int bad = 0;
	for (uint32_t f = 0; f < n; f++) {
		uint32_t sz = GetPayloadSize(h, f); payload = GetPayload(h, payload, f);
		if (!buf) {
			if (CFHD_PrepareToDecode(dec, 0, 0, CFHD_PIXEL_FORMAT_YUY2, CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
					payload, sz, &w, &hgt, &fmt)) { fprintf(stderr, "prepare failed\n"); return 1; }
			CFHD_InitSampleMetadata(md, METADATATYPE_ORIGINAL, payload, sz);
			CFHD_SetActiveMetadata(dec, md, TAG_CPU_MAX, METADATATYPE_UINT32, &cpus, sizeof cpus);
			CFHD_GetImagePitch(w, fmt, &pitch);
			buf = aligned_alloc(64, (size_t)pitch * hgt);
			fprintf(stderr, "%s: %u frames, %dx%d YUY2 pitch %d, %u threads\n", argv[1], n, w, hgt, pitch, cpus);
		}
		double t = now();
		CFHD_Error e = CFHD_DecodeSample(dec, payload, sz, buf, pitch);
		tot += now() - t;
		if (e) { fprintf(stderr, "decode error %d at frame %u (black frame written)\n", e, f); memset(buf, 0, (size_t)pitch * hgt); bad++; }
		for (int y = 0; y < hgt; y++) fwrite((char *)buf + (size_t)y * pitch, 1, (size_t)w * 2, out);
	}
	fclose(out);
	printf("%s: %u frames (%d undecodable), average decode %.2f ms (%.1f fps)\n", argv[1], n, bad, tot / n * 1e3, n / tot);
	return 0;
}

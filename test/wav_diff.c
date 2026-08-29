/*
 * Compare two 16-bit WAVs and report how far apart they are.
 *
 * Exists so the OPL3 silent-slot optimisation's fidelity claim is checked by
 * the build rather than asserted in a comment: `make -C picosdl/test opl3`
 * renders the same tune with the optimisation on and off and runs this over the
 * two files.
 *
 *   wav_diff a.wav b.wav [min_dB]
 *
 * Exits non-zero if the difference rejection is below min_dB (default 60),
 * i.e. if the optimisation has started changing the sound audibly.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Walk the RIFF chunks to find `data` - not every writer puts it at offset 44. */
static short *load_wav(const char *path, long *count)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "wav_diff: cannot open %s\n", path);
        return NULL;
    }

    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "wav_diff: %s is not a WAV\n", path);
        fclose(f);
        return NULL;
    }

    for (;;) {
        char id[4];
        unsigned size;
        if (fread(id, 1, 4, f) != 4 || fread(&size, 4, 1, f) != 1)
            break;
        if (memcmp(id, "data", 4) == 0) {
            short *pcm = malloc(size);
            if (pcm == NULL || fread(pcm, 1, size, f) != size) {
                fprintf(stderr, "wav_diff: short read on %s\n", path);
                free(pcm);
                fclose(f);
                return NULL;
            }
            *count = (long)(size / sizeof(short));
            fclose(f);
            return pcm;
        }
        fseek(f, (long)size + (size & 1), SEEK_CUR);
    }

    fprintf(stderr, "wav_diff: no data chunk in %s\n", path);
    fclose(f);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s a.wav b.wav [min_dB]\n", argv[0]);
        return 2;
    }
    double min_db = argc > 3 ? atof(argv[3]) : 60.0;

    long na = 0, nb = 0;
    short *a = load_wav(argv[1], &na);
    short *b = load_wav(argv[2], &nb);
    if (a == NULL || b == NULL)
        return 2;

    if (na != nb)
        printf("note: lengths differ (%ld vs %ld samples), comparing the overlap\n",
               na, nb);
    long n = na < nb ? na : nb;

    double  sig = 0, err = 0;
    long    differing = 0;
    int     worst = 0;
    double  peak = 0;
    for (long i = 0; i < n; ++i) {
        double s = a[i], d = (double)a[i] - b[i];
        sig += s * s;
        err += d * d;
        if (d != 0) {
            differing++;
            if (fabs(d) > worst)
                worst = (int)fabs(d);
        }
        if (fabs(s) > peak)
            peak = fabs(s);
    }

    double rejection = err > 0 ? 10.0 * log10(sig / err) : 999.0;

    printf("%ld samples compared\n", n);
    printf("  differing      %ld (%.1f%%)\n", differing, 100.0 * differing / n);
    printf("  worst error    %d LSB, against a peak of %.0f and RMS of %.0f\n",
           worst, peak, sqrt(sig / n));
    printf("  rejection      %.1f dB (threshold %.0f dB)\n", rejection, min_db);

    free(a);
    free(b);

    if (rejection < min_db) {
        printf("FAIL: the two renderings differ audibly\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}

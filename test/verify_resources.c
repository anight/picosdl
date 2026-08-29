/*
 * End-to-end check of the build-time sprite conversion.
 *
 * Every sprite in the game now lives in flash as 8bpp indexed into one global
 * 256-colour palette, with the sprite set's palette row baked into each pixel.
 * That is four separate things that can be subtly wrong - the row offset, the
 * palette contents, the 1bpp/4bpp unpacking, and the BMP's bottom-up row order -
 * and all four produce an image that still looks like a sprite.
 *
 * So: run each sprite through the real path. Load its set's palette into the
 * CLUT the way the game will, blit the const flash surface onto a scratch
 * surface with picosdl's actual blitter, map the result back through the
 * palette, and compare against a reference built from the original BMPs by a
 * completely different route (dump_reference.py).
 *
 *   python3 PR/src/bin/dump_reference.py
 *   make -C picosdl/test verify
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psdl_internal.h"
#include "resources.h"

#define REFERENCE_PATH "../../PR/src/bin/resources/reference.bin"

static int checked, mismatched, missing;

/* The scratch surface everything is blitted onto. Bigger than any sprite. */
#define SCRATCH_W 512
#define SCRATCH_H 512

static SDL_Surface *scratch;

static const struct sprite_set_s *find_set(const char *datfile, int shpl_id)
{
	return resources_find_sprite_set(datfile, shpl_id);
}

/* Load a set's sixteen colours into its row, exactly as the game will. */
static void load_set_palette(const struct sprite_set_s *set)
{
	const unsigned char (*src)[3] = set->palette;

	/* The guard sets carry an all-black shpl palette; the game colours them
	 * from PRINCE.DAT resource 10 instead. Substitute the first variant, or the
	 * comparison would be black-against-black and would prove nothing about
	 * the indices. dump_reference.py substitutes the same one. */
	int all_black = 1;
	for (int i = 0; i < 16 && all_black; ++i)
		if (set->palette[i][0] || set->palette[i][1] || set->palette[i][2])
			all_black = 0;
	if (all_black && resources_guard_palette_count > 0)
		src = resources_guard_palettes[0];

	SDL_Color colors[16];
	for (int i = 0; i < 16; ++i) {
		colors[i].r = src[i][0];
		colors[i].g = src[i][1];
		colors[i].b = src[i][2];
		colors[i].a = SDL_ALPHA_OPAQUE;
	}
	SDL_SetPaletteColors(PSDL_GlobalPalette(), colors, set->palette_row * 16, 16);
}

static int check_one(const struct sprite_set_s *set, unsigned index,
                     int w, int h, const unsigned char *want)
{
	if (index >= set->n_images || set->images[index] == NULL) {
		printf("  MISSING %s res%d image %u\n", set->datfile,
		       set->shpl_resource_id, index);
		return 0;
	}

	const SDL_Surface *src = set->images[index];

	if (src->w != w || src->h != h) {
		printf("  SIZE    %s res%d image %u: have %dx%d, want %dx%d\n",
		       set->datfile, set->shpl_resource_id, index, src->w, src->h, w, h);
		return 0;
	}

	/* The colour key must be this set's row base, or transparency picks the
	 * wrong index once the sprite is drawn over something. */
	if (src->colorkey != (Uint32)(set->palette_row * 16)) {
		printf("  KEY     %s res%d image %u: key %u, row base %u\n",
		       set->datfile, set->shpl_resource_id, index,
		       (unsigned)src->colorkey, (unsigned)(set->palette_row * 16));
		return 0;
	}

	/* Blit opaquely, so transparent pixels come through as the row base and
	 * get compared too - a wrong base would otherwise hide inside them. */
	SDL_FillRect(scratch, NULL, 0);
	SDL_Rect at = { 0, 0, 0, 0 };
	PSDL_BlitTransp((SDL_Surface *)src, NULL, scratch, &at, 0);

	const SDL_Color *pal = PSDL_GlobalPalette()->colors;
	for (int y = 0; y < h; ++y) {
		const Uint8 *row = (const Uint8 *)scratch->pixels + (size_t)y * scratch->pitch;
		for (int x = 0; x < w; ++x) {
			const SDL_Color *c = &pal[row[x]];
			const unsigned char *e = want + ((size_t)y * w + x) * 3;
			if (c->r != e[0] || c->g != e[1] || c->b != e[2]) {
				printf("  PIXEL   %s res%d image %u at (%d,%d): "
				       "index %u -> (%u,%u,%u), want (%u,%u,%u)\n",
				       set->datfile, set->shpl_resource_id, index, x, y,
				       row[x], c->r, c->g, c->b, e[0], e[1], e[2]);
				return 0;
			}
		}
	}
	return 1;
}

int main(void)
{
	SDL_Init(SDL_INIT_VIDEO);

	scratch = SDL_CreateRGBSurface(0, SCRATCH_W, SCRATCH_H, 8, 0, 0, 0, 0);
	if (scratch == NULL) {
		printf("could not create the scratch surface: %s\n", SDL_GetError());
		return 1;
	}

	FILE *fp = fopen(REFERENCE_PATH, "rb");
	if (fp == NULL) {
		printf("cannot open %s - run PR/src/bin/dump_reference.py first\n",
		       REFERENCE_PATH);
		return 1;
	}

	Uint32 count = 0;
	if (fread(&count, 4, 1, fp) != 1) {
		printf("short reference file\n");
		return 1;
	}
	printf("checking %u sprites against %s\n", (unsigned)count, REFERENCE_PATH);

	unsigned char *buf = malloc(SCRATCH_W * SCRATCH_H * 3);
	const struct sprite_set_s *cur = NULL;

	for (Uint32 n = 0; n < count; ++n) {
		Uint32 shpl_id, index, w, h;
		char   datfile[16];
		if (fread(&shpl_id, 4, 1, fp) != 1 || fread(&index, 4, 1, fp) != 1 ||
		    fread(&w, 4, 1, fp) != 1 || fread(&h, 4, 1, fp) != 1 ||
		    fread(datfile, 16, 1, fp) != 1) {
			printf("truncated reference at record %u\n", (unsigned)n);
			return 1;
		}
		size_t bytes = (size_t)w * h * 3;
		if (fread(buf, 1, bytes, fp) != bytes) {
			printf("truncated pixels at record %u\n", (unsigned)n);
			return 1;
		}

		const struct sprite_set_s *set = find_set(datfile, (int)shpl_id);
		if (set == NULL) {
			printf("  NO SET  %s res%u\n", datfile, (unsigned)shpl_id);
			missing++;
			continue;
		}

		/* Rows are shared between sets, so the palette has to be reloaded
		 * whenever the set changes - as it will be in the game. */
		if (set != cur) {
			load_set_palette(set);
			cur = set;
		}

		checked++;
		if (!check_one(set, index, (int)w, (int)h, buf))
			mismatched++;
	}

	fclose(fp);
	free(buf);

	printf("\n%d sprites checked, %d mismatched, %d with no sprite set\n",
	       checked, mismatched, missing);

	/* Every surface must be const-in-flash, or it is not doing its job. */
	int not_flash = 0, total = 0;
	for (unsigned i = 0; i < sprite_sets_count; ++i) {
		for (unsigned j = 0; j < sprite_sets[i].n_images; ++j) {
			const SDL_Surface *s = sprite_sets[i].images[j];
			if (s == NULL)
				continue;
			total++;
			if ((s->flags & PSDL_SURF_CONST) == 0)
				not_flash++;
		}
	}
	printf("%d surfaces, %d not marked const-in-flash\n", total, not_flash);

	/* And freeing one must be a no-op rather than a write to XIP. */
	const SDL_Surface *probe = NULL;
	for (unsigned i = 0; i < sprite_sets_count && probe == NULL; ++i)
		for (unsigned j = 0; j < sprite_sets[i].n_images && probe == NULL; ++j)
			probe = sprite_sets[i].images[j];
	if (probe != NULL) {
		int before = probe->refcount;
		SDL_FreeSurface((SDL_Surface *)probe);
		SDL_FreeSurface((SDL_Surface *)probe);
		if (probe->refcount != before) {
			printf("FAIL: SDL_FreeSurface wrote to a flash surface\n");
			mismatched++;
		} else {
			printf("SDL_FreeSurface on a flash surface: no write, as required\n");
		}
	}

	return (mismatched || missing || not_flash) ? 1 : 0;
}

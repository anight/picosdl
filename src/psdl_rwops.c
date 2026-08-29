/*
 * RWops.
 *
 * The memory flavour is real and is all the game needs to read resources, which
 * on this target are const arrays in flash. The file flavour is where saved
 * games and the hall of fame would go; there is no filesystem yet, so it fails
 * cleanly rather than pretending.
 *
 * A small fixed pool, like everything else here.
 */
#include "psdl_internal.h"

#ifndef PSDL_MAX_RWOPS
#define PSDL_MAX_RWOPS 4
#endif

static SDL_RWops s_pool[PSDL_MAX_RWOPS];
static Uint8     s_pool_used[PSDL_MAX_RWOPS];

#define PSDL_RWOPS_MEMORY 1

static SDL_RWops *rwops_alloc(void)
{
	for (int i = 0; i < PSDL_MAX_RWOPS; ++i) {
		if (s_pool_used[i])
			continue;
		s_pool_used[i] = 1;
		memset(&s_pool[i], 0, sizeof(s_pool[i]));
		return &s_pool[i];
	}
	SDL_SetError("picosdl: out of RWops - raise PSDL_MAX_RWOPS");
	return NULL;
}

static Sint64 mem_size(SDL_RWops *ctx)
{
	return (Sint64)(ctx->mem.stop - ctx->mem.base);
}

static Sint64 mem_seek(SDL_RWops *ctx, Sint64 offset, int whence)
{
	Uint8 *pos;
	switch (whence) {
	case RW_SEEK_SET: pos = ctx->mem.base + offset; break;
	case RW_SEEK_CUR: pos = ctx->mem.here + offset; break;
	case RW_SEEK_END: pos = ctx->mem.stop + offset; break;
	default:
		SDL_SetError("picosdl: bad whence %d", whence);
		return -1;
	}
	if (pos < ctx->mem.base) pos = ctx->mem.base;
	if (pos > ctx->mem.stop) pos = ctx->mem.stop;
	ctx->mem.here = pos;
	return (Sint64)(pos - ctx->mem.base);
}

static size_t mem_read(SDL_RWops *ctx, void *ptr, size_t size, size_t maxnum)
{
	if (size == 0 || maxnum == 0)
		return 0;
	size_t available = (size_t)(ctx->mem.stop - ctx->mem.here) / size;
	if (maxnum > available)
		maxnum = available;
	if (maxnum == 0)
		return 0;
	memcpy(ptr, ctx->mem.here, maxnum * size);
	ctx->mem.here += maxnum * size;
	return maxnum;
}

static size_t mem_write(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
	if (size == 0 || num == 0)
		return 0;
	size_t room = (size_t)(ctx->mem.stop - ctx->mem.here) / size;
	if (num > room)
		num = room;
	if (num == 0)
		return 0;
	memcpy(ctx->mem.here, ptr, num * size);
	ctx->mem.here += num * size;
	return num;
}

static size_t mem_write_ro(SDL_RWops *ctx, const void *ptr, size_t size, size_t num)
{
	(void)ctx; (void)ptr; (void)size; (void)num;
	SDL_SetError("picosdl: RWops is read-only");
	return 0;
}

static int mem_close(SDL_RWops *ctx)
{
	for (int i = 0; i < PSDL_MAX_RWOPS; ++i) {
		if (&s_pool[i] == ctx) {
			s_pool_used[i] = 0;
			return 0;
		}
	}
	return 0;
}

static SDL_RWops *rwops_from_range(Uint8 *base, int size, int writable)
{
	SDL_RWops *rw = rwops_alloc();
	if (rw == NULL)
		return NULL;
	rw->type  = PSDL_RWOPS_MEMORY;
	rw->size  = mem_size;
	rw->seek  = mem_seek;
	rw->read  = mem_read;
	rw->write = writable ? mem_write : mem_write_ro;
	rw->close = mem_close;
	rw->mem.base = base;
	rw->mem.here = base;
	rw->mem.stop = base + size;
	return rw;
}

SDL_RWops *SDL_RWFromMem(void *mem, int size)
{
	if (mem == NULL || size < 0) {
		SDL_SetError("picosdl: SDL_RWFromMem: bad arguments");
		return NULL;
	}
	return rwops_from_range((Uint8 *)mem, size, 1);
}

SDL_RWops *SDL_RWFromConstMem(const void *mem, int size)
{
	if (mem == NULL || size < 0) {
		SDL_SetError("picosdl: SDL_RWFromConstMem: bad arguments");
		return NULL;
	}
	return rwops_from_range((Uint8 *)(uintptr_t)mem, size, 0);
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
	/* No filesystem on this target yet. Saved games and the hall of fame will
	 * want a small flash key-value store; until then, fail rather than
	 * silently succeed and lose the data. */
	SDL_SetError("picosdl: no filesystem (tried to open '%s' mode '%s')",
	             file ? file : "(null)", mode ? mode : "(null)");
	return NULL;
}

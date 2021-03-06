/*
 * NVRAM variable manipulation (direct mapped flash)
 *
 * Copyright (C) 2012, Broadcom Corporation. All Rights Reserved.
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $Id: nvram_rw.c 258972 2011-05-11 08:57:26Z $
 */

#include <bcm_cfg.h>
#include <typedefs.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <siutils.h>
#include <bcmnvram.h>
#include <bcmendian.h>
#include <flashutl.h>
#include <hndsoc.h>
#include <sbchipc.h>

#ifdef NFLASH_SUPPORT
#include <hndnand.h>
#endif	/* NFLASH_SUPPORT */
#ifdef _CFE_
#include <hndsflash.h>
#else

#endif

struct nvram_tuple *_nvram_realloc(struct nvram_tuple *t, const char *name, const char *value);
void  _nvram_free(struct nvram_tuple *t);
int  _nvram_read(void *buf);

extern char *_nvram_get(const char *name);
extern int _nvram_set(const char *name, const char *value);
extern int _nvram_unset(const char *name);
extern int _nvram_getall(char *buf, int count);
extern int _nvram_commit(struct nvram_header *header);
extern int _nvram_init(void *si);
extern void _nvram_exit(void);

static struct nvram_header *nvram_header = NULL;
static int nvram_do_reset = FALSE;

#ifdef _CFE_
/* For NAND boot, flash0.nvram will be changed to nflash0.nvram */
char *flashdrv_nvram = "flash0.nvram";
#endif

#if defined(__ECOS)
extern int kernel_initial;
#define NVRAM_LOCK()	cyg_scheduler_lock()
#define NVRAM_UNLOCK()	cyg_scheduler_unlock()
#else
#define NVRAM_LOCK()	do {} while (0)
#define NVRAM_UNLOCK()	do {} while (0)
#endif

/* Convenience */
#define KB * 1024
#define MB * 1024 * 1024

char *
nvram_get(const char *name)
{
	char *value;

#ifdef __ECOS
	if (!kernel_initial)
		return NULL;
#endif

	NVRAM_LOCK();
	value = _nvram_get(name);
	NVRAM_UNLOCK();

	return value;
}

int
nvram_getall(char *buf, int count)
{
	int ret;

	NVRAM_LOCK();
	ret = _nvram_getall(buf, count);
	NVRAM_UNLOCK();

	return ret;
}

int
BCMINITFN(nvram_set)(const char *name, const char *value)
{
	int ret;

	NVRAM_LOCK();
	ret = _nvram_set(name, value);
	NVRAM_UNLOCK();

	return ret;
}

int
BCMINITFN(nvram_unset)(const char *name)
{
	int ret;

	NVRAM_LOCK();
	ret = _nvram_unset(name);
	NVRAM_UNLOCK();

	return ret;
}

int
BCMINITFN(nvram_resetgpio_init)(void *si)
{
	char *value;
	int gpio;
	si_t *sih;

	sih = (si_t *)si;

	value = nvram_get("reset_gpio");
	if (!value)
		return -1;

	gpio = (int) bcm_atoi(value);
	if (gpio > 31)
		return -1;

	/* Setup GPIO input */
	si_gpioouten(sih, ((uint32) 1 << gpio), 0, GPIO_DRV_PRIORITY);

	return gpio;
}

int
BCMINITFN(nvram_reset)(void  *si)
{
	int gpio;
	uint msec;
	si_t * sih = (si_t *)si;

	if ((gpio = nvram_resetgpio_init((void *)sih)) < 0)
		return FALSE;

	/* GPIO reset is asserted low */
	for (msec = 0; msec < 5000; msec++) {
		if (si_gpioin(sih) & ((uint32) 1 << gpio))
			return FALSE;
		OSL_DELAY(1000);
	}

	nvram_do_reset = TRUE;
	return TRUE;
}

#ifdef NFLASH_SUPPORT
static unsigned char nand_nvh[NVRAM_SPACE];

static struct nvram_header *
BCMINITFN(nand_find_nvram)(hndnand_t *nfl, uint32 off)
{
	int blocksize = nfl->blocksize;
	unsigned char *buf = nand_nvh;
	int rlen = sizeof(nand_nvh);
	int len;

	for (; off < NFL_BOOT_SIZE; off += blocksize) {
		if (hndnand_checkbadb(nfl, off) != 0)
			continue;

		len = blocksize;
		if (len >= rlen)
			len = rlen;

		if (hndnand_read(nfl, off, len, buf) == 0)
			break;

		buf += len;
		rlen -= len;
		if (rlen == 0)
			return (struct nvram_header *)nand_nvh;
	}

	return NULL;
}
#endif /* NFLASH_SUPPORT */

extern unsigned char embedded_nvram[];

static struct nvram_header *
BCMINITFN(find_nvram)(si_t *sih, bool embonly, bool *isemb)
{
	struct nvram_header *nvh;
	uint32 off, lim;
	uint32 flbase = SI_FLASH2;
	int bootdev;
#ifdef NFLASH_SUPPORT
	hndnand_t *nfl_info = NULL;
#endif
#ifdef _CFE_
	hndsflash_t *sfl_info = NULL;
#endif

	bootdev = soc_boot_dev((void *)sih);
#ifdef NFLASH_SUPPORT
	if (bootdev == SOC_BOOTDEV_NANDFLASH) {
		/* Init nand anyway */
		nfl_info = hndnand_init(sih);
		if (nfl_info)
			flbase = nfl_info->phybase;
	}
	else
#endif /* NFLASH_SUPPORT */
	if (bootdev == SOC_BOOTDEV_SFLASH) {
#ifdef _CFE_
		/* Init nand anyway */
		sfl_info = hndsflash_init(sih);
		if (sfl_info)
			flbase = sfl_info->phybase;
#else
	if (sih->ccrev == 42)
		flbase = SI_NS_NORFLASH;
#endif
	}

	if (!embonly) {
		*isemb = FALSE;
#ifdef NFLASH_SUPPORT
		if (nfl_info) {
			uint32 blocksize;

			blocksize = nfl_info->blocksize;
			off = blocksize;
			for (; off < NFL_BOOT_SIZE; off += blocksize) {
				if (hndnand_checkbadb(nfl_info, off) != 0)
					continue;
				nvh = (struct nvram_header *)OSL_UNCACHED(flbase + off);
				if (nvh->magic != NVRAM_MAGIC)
					continue;

				/* Read into the nand_nvram */
				if ((nvh = nand_find_nvram(nfl_info, off)) == NULL)
					continue;
//				if (nvram_calc_crc(nvh) == (uint8)nvh->crc_ver_init)
					return nvh;
			}
		}
		else
#endif /* NFLASH_SUPPORT */
		{
			lim = SI_FLASH2_SZ;
			off = FLASH_MIN;
			while (off <= lim) {
				nvh = (struct nvram_header *)
					OSL_UNCACHED(flbase + off - NVRAM_SPACE);
				if (nvh->magic == NVRAM_MAGIC) {
//					if (nvram_calc_crc(nvh) == (uint8) nvh->crc_ver_init) {
						return (nvh);
//					}
				}
				off <<= 1;
			}
		}
#ifdef BCMDBG
		printf("find_nvram: nvram not found, trying embedded nvram next\n");
#endif /* BCMDBG */
	}

	/* Now check embedded nvram */
	*isemb = TRUE;
	nvh = (struct nvram_header *)OSL_UNCACHED(flbase + (4 * 1024));
	if (nvh->magic == NVRAM_MAGIC)
		return (nvh);
	nvh = (struct nvram_header *)OSL_UNCACHED(flbase + 1024);
	if (nvh->magic == NVRAM_MAGIC)
		return (nvh);
#ifdef _CFE_
	nvh = (struct nvram_header *)embedded_nvram;
	if (nvh->magic == NVRAM_MAGIC)
		return (nvh);
#endif
	printf("find_nvram: no nvram found\n");
	return (NULL);
}

int
BCMATTACHFN(nvram_init)(void *si)
{
	bool isemb;
	int ret;
	si_t *sih;
	static int nvram_status = -1;

#ifdef __ECOS
	if (!kernel_initial)
		return 0;
#endif

	/* Check for previous 'restore defaults' condition */
	if (nvram_status == 1)
		return 1;

	/* Check whether nvram already initilized */
	if (nvram_status == 0 && !nvram_do_reset)
		return 0;

	sih = (si_t *)si;

	/* Restore defaults from embedded NVRAM if button held down */
	if (nvram_do_reset) {
		/* Initialize with embedded NVRAM */
		nvram_header = find_nvram(sih, TRUE, &isemb);
		ret = _nvram_init(si);
		if (ret == 0) {
			nvram_status = 1;
			return 1;
		}
		nvram_status = -1;
		_nvram_exit();
	}

	/* Find NVRAM */
	nvram_header = find_nvram(sih, FALSE, &isemb);
	ret = _nvram_init(si);
	if (ret == 0) {
		/* Restore defaults if embedded NVRAM used */
		if (nvram_header && isemb) {
			ret = 1;
		}
	}
	nvram_status = ret;
	return ret;
}

int
BCMINITFN(nvram_append)(void *si, char *vars, uint varsz)
{
	return 0;
}

void
BCMINITFN(nvram_exit)(void *si)
{
	si_t *sih;

	sih = (si_t *)si;

	_nvram_exit();
}

int
BCMINITFN(_nvram_read)(void *buf)
{
	uint32 *src, *dst;
	uint i;

	if (!nvram_header)
		return -19; /* -ENODEV */

	src = (uint32 *) nvram_header;
	dst = (uint32 *) buf;

	for (i = 0; i < sizeof(struct nvram_header); i += 4)
		*dst++ = *src++;

	for (; i < nvram_header->len && i < NVRAM_SPACE; i += 4)
		*dst++ = ltoh32(*src++);

	return 0;
}

struct nvram_tuple *
BCMINITFN(_nvram_realloc)(struct nvram_tuple *t, const char *name, const char *value)
{
	if (!(t = MALLOC(NULL, sizeof(struct nvram_tuple) + strlen(name) + 1 +
	                 strlen(value) + 1))) {
		printf("_nvram_realloc: our of memory\n");
		return NULL;
	}

	/* Copy name */
	t->name = (char *) &t[1];
	strcpy(t->name, name);

	/* Copy value */
	t->value = t->name + strlen(name) + 1;
	strcpy(t->value, value);

	return t;
}

void
BCMINITFN(_nvram_free)(struct nvram_tuple *t)
{
	if (t)
		MFREE(NULL, t, sizeof(struct nvram_tuple) + strlen(t->name) + 1 +
		      strlen(t->value) + 1);
}

#ifdef __ECOS
int
BCMINITFN(nvram_reinit_hash)(void)
{
	struct nvram_header *header;
	int ret;

	if (!(header = (struct nvram_header *) MALLOC(NULL, NVRAM_SPACE))) {
		printf("nvram_reinit_hash: out of memory\n");
		return -12; /* -ENOMEM */
	}

	NVRAM_LOCK();

	/* Regenerate NVRAM */
	ret = _nvram_commit(header);

	NVRAM_UNLOCK();
	MFREE(NULL, header, NVRAM_SPACE);
	return ret;
}
#endif /* __ECOS */

int
BCMINITFN(nvram_commit)(void)
{
	struct nvram_header *header;
	int ret;
	uint32 *src, *dst;
	uint i;

	if (!(header = (struct nvram_header *) MALLOC(NULL, NVRAM_SPACE))) {
		printf("nvram_commit: out of memory\n");
		return -12; /* -ENOMEM */
	}

	NVRAM_LOCK();

	/* Regenerate NVRAM */
	ret = _nvram_commit(header);
	if (ret)
		goto done;

	src = (uint32 *) &header[1];
	dst = src;

	for (i = sizeof(struct nvram_header); i < header->len && i < NVRAM_SPACE; i += 4)
		*dst++ = htol32(*src++);

#ifdef _CFE_
	if ((ret = cfe_open(flashdrv_nvram)) >= 0) {
		cfe_writeblk(ret, 0, (unsigned char *) header, header->len);
		cfe_close(ret);
	}
#else
	if (sysFlashInit(NULL) == 0) {
		/* set/write invalid MAGIC # (in case writing image fails/is interrupted)
		 * write the NVRAM image to flash(with invalid magic)
		 * set/write valid MAGIC #
		 */
		header->magic = NVRAM_CLEAR_MAGIC;
		nvWriteChars((unsigned char *)&header->magic, sizeof(header->magic));

		header->magic = NVRAM_INVALID_MAGIC;
		nvWrite((unsigned short *) header, NVRAM_SPACE);

		header->magic = NVRAM_MAGIC;
		nvWriteChars((unsigned char *)&header->magic, sizeof(header->magic));
	}
#endif /* ifdef _CFE_ */

done:
	NVRAM_UNLOCK();
	MFREE(NULL, header, NVRAM_SPACE);
	return ret;
}

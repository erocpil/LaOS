/* 有界 SVR4 newc 格式读取器，支持独立读取游标及兼容旧接口。 */
#include "cpio.h"
#include "string.h"

#define CPIO_HDR_LEN 110U
static struct cpio_reader default_reader;

void cpio_reset(void)
{
	memset(&default_reader, 0, sizeof(default_reader));
}

static int hex32(const char *s, uint32_t *value)
{
	uint32_t v = 0;
	for (int i = 0; i < 8; i++) {
		unsigned c = (unsigned char)s[i], digit;
		if (c >= '0' && c <= '9') {
			digit = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			digit = c - 'a' + 10;
		} else if (c >= 'A' && c <= 'F') {
			digit = c - 'A' + 10;
		} else {
			return -1;
		}
		v = (v << 4) | digit;
	}
	*value = v;
	return 0;
}

int cpio_reader_init(struct cpio_reader *reader, const void *archive, uint32_t size)
{
	if (!reader || !archive || (uintptr_t)archive > UINTPTR_MAX - size ||
		size < CPIO_HDR_LEN + 1 || memcmp(archive, "070701", 6)) {
		return -1;
	}
	*reader = (struct cpio_reader){.archive = archive, .size = size};
	return 0;
}

int cpio_reader_next(struct cpio_reader *reader, struct cpio_entry *entry)
{
	if (!reader || !reader->archive || !entry) {
		return -1;
	}
	if (reader->finished) {
		return 0;
	}
	uint64_t off = reader->offset;
	if (off > reader->size || reader->size - off < CPIO_HDR_LEN) {
		return -1;
	}
	const char *hdr = (const char *)reader->archive + off;
	uint32_t namesize, filesize, mode;
	if (memcmp(hdr, "070701", 6) || hex32(hdr + 94, &namesize) || hex32(hdr + 54, &filesize) ||
		hex32(hdr + 14, &mode) || !namesize) {
		return -1;
	}
	uint64_t name_start = off + CPIO_HDR_LEN;
	if (namesize > reader->size - name_start) {
		return -1;
	}
	const char *name = (const char *)reader->archive + name_start;
	if (name[namesize - 1]) {
		return -1;
	}
	for (uint32_t i = 0; i + 1 < namesize; i++) {
		if (!name[i]) {
			return -1;
		}
	}
	uint64_t data_start = (name_start + namesize + 3) & ~3ULL;
	if (data_start > reader->size || filesize > reader->size - data_start) {
		return -1;
	}
	uint64_t next = (data_start + filesize + 3) & ~3ULL;
	if (next > reader->size) {
		return -1;
	}
	if (namesize == 11 && !memcmp(name, "TRAILER!!!", 11)) {
		if (filesize) {
			return -1;
		}
		reader->finished = 1;
		return 0;
	}
	*entry = (struct cpio_entry){name, reader->archive + data_start, filesize, mode};
	reader->offset = (uint32_t)next;
	return 1;
}

int cpio_init(const void *archive, uint32_t size)
{
	return cpio_reader_init(&default_reader, archive, size);
}
int cpio_next(struct cpio_entry *entry)
{
	return cpio_reader_next(&default_reader, entry);
}
int cpio_find(const char *name, struct cpio_entry *entry)
{
	if (!name || !entry || !default_reader.archive) {
		return -1;
	}
	struct cpio_reader reader = default_reader;
	reader.offset = 0;
	reader.finished = 0;
	struct cpio_entry e;
	int ret;
	while ((ret = cpio_reader_next(&reader, &e)) == 1) {
		if (!strcmp(name, e.name)) {
			*entry = e;
			return 1;
		}
	}
	return ret;
}

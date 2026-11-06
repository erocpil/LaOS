/*
 * SVR4 newc CPIO 归档解析器接口。
 *
 * 解析 initrd 使用的 newc 格式（魔数 "070701"），提供独立游标迭代和
 * 兼容旧接口的按名称查找。
 */

#ifndef __CPIO_H__
#define __CPIO_H__

#include <stdint.h>

/* CPIO 归档中的单个文件条目；数据指针借用归档存储。 */
struct cpio_entry {
	const char *name; /* 以 NUL 结尾的文件名。 */
	const void *data; /* 文件内容。 */
	uint32_t size;	  /* 文件长度（字节）。 */
	uint32_t mode;	  /* newc c_mode，用于区分普通文件和目录。 */
};

/* 单个归档的迭代游标；归档存储由调用方持有。 */
struct cpio_reader {
	const uint8_t *archive;
	uint32_t size;
	uint32_t offset;
	int finished;
};
int cpio_reader_init(struct cpio_reader *reader, const void *archive, uint32_t size);
int cpio_reader_next(struct cpio_reader *reader, struct cpio_entry *entry);

/*
 * 初始化兼容旧接口的默认游标。成功返回 0，归档格式无效时返回 -1。
 */
int cpio_init(const void *archive, uint32_t size);

/* 清除当前归档游标状态，供启动测试阶段结束时切换归档使用。 */
void cpio_reset(void);

/*
 * 读取下一个条目。找到条目时填充 entry 并返回 1；到达 TRAILER!!! 时返回 0；
 * 格式错误时返回 -1。
 *
 * entry 中的名称和数据指针指向 cpio_init() 提供的归档缓冲区，调用方不得释放。
 */
int cpio_next(struct cpio_entry *entry);

/*
 * 按完整名称匹配文件。找到时填充 entry 并返回 1；未找到或解析失败时返回值
 * 分别为 0 和 -1。
 */
int cpio_find(const char *name, struct cpio_entry *entry);

#endif /* __CPIO_H__ */

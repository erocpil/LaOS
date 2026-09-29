/*
 * task_parser.c - task.conf 声明式任务清单解析
 *
 * 读取 task.conf v1 文本，解析为 task_entry 结构供 task.c 使用。
 */

#include "task_parser.h"
#include "task_conf.h"
#include "task.h"
#include "heap.h"
#include "string.h"
#include "debug.h"

static int is_line_end(char c)
{
	return c == '\n' || c == '\r' || c == '\0';
}

// 跳过空白字符(空格，制表符)
static void skip_spaces(char **p, char *end)
{
	while (*p < end && (**p == ' ' || **p == '	')) {
		(*p)++;
	}
}

// 跳过整行(用于处理注释和换行)
static void skip_line(char **p, char *end)
{
	while (*p < end && **p != '\n') {
		(*p)++;
	}

	if (*p < end && **p == '\n') {
		(*p)++;
	}
}

// 解析十进制整数
static int parse_dec(char **p, char *end, int *value)
{
	skip_spaces(p, end);
	if (*p >= end) {
		return -1;
	}

	int val = 0;
	while (*p < end && **p >= '0' && **p <= '9') {
		val = val * 10 + (**p - '0');
		(*p)++;
	}
	*value = val;

	return 0;
}

// 解析十六进制地址 (支持 0x 前缀)
static int parse_hex(char **p, char *end, uint64_t *value)
{
	skip_spaces(p, end);
	if (*p >= end) {
		return -1;
	}
	if (end - *p >= 2 && **p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
		*p += 2;
	}

	uint64_t val = 0;
	while (*p < end) {
		char c = **p;
		if (c >= '0' && c <= '9') {
			val = (val << 4) | (c - '0');
		} else if (c >= 'a' && c <= 'f') {
			val = (val << 4) | (c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			val = (val << 4) | (c - 'A' + 10);
		} else {
			break;
		}
		(*p)++;
	}
	*value = val;

	return 0;
}

// 解析字符串并拷贝到缓冲区
static int parse_string(char **p, char *end, char *dest, int max_len)
{
	skip_spaces(p, end);
	if (*p >= end) {
		dest[0] = '\0';
		return -1;
	}
	int i = 0;
	while (*p < end && **p != ' ' && **p != '	' && **p != '\n' &&
			**p != '\r' && **p != '\0' && i < max_len - 1) {
		dest[i++] = **p;
		(*p)++;
	}
	dest[i] = '\0';

	return 0;
}

static int parse_test_directive(char **p, char *end)
{
	struct selftest_directive d;
	char token[64];

	memset(&d, 0, sizeof(d));
	d.kv_count = 0;

	if (parse_string(p, end, d.name, sizeof(d.name)) != 0) {
		return -1;
	}

	/* Parse key=value pairs into directive record.
	 * No name validation — selftest_apply_all matches by registered name.
	 * Unknown test names are diagnosed at apply time, not at parse time. */
	for (;;) {
		skip_spaces(p, end);
		if (*p >= end || is_line_end(**p) || **p == '#') {
			break;
		}

		if (parse_string(p, end, token, sizeof(token)) != 0) {
			return -1;
		}

		/* Split key=value */
		char *eq = NULL;
		for (char *s = token; *s; s++) {
			if (*s == '=') { eq = s; break; }
		}
		if (!eq || d.kv_count >= SELFTEST_MAX_KV) {
			L("task.conf: invalid @test kv pair '%s'", token);
			return -1;
		}

		int klen = (int)(eq - token);
		if (klen >= (int)sizeof(d.kvs[0].key)) {
			klen = (int)sizeof(d.kvs[0].key) - 1;
		}
		memcpy(d.kvs[d.kv_count].key, token, klen);
		d.kvs[d.kv_count].key[klen] = '\0';

		int vlen = (int)strlen(eq + 1);
		if (vlen >= (int)sizeof(d.kvs[0].value)) {
			vlen = (int)sizeof(d.kvs[0].value) - 1;
		}
		memcpy(d.kvs[d.kv_count].value, eq + 1, vlen);
		d.kvs[d.kv_count].value[vlen] = '\0';

		d.kv_count++;
	}

	task_conf_add_directive(&d);

	return 0;
}

static int parse_directive(char **p, char *end)
{
	char name[32];
	char value[32];

	if (parse_string(p, end, name, sizeof(name)) != 0) {
		return -1;
	}

	if (strcmp(name, "@version") == 0) {
		if (parse_string(p, end, value, sizeof(value)) != 0) {
			return -1;
		}
		if (strcmp(value, "1") != 0) {
			L("task.conf: unsupported DSL version %s", value);
			return -1;
		}
		skip_spaces(p, end);
		if (*p < end && !is_line_end(**p) && **p != '#') {
			L("task.conf: trailing token after @version");
			return -1;
		}
		return 0;
	}

	if (strcmp(name, "@test") == 0) {
		return parse_test_directive(p, end);
	}

	if (strcmp(name, "@module_missing") == 0) {
		char value[16];
		if (parse_string(p, end, value, sizeof(value)) != 0) {
			return -1;
		}
		if (strcmp(value, "panic") == 0) {
			task_conf.module_missing_panic = true;
		} else if (strcmp(value, "skip") == 0) {
			task_conf.module_missing_panic = false;
		} else {
			L("task.conf: @module_missing expects skip or panic, got '%s'", value);
			return -1;
		}
		return 0;
	}

	L("task.conf: unsupported directive %s", name);

	return -1;
}

/**
 * parse_args() — 引号感知分词，分离 kv 和位置参数。
 *
 * 规则：
 *   - 空格/制表符分隔 token
 *   - "..." 双引号包裹 token（内部可含空格）
 *   - 含 '=' → kv 对（key=value），存入 kv_buf + 指针数组
 *   - 不含 '=' → 位置参数，存入 pos_buf（空格分隔）
 */
static int parse_args(char **p, char *end, char *pos_buf, int pos_max, char *kv_buf,
		int kv_max, int *kv_count, char *kv_keys[], char *kv_values[], int kv_cap)
{
	*kv_count = 0;
	pos_buf[0] = '\0';
	kv_buf[0] = '\0';

	char *pos_cur = pos_buf;
	char *pos_end = pos_buf + pos_max - 1;
	char *kv_cur = kv_buf;
	char *kv_end = kv_buf + kv_max - 1;

	while (*p < end && **p != '\n' && **p != '\r' && **p != '\0') {
		/* 跳过前导空格 */
		while (*p < end && (**p == ' ' || **p == '	')) {
			(*p)++;
		}
		if (*p == end || **p == '\n' || **p == '\r' || **p == '\0') {
			break;
		}

		/* 检测引号 */
		char quote = 0;
		if (**p == '"') {
			quote = '"';
			(*p)++; /* 跳过开引号 */
		}

		/* 定位 token 尾 */
		char *start = *p;
		while (*p < end && **p != '\n' && **p != '\r' && **p != '\0') {
			if (quote) {
				if (**p == '"') break;
			} else {
				if (**p == ' ' || **p == '	') {
					break;
				}
			}
			(*p)++;
		}
		char *token_end = *p; /* token 尾后一个字符 */

		if (quote && *p < end && **p == '"') {
			(*p)++; /* 跳闭引号 */
		}

		/* 分类：找 '=' */
		char *eq = NULL;
		for (char *s = start; s < token_end; s++) {
			if (*s == '=') {
				eq = s;
				break;
			}
		}

		if (eq && *kv_count < kv_cap) {
			/* kv 对：切分 key 和 value */
			int klen = (int)(eq - start);
			int vlen = (int)(token_end - eq - 1);
			if (kv_cur + klen + 1 + vlen + 1 <= kv_end) {
				memcpy(kv_cur, start, klen);
				kv_cur[klen] = '\0';
				kv_keys[*kv_count] = kv_cur;
				kv_cur += klen + 1;
				memcpy(kv_cur, eq + 1, vlen);
				kv_cur[vlen] = '\0';
				kv_values[*kv_count] = kv_cur;
				kv_cur += vlen + 1;
				(*kv_count)++;
			}
		} else if (!eq) {
			/* 位置参数：追加到 pos_buf */
			int len = (int)(token_end - start);
			if (pos_cur + len + 1 <= pos_end) {
				if (pos_cur != pos_buf) {
					*pos_cur++ = ' ';
				}
				memcpy(pos_cur, start, len);
				pos_cur[len] = '\0';
				pos_cur += len;
			}
		}
	}
	*pos_cur = '\0';
	*kv_cur = '\0';

	return 0;
}

int separate_name(char *info, char **module, char **name)
{
	if (!info) {
		return -1;
	}

	*module = info;
	char *p = info;
	while (*p && *p != ':') {
		p++;
	}

	if (*p != '\0') {
		*name = p + 1;
		*p = '\0';
		L(">>> info %s module %s name %s", info, *module, *name);
		return 1;
	}

	return 0;
}

int task_conf_parse_legacy_v1(const struct boot_module *f)
{
	char *p = (char*)f->address;
	char *end = (char*)((uintptr_t)f->address + f->size);

	while (p < end) {
		skip_spaces(&p, end);

		// 1. 处理空行或注释
		if (*p == '\n' || *p == '\r' || *p == '\0') {
			p++;
			continue;
		}
		if (*p == '#') {
			skip_line(&p, end);
			continue;
		}
		if (*p == '@') {
			if (parse_directive(&p, end) != 0) {
				return -1;
			}
			skip_line(&p, end);
			continue;
		}

		// 2. 分配并填充配置对象
		struct task *t = kmalloc(sizeof(struct task));
		if (!t) {
			return -1;
		}

		memset(t, 0, sizeof(*t));

		// 3. 顺序解析字段 (对应的配置格式)
		if (parse_dec(&p, end, &t->cpu_id) ||
				parse_string(&p, end, t->info, 64)) {
			kfree(t);
			return -1;
		}
		separate_name(t->info, &t->module, &t->name);
		if (parse_dec(&p, end, &t->type) ||
				parse_hex(&p, end, &t->magic) ||
				parse_args(&p, end, t->args_buf, 256, t->kv_buf, 256,
					&t->kv_count, t->kv_keys, t->kv_values, 16)) {
			kfree(t);
			return -1;
		}
		L("cpu %d module %s name %s type %d magic %p args '%s' kv=%d",
				t->cpu_id, t->module, t->name, t->type,
				(void*)t->magic, t->args_buf, t->kv_count);

		// 4. 加入全局链表
		list_add_tail(&t->node, &task_conf.head);

		// 5. 解析完一行后跳转到下一行起点
		skip_line(&p, end);
	}

	return 0;
}

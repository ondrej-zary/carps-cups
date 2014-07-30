/* CUPS driver for Canon CARPS printers */
/* Copyright (c) 2014 Ondrej Zary */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include "carps.h"

//#define DEBUG
//#define PBM

#define ERR(fmt, args ...)	fprintf(stderr, "ERROR: CARPS " fmt "\n", ##args);
#define WARN(fmt, args ...)	fprintf(stderr, "WARNING: CARPS " fmt "\n", ##args);

#ifdef DEBUG
//#define DBG(fmt, args ...)	fprintf(stderr, "DEBUG: CARPS " fmt "\n", ##args);
#define DBG(fmt, args ...)	fprintf(stderr, fmt, ##args);
#else
#define DBG(fmt, args ...)	do {} while (0)
#endif

int global_line_num, global_outpos;

void fill_header(struct carps_header *header, u8 data_type, u8 block_type, u16 data_len) {
	memset(header, 0, sizeof(struct carps_header));
	header->magic1 = 0xCD;
	header->magic2 = 0xCA;
	header->magic3 = 0x10;
	header->data_type = data_type;
	header->block_type = block_type;
	header->one = 0x01;
	header->data_len = cpu_to_be16(data_len);
}

void write_block(u8 data_type, u8 block_type, void *data, u16 data_len, FILE *stream) {
	struct carps_header header;

	fill_header(&header, data_type, block_type, data_len);
	fwrite(&header, 1, sizeof(header), stream);
	fwrite(data, 1, data_len, stream);
	global_outpos += sizeof(header) + data_len;
}

/* put n bits of data */
void put_bits(char **data, u16 *len, u8 *bitpos, u8 n, u8 bits) {
	if (!data)
		return;
	DBG("put_bits len=%d, pos=%d, n=%d, bits=%s\n", *len, *bitpos, n, bin_n(bits, n));
	bits <<= 8 - n;
	for (int i = 0; i < n; i++) {
		/* clear the byte first */
		if (*bitpos == 0)
			*data[0] = 0;
		if (bits & 0x80)
			*data[0] |= 1 << (7 - *bitpos);
		bits <<= 1;
		(*bitpos)++;
		if (*bitpos > 7) {
			*data[0] ^= PRINT_DATA_XOR;
			(*data)++;
			(*len)++;
			*bitpos = 0;
		}
	}
}

u16 line_len, line_len_file, line_pos;
int width, height, dpi;
u8 *last_lines[8], *cur_line;

int count_run_length(int line_pos, int line_num, __attribute__((unused)) int param) {
	int i;
	u8 first;

	if (line_num == 0 && line_pos == 0)	/* prevent -1 on first line */
		return 0;

	line_pos -= 1;

	if (line_pos < 0)	/* will work only for -1 */
		first = last_lines[0][line_len - 1];
	else
		first = cur_line[line_pos];

	for (i = line_pos + 1; i < line_len; i++)
		if (cur_line[i] != first)
			break;

	return i - line_pos - 1;
}

int count_prev(int line_pos, int line_num, int num_last) {
	int i;
	if (line_num <= num_last)
		return 0;

	for (i = line_pos; i < line_len; i++)
		if (cur_line[i] != last_lines[num_last][i])
			break;

	return i - line_pos;
}

int count_this(int line_pos, __attribute__((unused)) int line_num, int offset) {
	int i;
	int max = 0;
	if (line_pos < -offset)
		return 0;
	if (offset == -80)	/* does not use prefix: 127 is max */
		max = 127;

	for (i = line_pos; i < line_len; i++)
		if (cur_line[i] != cur_line[i + offset])
			break;

	if (max && i - line_pos > max)
		return max;

	return i - line_pos;
}

int dict_search(u8 byte, u8 *dict) {
	for (int i = 0; i < DICT_SIZE; i++)
		if (dict[i] == byte)
			return i;

	return -1;
}

void dict_add(u8 byte, u8 *dict) {
//	DBG("DICTIONARY=");
//	for (int i = 0; i < DICT_SIZE; i++)
//		DBG("%02X ", dict[i]);
//	DBG("\n");

	for (int i = 0; i < DICT_SIZE; i++)
		if (dict[i] == byte) {
			memmove(dict + i, dict + i + 1, DICT_SIZE - i);
			break;
		}
	memmove(dict + 1, dict, DICT_SIZE - 1);
	dict[0] = byte;
}

int fls(unsigned int n) {
	int i = 0;

	while (n >>= 1)
		i++;

	return i;
}

int encode_number(char **data, u16 *len, u8 *bitpos, int num) {
	int num_bits;
	int bits = 0;
	DBG("encode_number(%d)\n", num);

	if (num == 0) {
		put_bits(data, len, bitpos, 6, 0b111111);
		return 6;
	}

	if (num == 1) {
		put_bits(data, len, bitpos, 2, 0b00);
		return 2;
	}

	num_bits = fls(num);
	DBG("num_bits=%d\n", num_bits);
	if (num_bits == 1) {
		put_bits(data, len, bitpos, 2, 0b01);
		bits += 2;
	} else {
		put_bits(data, len, bitpos, num_bits - 1, 0xff);
		put_bits(data, len, bitpos, 1, 0b0);
		bits += num_bits;
	}
	put_bits(data, len, bitpos, num_bits, ~num & MASK(num_bits));
	bits += num_bits;

	return bits;
}

int encode_prefix(char **data, u16 *len, u8 *bitpos, int num) {
	put_bits(data, len, bitpos, 8, 0b11111100);

	return 8 + encode_number(data, len, bitpos, num / 128);
}

int encode_last(char **data, u16 *len, u8 *bitpos, int count, __attribute__((unused)) bool *prev8_flag, bool *twobyte_flag, int num_last) {
	int bits = 0;
	bool twobyte_flag_change = (num_last == -1) ? *twobyte_flag : !*twobyte_flag;

	if (data) /* change flag only if this encoding is really used */
		*twobyte_flag = (num_last == -2);

	if (count >= 128)
		bits += encode_prefix(data, len, bitpos, count);
	count %= 128;
	if (twobyte_flag_change) {
		put_bits(data, len, bitpos, 2, 0b11);
		bits += 2;
		bits += 6;////penalty
	}
	put_bits(data, len, bitpos, 4, 0b1110);
	bits += 4;

	return bits + encode_number(data, len, bitpos, count);
}

int encode_prev(char **data, u16 *len, u8 *bitpos, int count, bool *prev8_flag, __attribute__((unused)) bool *twobyte_flag, int num_last) {
	int bits = 0;
	bool prev8_flag_change = (num_last == 3) ? *prev8_flag : !*prev8_flag;

	if (data) /* change flag only if this encoding is really used */
		*prev8_flag = (num_last == 7);

	if (count >= 128)
		bits += encode_prefix(data, len, bitpos, count);
	count %= 128;
	if (prev8_flag_change) {
		put_bits(data, len, bitpos, 3, 0b110);
		bits += 3;
		bits += 7;////penalty
	}
	put_bits(data, len, bitpos, 1, 0b0);
	bits += 1;

	return bits + encode_number(data, len, bitpos, count);
}

int encode_dict(char **data, u16 *len, u8 *bitpos, u8 pos) {
	put_bits(data, len, bitpos, 2, 0b10);
	put_bits(data, len, bitpos, 4, ~pos & 0b1111);

	return 2 + 4;
}

int encode_80(char **data, u16 *len, u8 *bitpos, int count, __attribute__((unused)) bool *prev8_flag, __attribute__((unused)) bool *twobyte_flag, __attribute__((unused)) int param) {
	put_bits(data, len, bitpos, 5, 0b11110);

	return 5 + encode_number(data, len, bitpos, count);
}

struct print_encoder {
	char *name;
	int (*get_count)(int line_pos, int line_num, int param);
	int (*encode)(char **data, u16 *len, u8 *bitpos, int count, bool *prev8_flag, bool *twobyte_flag, int param);
	int param;
	int ratio;
	int count;
};

struct print_encoder encoders[] = {
	{ .name = "@-80", .get_count = count_this, .encode = encode_80, .param = -80 },
	{ .name = "run_len", .get_count = count_run_length, .encode = encode_last, .param = -1 },
	{ .name = "@-2", .get_count = count_this, .encode = encode_last, .param = -2 },
	{ .name = "previous[3]", .get_count = count_prev, .encode = encode_prev, .param = 3 },
	{ .name = "previous[7]", .get_count = count_prev, .encode = encode_prev, .param = 7 },
};

u16 encode_print_data(int *num_lines, bool last, FILE *f, cups_raster_t *ras, char *out) {
	u8 bitpos = 0;
	u16 len = 0;
	int line_num = 0;
	DBG("num_lines=%d\n", *num_lines);
	u8 dictionary[DICT_SIZE];
	bool prev8_flag = false;
	bool twobyte_flag = false;
#ifdef DEBUG
	char *start = out;
#endif
	memset(dictionary, 0xaa, DICT_SIZE);

	while (((f && !feof(f)) || (ras)) && line_num < *num_lines) {
		memset(cur_line, 0, line_len);
		if (ras) {
			DBG("cupsRasterReadPixels(%p, %p, %d)\n", ras, cur_line, line_len_file);
			if (cupsRasterReadPixels(ras, cur_line, line_len_file) == 0)
				break;
		} else
			fread(cur_line, 1, line_len_file, f);
		DBG("line_num=%d (global=%d)\n", line_num, global_line_num);
		line_pos = 0;

		while (line_pos < line_len) {
			DBG("line_pos=%d, outpos=%d: ", line_pos, global_outpos + out - start);
			/* try all compression methods */
			for (unsigned int i = 0; i < ARRAY_SIZE(encoders); i++) {
				int bits = 0;
				encoders[i].count = encoders[i].get_count(line_pos, line_num, encoders[i].param);
				if (encoders[i].count > 1) {
					bits = encoders[i].encode(NULL, NULL, NULL, encoders[i].count, &prev8_flag, &twobyte_flag, encoders[i].param);
					encoders[i].ratio = bits ? encoders[i].count * 80 / bits : 0;
				} else
					encoders[i].ratio = 0;
				DBG("%s=%d, %d bits, ratio=%d\n", encoders[i].name, encoders[i].count, bits, encoders[i].ratio);
			}
			/* choose the best one */
			int best_ratio = 0;
			int best_encoder;
			for (unsigned int i = 0; i < ARRAY_SIZE(encoders); i++)
				if (encoders[i].ratio > best_ratio) {
					best_ratio = encoders[i].ratio;
					best_encoder = i;
				}
			/* if found, use it */
			if (best_ratio) {
				DBG("Using %s\n", encoders[best_encoder].name);
				encoders[best_encoder].encode(&out, &len, &bitpos, encoders[best_encoder].count, &prev8_flag, &twobyte_flag, encoders[best_encoder].param);
				line_pos += encoders[best_encoder].count;
				continue;
			}
			/* dictionary */
			int pos = dict_search(cur_line[line_pos], dictionary);
			if (pos >= 0) {
				DBG("dict @%d\n", pos);
				encode_dict(&out, &len, &bitpos, pos);
				dict_add(cur_line[line_pos], dictionary);
				line_pos++;
				continue;
			}
			/* zero byte */
			if (cur_line[line_pos] == 0x00) {
				DBG("zero\n");
				put_bits(&out, &len, &bitpos, 8, 0b11111101);
				dict_add(0, dictionary);
				line_pos++;
				continue;
			}
			/* fallback: byte immediate */
			put_bits(&out, &len, &bitpos, 4, 0b1101);
			put_bits(&out, &len, &bitpos, 8, cur_line[line_pos]);
			dict_add(cur_line[line_pos], dictionary);
			line_pos++;
		}
		memcpy(last_lines[7], last_lines[6], line_len);
		memcpy(last_lines[6], last_lines[5], line_len);
		memcpy(last_lines[5], last_lines[4], line_len);
		memcpy(last_lines[4], last_lines[3], line_len);
		memcpy(last_lines[3], last_lines[2], line_len);
		memcpy(last_lines[2], last_lines[1], line_len);
		memcpy(last_lines[1], last_lines[0], line_len);
		memcpy(last_lines[0], cur_line, line_len);
		line_pos = 0;
		line_num++;
		global_line_num++;
	}
	/* block end marker */
	DBG("block end\n");
	put_bits(&out, &len, &bitpos, 8, 0b11111110);
	put_bits(&out, &len, &bitpos, 2, 0b00);
	/* fill unused bits in last byte */
	DBG("%d unused bits\n", 8 - bitpos);
	put_bits(&out, &len, &bitpos, 8 - bitpos, 0xff);

	if (last) {
		put_bits(&out, &len, &bitpos, 8, 0xfe);
		put_bits(&out, &len, &bitpos, 8, 0x7f);
		put_bits(&out, &len, &bitpos, 8, 0xff);
		put_bits(&out, &len, &bitpos, 8, 0xff);
	}

	*num_lines = line_num;

	return len;
}

int encode_print_block(int height, FILE *f, cups_raster_t *ras) {
	int num_lines = 65536 / line_len;
	int ofs;
	bool last = false;
	char buf[BUF_SIZE], buf2[BUF_SIZE];
	char *buf_pos;

	if (num_lines > height) {
		DBG("num_lines := %d\n", height);
		num_lines = height;
		last = true;
	}
	/* encode print data first as we need the length and line count */
	u16 len = encode_print_data(&num_lines, last, f, ras, buf2);
	/* strip header */
	ofs = sprintf(buf, "\x01\x1b[;%d;%d;15.P", width, num_lines);
	/* print data header */
	struct carps_print_header *ph = (void *)buf + ofs;
	memset(ph, 0, sizeof(struct carps_print_header));
	ph->one = 0x01;
	ph->two = 0x02;
	ph->four = 0x04;
	ph->eight = 0x08;
	ph->magic = 0x50;
	ph->last = last ? 0 : 1;
	ph->data_len = cpu_to_le16(len);
	/* copy print data after the headers */
	memcpy(buf + ofs + sizeof(struct carps_print_header), buf2, len);
	len = ofs + sizeof(struct carps_print_header) + len;
	buf[len++] = 0x80;	/* strip data end */

	if (len <= MAX_DATA_LEN)
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, len, stdout);
	else {
		/* write strip header + print data header separately */
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, ofs + sizeof(struct carps_print_header), stdout);
		buf_pos = buf;
		buf_pos += ofs + sizeof(struct carps_print_header);
		len -= ofs + sizeof(struct carps_print_header);
		/* then write data blocks at most MAX_BLOCK_LEN bytes long */
		while (len) {
			/* insert 0x01 byte at the beginning of each continuing block */
			buf_pos--;
			len++;
			buf_pos[0] = 0x01;
			int block_len = (len > MAX_DATA_LEN) ? MAX_DATA_LEN : len;
			fprintf(stderr, "len=%d, block_len=%d\n", len, block_len);
			write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf_pos, block_len, stdout);
			buf_pos += block_len;
			len -= block_len;
		}
	}

	return num_lines;
}

enum carps_paper_size encode_paper_size(const char *paper_size_name) {
	if (!strcmp(paper_size_name, "A4"))
		return PAPER_A4;
	else if (!strcmp(paper_size_name, "A5"))
		return PAPER_A5;
	else if (!strcmp(paper_size_name, "B5"))
		return PAPER_B5;
	else if (!strcmp(paper_size_name, "Letter"))
		return PAPER_LETTER;
	else if (!strcmp(paper_size_name, "Legal"))
		return PAPER_LEGAL;
	else if (!strcmp(paper_size_name, "Executive"))
		return PAPER_EXECUTIVE;
	else if (!strcmp(paper_size_name, "Monarch"))
		return PAPER_ENV_MONAR;
	else if (!strcmp(paper_size_name, "Env10"))
		return PAPER_ENV_COM10;
	else if (!strcmp(paper_size_name, "DL"))
		return PAPER_ENV_DL;
	else if (!strcmp(paper_size_name, "C5"))
		return PAPER_ENV_C5;
	else
		return PAPER_CUSTOM;
}

void fill_print_data_header(char *buf, unsigned int copies, unsigned int dpi, unsigned int weight, const char *paper_size_name, unsigned int paper_width, unsigned int paper_height) {
	char tmp[100];
	enum carps_paper_size paper_size = encode_paper_size(paper_size_name);

//	\x01.%@.P42;600;1J;ImgColor.\.[11h.[?7;600 I.[20't.[14;;;;;;p.[?2h.[1v.[600;1;0;32;;64;0'c
	buf[0] = 1;
	buf[1] = 0;
	strcat(buf, "\x1b%@");
	/* ??? and resolution */
	sprintf(tmp, "\x1bP42;%d;1J;ImgColor", dpi);
	strcat(buf, tmp);
	/* ??? */
	strcat(buf, "\x1b\\");
	/* ??? */
	strcat(buf, "\x1b[11h");
	/* ??? and resolution */
	sprintf(tmp, "\x1b[?7;%d I", dpi);
	strcat(buf, tmp);
	/* paper weight */
	sprintf(tmp, "\x1b[%d't", weight);
	strcat(buf, tmp);
	/* paper size */
	if (paper_size == PAPER_CUSTOM) {
		/* compute custom paper size in print dots */
		int margin = 2 * dpi / 10;
		paper_height = paper_height * dpi / POINTS_PER_INCH;
		paper_width = paper_width * dpi / POINTS_PER_INCH;
		sprintf(tmp, "\x1b[%d;%d;%d;%d;%d;%d;%dp", paper_size, paper_height, paper_width, margin, margin, margin, margin);
	} else
		sprintf(tmp, "\x1b[%d;;;;;;p", paper_size);
	strcat(buf, tmp);
	/* ??? */
	strcat(buf, "\x1b[?2h");
	/* number of copies */
	sprintf(tmp, "\x1b[%dv", copies);
	strcat(buf, tmp);
	/* resolution and ??? */
	sprintf(tmp, "\x1b[%d;1;0;32;;64;0'c", dpi);
	strcat(buf, tmp);
}

char *ppd_get(ppd_file_t *ppd, const char *name) {
	ppd_attr_t *attr = ppdFindAttr(ppd, name, NULL);

	if (attr)
		return attr->value;
	else {
		ppd_choice_t *choice;
		choice = ppdFindMarkedChoice(ppd, name);
		if (!choice)
			return NULL;
		return choice->choice;
	}
}

int main(int argc, char *argv[]) {
	char buf[BUF_SIZE];
	struct carps_doc_info *info;
	struct carps_time *doc_time;
	struct carps_print_params params;
	char tmp[100];
#ifdef PBM
	bool pbm_mode = false;
#else
#define pbm_mode 0
#endif
	FILE *f;
	cups_raster_t *ras = NULL;
	cups_page_header2_t page_header;
	unsigned int page = 0, copies;
	int fd;
	ppd_file_t *ppd;
	bool header_written = false;
#ifdef PBM
	if (argc < 2 || argc == 3 || argc == 4 || argc == 5 || argc > 7) {
		fprintf(stderr, "usage: rastertocarps <file.pbm>\n");
#else
	if (argc < 6 || argc > 7) {
#endif
		fprintf(stderr, "usage: rastertocarps job-id user title copies options [file]\n");
		return 1;
	}
#ifdef PBM
	if (argc < 3)
		pbm_mode = true;
#endif
	if (pbm_mode) {
		f = fopen(argv[1], "r");
		if (!f) {
			perror("Unable to open file");
			return 2;
		}

		fgets(tmp, sizeof(tmp), f);
		if (strcmp(tmp, "P4\n")) {
			fprintf(stderr, "Invalid PBM file\n");
			return 2;
		}
		do
			fgets(tmp, sizeof(tmp), f);
		while (tmp[0] == '#');
		sscanf(tmp, "%d %d", &width, &height);
		DBG("width=%d height=%d\n", width, height);
		line_len_file = DIV_ROUND_UP(width, 8);
		line_len = ROUND_UP_MULTIPLE(line_len_file, 4);
		cur_line = malloc(line_len);
		for (int i = 0; i < 8; i++)
			last_lines[i] = malloc(line_len);
	} else {
		int n;
		cups_option_t *options;

		copies = atoi(argv[4]);
		if (copies < 1)
			copies = 1;

		if (argc > 6) {
			fd = open(argv[6], O_RDONLY);
			if (fd == -1) {
				perror("ERROR: Unable to open raster file - ");
				return 1;
			}
		} else
			fd = 0;
		ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
		ppd = ppdOpenFile(getenv("PPD"));
		if (!ppd) {
			fprintf(stderr, "Unable to open PPD file %s\n", getenv("PPD"));
			return 2;
		}
		ppdMarkDefaults(ppd);
		n = cupsParseOptions(argv[5], 0, &options);
		cupsMarkOptions(ppd, n, options);
		cupsFreeOptions(n, options);
	}

	/* document beginning */
	u8 begin_data[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_BEGIN, begin_data, sizeof(begin_data), stdout);
	/* document info - title */
	char *doc_title;
	if (pbm_mode)
		doc_title = "Untitled";
	else
		doc_title = argv[3];
	info = (void *)buf;
	info->type = cpu_to_be16(CARPS_DOC_INFO_TITLE);
	info->unknown = cpu_to_be16(0x11);
	info->data_len = strlen(doc_title) > 255 ? 255 : strlen(doc_title);
	strncpy(buf + sizeof(struct carps_doc_info), doc_title, 255);
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_DOC_INFO, buf, sizeof(struct carps_doc_info) + strlen(doc_title), stdout);
	/* document info - user name */
	char *user_name;
	if (pbm_mode)
		user_name = "root";
	else
		user_name = argv[2];
	info = (void *)buf;
	info->type = cpu_to_be16(CARPS_DOC_INFO_USER);
	info->unknown = cpu_to_be16(0x11);
	info->data_len = strlen(user_name) > 255 ? 255 : strlen(user_name);
	strncpy(buf + sizeof(struct carps_doc_info), user_name, 255);
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_DOC_INFO, buf, sizeof(struct carps_doc_info) + strlen(user_name), stdout);
	/* document info - time */
	time_t timestamp = time(NULL);
	struct tm *tm = gmtime(&timestamp);
	doc_time = (void *)buf;
	memset(doc_time, 0, sizeof(struct carps_time));
	doc_time->type = cpu_to_be16(CARPS_DOC_INFO_TIME);
	if (!pbm_mode) {
		doc_time->year = (1900 + tm->tm_year) >> 4;
		doc_time->year_month = ((1900 + tm->tm_year) << 4) | (tm->tm_mon + 1);
		doc_time->day = (tm->tm_mday << 3) | tm->tm_wday;
		doc_time->hour = tm->tm_hour;
		doc_time->min = tm->tm_min;
		doc_time->sec_msec = tm->tm_sec << 2;
	} else {
		/* MF5730 driver does not fill the time data, maybe because of a bug? */
		/* Printer accepts data with time so we always fill it in. */
		/* But we use this for test purposes so the output file does not change with time */
		doc_time->day = 7;
	}
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_DOC_INFO, buf, sizeof(struct carps_time), stdout);
	/* begin 1 */
	memset(buf, 0, 4);
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_BEGIN1, buf, 4, stdout);
	/* begin 2 */
	memset(buf, 0, 4);
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_BEGIN2, buf, 4, stdout);
	/* print params - unknown  */
	u8 unknown_param[] = { 0x00, 0x2e, 0x82, 0x00, 0x00 };
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, unknown_param, sizeof(unknown_param), stdout);
	/* print params - image refinement */
	params.magic = CARPS_PARAM_MAGIC;
	params.param = CARPS_PARAM_IMAGEREFINE;
	params.enabled = CARPS_PARAM_ENABLED;
	if (!pbm_mode) {
		char *value = ppd_get(ppd, "ImageRefinement");
		if (!strcmp(value, "OFF"))
			params.enabled = CARPS_PARAM_DISABLED;
	}
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, &params, sizeof(params), stdout);
	/* print params - toner save */
	params.magic = CARPS_PARAM_MAGIC;
	params.param = CARPS_PARAM_TONERSAVE;
	params.enabled = CARPS_PARAM_DISABLED;
	if (!pbm_mode) {
		char *value = ppd_get(ppd, "TonerSave");
		if (strcmp(value, "DEFAULT")) {
			if (!strcmp(value, "ON"))
				params.enabled = CARPS_PARAM_ENABLED;
			write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, &params, sizeof(params), stdout);
		}
	} else
		write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, &params, sizeof(params), stdout);

	if (!pbm_mode) {
		while (cupsRasterReadHeader2(ras, &page_header)) {
			page++;
			fprintf(stderr, "PAGE: %d %d\n", page, page_header.NumCopies);

			line_len_file = page_header.cupsBytesPerLine;
			line_len = ROUND_UP_MULTIPLE(line_len_file, 4);
			if (!cur_line) {
				cur_line = malloc(line_len);
				for (int i = 0; i < 8; i++)
					last_lines[i] = malloc(line_len);
			}
			height = page_header.cupsHeight;
			width = page_header.cupsWidth;
			dpi = page_header.HWResolution[0];
			DBG("line_len_file=%d,line_len=%d height=%d width=%d", line_len_file, line_len, height, width);
			if (!header_written) {	/* print data header */
				char *page_size_name = page_header.cupsPageSizeName;
				/* get page size name from PPD if cupsPageSizeName is empty */
				if (strlen(page_size_name) == 0)
					page_size_name = ppd_get(ppd, "PageSize");
				fill_print_data_header(buf, copies, dpi, page_header.cupsMediaType, page_size_name, page_header.PageSize[0], page_header.PageSize[1]);
				write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, strlen(buf), stdout);
				header_written = true;
			}

			/* read raster data */
			while (height > 0)
				height -= encode_print_block(height, NULL, ras);
			/* end of page */
			u8 page_end[] = { 0x01, 0x0c };
			write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, page_end, sizeof(page_end), stdout);
		}
	} else {
		/* print data header */
		fill_print_data_header(buf, 1, 600, WEIGHT_PLAIN, "A4", 0, 0);	/* 1 copy, 600 dpi, plain paper, A4 */
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, strlen(buf), stdout);
		/* print data */
		while (!feof(f) && height > 0)
			height -= encode_print_block(height, f, NULL);
		/* end of page */
		u8 page_end[] = { 0x01, 0x0c };
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, page_end, sizeof(page_end), stdout);
	}
	if (pbm_mode)
		fclose(f);
	else {
		ppdClose(ppd);
		cupsRasterClose(ras);
	}
	/* end of print data */
	u8 print_data_end[] = { 0x01, 0x1b, 'P', '0', 'J', 0x1b, '\\' };
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, print_data_end, sizeof(print_data_end), stdout);
	/* end of print data */
	buf[0] = 1;
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PRINT, buf, 1, stdout);
	/* end 2 */
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END2, NULL, 0, stdout);
	/* end 1 */
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END1, NULL, 0, stdout);
	/* end of document */
	buf[0] = 0;
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END, buf, 1, stdout);

	if (cur_line) {
		free(cur_line);
		for (int i = 0; i < 8; i++)
			free(last_lines[i]);
	}

	return 0;
}

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cups/raster.h>
#include "carps.h"

//#define DEBUG

#define ERR(fmt, args ...)	fprintf(stderr, "ERROR: CARPS " fmt "\n", ##args);
#define WARN(fmt, args ...)	fprintf(stderr, "WARNING: CARPS " fmt "\n", ##args);

#ifdef DEBUG
//#define DBG(fmt, args ...)	fprintf(stderr, "DEBUG: CARPS " fmt "\n", ##args);
#define DBG(fmt, args ...)	fprintf(stderr, fmt, ##args);
#else
#define DBG(fmt, args ...)	do {} while (0)
#endif

int global_line_num = 0, global_outpos = 0;

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

const char *bin_n(u8 x, u8 n) {
    static char b[9];
    b[0] = '\0';

    for (u8 i = 1 << (n - 1); i > 0; i >>= 1)
        strcat(b, (x & i) ? "1" : "0");

    return b;
}

/* put n bits of data */
void put_bits(char **data, u16 *len, u8 *bitpos, u8 n, u8 bits) {
	DBG("put_bits len=%d, pos=%d, n=%d, bits=%s\n", *len, *bitpos, n, bin_n(bits, n));
	bits <<= 8 - n;
//	printf("put_bits2 len=%d, pos=%d, n=%d, bits=%s\n", *len, *bitpos, n, bin_n(bits, 8));
	for (int i = 0; i < n; i++) {
		/* clear the byte first */
		if (*bitpos == 0)
			*data[0] = 0;
//		printf("data[0] = %s", bin_n(*data[0], 8));
		if (bits & 0x80)
			*data[0] |= 1 << (7 - *bitpos);
//		printf("->%s\n", bin_n(*data[0], 8));
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

u16 line_len;
u8 last_lines[8][1000], cur_line[1000];///////////
u16 line_pos;

int count_run_length(int pos) {
	int i;
	u8 first;

	if (pos < 0)	/* will work only for -1 */
		first = last_lines[0][line_len - pos + 1];
	else
		first = cur_line[pos];

	for (i = pos + 1; i < line_len; i++)
		if (cur_line[i] != first)
			break;

	return i - pos;
}

int count_previous(int pos, int num_last) {
	int i;

	for (i = pos; i < line_len; i++)
		if (cur_line[i] != last_lines[num_last][i])
			break;

	return i - pos;
}

int count_this(int pos, int offset, int max) {
	int i;

	for (i = pos; i < line_len; i++)
		if (cur_line[i] != cur_line[i + offset])
			break;

	if (max && i - pos > max)
		return max;

	return i - pos;
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

int encode_last_bytes(char **data, u16 *len, u8 *bitpos, int count, bool twobyte_flag_change) {
	int bits = 0;
	int len2 = 0;
	u8 bitpos2 = 0;

	if (!len)
		len = &len2;
	if (!bitpos)
		bitpos = &bitpos2;

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

int encode_previous(char **data, u16 *len, u8 *bitpos, int count, bool prev8_flag_change) {
	int bits = 0;
	int len2 = 0;
	u8 bitpos2 = 0;

	if (!len)
		len = &len2;
	if (!bitpos)
		bitpos = &bitpos2;

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

int encode_80(char **data, u16 *len, u8 *bitpos, int count) {
	int len2 = 0;
	u8 bitpos2 = 0;

	if (!len)
		len = &len2;
	if (!bitpos)
		bitpos = &bitpos2;

	put_bits(data, len, bitpos, 5, 0b11110);

	return 5 + encode_number(data, len, bitpos, count);
}

u16 encode_print_data(int *num_lines, bool last, FILE *f, cups_raster_t *ras, char *out) {
	u8 bitpos = 0;
	u16 len = 0;
	int line_num = 0;
	DBG("num_lines=%d\n", *num_lines);
	u8 dictionary[DICT_SIZE];
	bool prev8_flag = false;
	bool twobyte_flag = false;
	char *start = out;

	memset(dictionary, 0xaa, DICT_SIZE);

	while ( ( (f && !feof(f)) || (ras) ) && line_num < *num_lines) {
		if (ras) {
			DBG("cupsRasterReadPixels(%p, %p, %d)\n", ras, cur_line, line_len);
			if (cupsRasterReadPixels(ras, cur_line, line_len) == 0)
				break;
		} else
			fread(cur_line, 1, line_len, f);
		DBG("line_num=%d (global=%d)\n", line_num, global_line_num);
		line_pos = 0;

		while (line_pos < line_len) {
			DBG("line_pos=%d, outpos=%d: ", line_pos, global_outpos + out - start);
			int count_80 = 0, count_2, count_last = 0, count_prev3 = 0, count_prev7 = 0;
			int bits_80 = 0, bits_2 = 0, bits_last = 0, bits_prev3 = 0, bits_prev7 = 0;
			unsigned int data_80 = 0, data_2, data_last = 0, data_prev3 = 0, data_prev7 = 0;
			int ratio_80, ratio_2, ratio_last, ratio_prev3, ratio_prev7;
			void *p;

			if (line_pos >= 80) {
				count_80 = count_this(line_pos, -80, 127); /* does not use prefix: 127 is max */
				if (count_80 > 1) {
					p = &data_80;
					bits_80 = encode_80(&p, NULL, NULL, count_80);
				}
			}
			if (line_pos >= 2) {
				count_2 = count_this(line_pos, -2, 0);
				if (count_2 > 1) {
					p = &data_2;
					bits_2 = encode_last_bytes(&p, NULL, NULL, count_2, !twobyte_flag);
				}
			}
			if (line_num > 0 || line_pos > 0) {	/* prevent -1 on first line */
				count_last = count_run_length(line_pos - 1) - 1;
				if (count_last > 1) {
					p = &data_last;
					bits_last = encode_last_bytes(&p, NULL, NULL, count_last, twobyte_flag);
				}
			}
			if (line_num > 3) {
				count_prev3 = count_previous(line_pos, 3);
				if (count_prev3 > 1) {
					p = &data_prev3;
					bits_prev3 = encode_previous(&p, NULL, NULL, count_prev3, prev8_flag);
				}
			}
			if (line_num > 7) {
				count_prev7 = count_previous(line_pos, 7);
				if (count_prev7 > 1) {
					p = &data_prev7;
					bits_prev7 = encode_previous(&p, NULL, NULL, count_prev7, !prev8_flag);
				}
			}

			ratio_80 = bits_80 ? count_80*80/bits_80 : 0;
			ratio_2 = bits_2 ? count_2*80/bits_2 : 0;
			ratio_last = bits_last ? count_last*80/bits_last : 0;
			ratio_prev3 = bits_prev3 ? count_prev3*80/bits_prev3 : 0;
			ratio_prev7 = bits_prev7 ? count_prev7*80/bits_prev7 : 0;

			DBG("@-80=%d, %d bits, ratio=%d\n", count_80, bits_80, ratio_80);
			DBG("@-2=%d, %d bits, ratio=%d\n", count_2, bits_2, ratio_2);
			DBG("run_len=%d, %d bits, ratio=%d\n", count_last, bits_last, ratio_last);
			DBG("previous [3] count=%d, %d bits, ratio=%d\n", count_prev3, bits_prev3, ratio_prev3);
			DBG("previous [7] count=%d, %d bits, ratio=%d\n", count_prev7, bits_prev7, ratio_prev7);

			if (ratio_80 > 0 && ratio_80 >= ratio_2 && ratio_80 >= ratio_last && ratio_80 >= ratio_prev3 && ratio_80 >= ratio_prev7) {
				DBG("@-80\n");
				encode_80(&out, &len, &bitpos, count_80);
				line_pos += count_80;
				continue;
			}

			if (ratio_2 > 0 && ratio_2 >= ratio_80 && ratio_2 >= ratio_last && ratio_2 >= ratio_prev3 && ratio_2 >= ratio_prev7) {
				DBG("@-2\n");
				encode_last_bytes(&out, &len, &bitpos, count_2, !twobyte_flag);
				if (!twobyte_flag)
					twobyte_flag = true;
				line_pos += count_2;
				continue;
			}

			if (ratio_last > 0 && ratio_last >= ratio_80 && ratio_last >= ratio_2 && ratio_last >= ratio_prev3 && ratio_last >= ratio_prev7) {
				DBG("run_len\n");
				encode_last_bytes(&out, &len, &bitpos, count_last, twobyte_flag);
				if (twobyte_flag)
					twobyte_flag = false;
				line_pos += count_last;
				continue;
			}

			if (ratio_prev3 > 0 && ratio_prev3 >= ratio_prev7) {
				DBG("prev3\n");
				encode_previous(&out, &len, &bitpos, count_prev3, prev8_flag);
				if (prev8_flag)
					prev8_flag = false;
				line_pos += count_prev3;
				continue;
			}

			if (ratio_prev7 > 0 && ratio_prev7 >= ratio_prev3) {
				DBG("prev7\n");
				encode_previous(&out, &len, &bitpos, count_prev7, !prev8_flag);
				if (!prev8_flag)
					prev8_flag = true;
				line_pos += count_prev7;
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
	
	if (num_lines > height) {
		DBG("num_lines := %d\n", height);
		num_lines = height;
		last = true;
	}
	/* encode print data first as we need the length and line count */
	u16 len = encode_print_data(&num_lines, last, f, ras, buf2);
	/* strip header */
	ofs = sprintf(buf, "\x01\x1b[;%d;%d;15.P", 4724, num_lines);
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
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, len, stdout);

	return num_lines;
}

int main(int argc, char *argv[]) {
	char buf[BUF_SIZE];
	struct carps_doc_info *info;
	struct carps_print_params params;
	char tmp[100];
	int width, height;
	bool pbm_mode = false;
	FILE *f;
	cups_raster_t *ras = NULL;
	cups_page_header2_t page_header;
	unsigned int page = 0;
	int fd;

	if (argc < 2 || argc == 3 || argc == 4 || argc == 5 || argc > 7) {
		fprintf(stderr, "usage: rastertocarps <file.pbm>\n");
		fprintf(stderr, "usage: rastertocarps job-id user title copies options [file]\n");
		return 1;
	}

	if (argc < 3)
		pbm_mode = true;

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
		line_len = width / 8;
		if (line_len > 500)////////////////
			line_len++;
	} else {
		if (argc > 6) {
			if ((fd = open(argv[6], O_RDONLY)) == -1) {
				perror("ERROR: Unable to open raster file - ");
				return 1;
			}
		} else
			fd = 0;
		ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
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
	/* document info - unknown */
	info = (void *)buf;
	info->type = cpu_to_be16(0x09);
	info->unknown = cpu_to_be16(0x00);
	info->data_len = 0x07;
	memset(buf + sizeof(struct carps_doc_info), 0, 5);
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_DOC_INFO, buf, sizeof(struct carps_doc_info) + 5, stdout);
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
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, &params, sizeof(params), stdout);
	/* print params - toner save */
	params.magic = CARPS_PARAM_MAGIC;
	params.param = CARPS_PARAM_TONERSAVE;
	params.enabled = CARPS_PARAM_DISABLED;
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_PARAMS, &params, sizeof(params), stdout);
	/* print data header */
//	\x01.%@.P42;600;1J;ImgColor.\.[11h.[?7;600 I.[20't.[14;;;;;;p.[?2h.[1v.[600;1;0;32;;64;0'c
	buf[0] = 1;
	buf[1] = 0;
	strcat(buf, "\x1b%@");
	strcat(buf, "\x1bP42;600;1J;ImgColor");	/* 600 dpi */
	strcat(buf, "\x1b\\");
	strcat(buf, "\x1b[11h");
	strcat(buf, "\x1b[?7;600 I");
	strcat(buf, "\x1b[20't");	/* plain paper */
	strcat(buf, "\x1b[14;;;;;;p");
	strcat(buf, "\x1b[?2h");
	strcat(buf, "\x1b[1v");	/* 1 copy */
	strcat(buf, "\x1b[600;1;0;32;;64;0'c");
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, strlen(buf), stdout);

	if (!pbm_mode) {
		while (cupsRasterReadHeader2(ras, &page_header)) {
			/* setup this page */
			page++;
			fprintf(stderr, "PAGE: %d %d\n", page, page_header.NumCopies);

			line_len = page_header.cupsBytesPerLine;
			height = page_header.cupsHeight;
			width = page_header.cupsWidth;
			DBG("line_len=%d height=%d width=%d", line_len, height, width);

			/* read raster data */
			while (height > 0) {
				height -= encode_print_block(height, NULL, ras);
			}
			/* finish this page */
			/* end of page */
			u8 page_end[] = { 0x01, 0x0c };
			write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, page_end, sizeof(page_end), stdout);
		}
	} else {
		/* print data */
		while (!feof(f) && height > 0) {
			height -= encode_print_block(height, f, NULL);
		}
		/* end of page */
		u8 page_end[] = { 0x01, 0x0c };
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, page_end, sizeof(page_end), stdout);
	}
	if (pbm_mode)
		fclose(f);
	else
		cupsRasterClose(ras);
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

	return 0;
}

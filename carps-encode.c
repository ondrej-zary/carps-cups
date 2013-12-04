#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "carps.h"

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
	fprintf(stderr, "put_bits len=%d, pos=%d, n=%d, bits=%s\n", *len, *bitpos, n, bin_n(bits, n));
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
u8 last_lines[8][600], cur_line[600];
u16 line_pos;

int count_run_length(int pos) {
	int i;
	u8 first = cur_line[pos];

	for (i = pos + 1; i < line_len; i++)
		if (cur_line[i] != first)
			break;

	return i - pos;
}

int fls(unsigned int n) {
	int i = 0;

	while (n >>= 1)
		i++;

	return i;
}

void encode_number(char **data, u16 *len, u8 *bitpos, int num) {
	int num_bits; 
	fprintf(stderr, "encode_number(%d)\n", num);

	if (num == 1) {
		put_bits(data, len, bitpos, 2, 0b00);
		return;
	}

	num_bits = fls(num);
	fprintf(stderr, "num_bits=%d\n", num_bits);
	if (num_bits == 1)
		put_bits(data, len, bitpos, 2, 0b01);
	else {
		put_bits(data, len, bitpos, num_bits - 1, 0xff);
		put_bits(data, len, bitpos, 1, 0b0);
	}
	put_bits(data, len, bitpos, num_bits, ~num & MASK(num_bits));
}

void encode_prefix(char **data, u16 *len, u8 *bitpos, int num) {
	put_bits(data, len, bitpos, 8, 0b11111100);
	encode_number(data, len, bitpos, num / 128);
}

void encode_last_bytes(char **data, u16 *len, u8 *bitpos, int count) {
	if (count >= 128)
		encode_prefix(data, len, bitpos, count);
	count %= 128;
	put_bits(data, len, bitpos, 4, 0b1110);
	encode_number(data, len, bitpos, count);
}

u16 encode_print_data(int max_lines, FILE *f, char *out) {
	u8 bitpos = 0;
	u16 len = 0;
	int line_num = 0;
	fprintf(stderr, "max_lines=%d\n", max_lines);

	while (!feof(f) && line_num <= max_lines) {
		fread(cur_line, 1, line_len, f);
		fprintf(stderr, "line_num=%d\n", line_num);
		line_pos = 0;

		while (line_pos < line_len) {
			fprintf(stderr, "line_pos=%d: ", line_pos);
			if (line_pos > 0) {
				int tmp = count_run_length(line_pos - 1);
				fprintf(stderr, "run_len=%d\n", tmp);
				if (tmp > 1) {
					encode_last_bytes(&out, &len, &bitpos, tmp);
					line_pos += tmp;
					continue;
				}
			}

			/* zero byte */
			if (cur_line[line_pos] == 0x00) {
				put_bits(&out, &len, &bitpos, 8, 0b11111101);
				line_pos++;
				continue;
			}
			/* fallback: byte immediate */
			put_bits(&out, &len, &bitpos, 4, 0b1101);
			put_bits(&out, &len, &bitpos, 8, cur_line[line_pos]);
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
	}

	/* ending 0x80 byte */
//	out[0] = 0x80;

	return len;
}

void usage() {
	printf("usage: carps-encode <file.pbm>\n");
}


int main(int argc, char *argv[]) {
	char buf[BUF_SIZE];
	struct carps_doc_info *info;
	struct carps_print_params params;
	char tmp[100];
	int width, height;

	if (argc < 2) {
		usage();
		return 1;
	}

	FILE *f = fopen(argv[1], "r");
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
	fprintf(stderr, "width=%d height=%d\n", width, height);
	line_len = width / 8;

	/* document beginning */
	u8 begin_data[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_BEGIN, begin_data, sizeof(begin_data), stdout);
	/* document info - title */
	char *doc_title = "Untitled";
	info = (void *)buf;
	info->type = cpu_to_be16(CARPS_DOC_INFO_TITLE);
	info->unknown = cpu_to_be16(0x11);
	info->data_len = strlen(doc_title);
	memcpy(buf + sizeof(struct carps_doc_info), doc_title, strlen(doc_title));
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_DOC_INFO, buf, sizeof(struct carps_doc_info) + strlen(doc_title), stdout);
	/* document info - user name */
	char *user_name = "root";
	info = (void *)buf;
	info->type = cpu_to_be16(CARPS_DOC_INFO_USER);
	info->unknown = cpu_to_be16(0x11);
	info->data_len = strlen(user_name);
	memcpy(buf + sizeof(struct carps_doc_info), user_name, strlen(user_name));
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
	strcat(buf, "\x1b[[11h");
	strcat(buf, "\x1b[?7;600 I");
	strcat(buf, "\x1b[20't");	/* plain paper */
	strcat(buf, "\x1b[14;;;;;;p");
	strcat(buf, "\x1b[?2h");
	strcat(buf, "\x1b[1v");	/* 1 copy */
	strcat(buf, "\x1b[600;1;0;32;;64;0'c");
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, strlen(buf), stdout);
	/* print data */
	while (!feof(f)) {
		int num_lines = 65536 / line_len;
		int ofs = sprintf(buf, "\x01\x1b[;%d;%d;15.P", 4724, num_lines);
		u16 len = encode_print_data(num_lines, f, buf + ofs + sizeof(struct carps_print_header));
		struct carps_print_header *ph = (void *)buf + ofs;
		memset(ph, 0, sizeof(struct carps_print_header));
		ph->one = 0x01;
		ph->two = 0x02;
		ph->four = 0x04;
		ph->eight = 0x08;
		ph->magic = 0x50;
		ph->last = 1;
		ph->data_len = cpu_to_le16(len);
		write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, ofs + sizeof(struct carps_print_header) + len, stdout);
	}
	fclose(f);
	/* end of page */
	u8 page_end[] = { 0x01, 0x0c };
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, page_end, sizeof(page_end), stdout);
	/* end of print data */
	u8 print_data_end[] = { 0x01, 0x1b, 'P', '0', 'J', 0x1b, '\\' };
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, print_data_end, sizeof(print_data_end), stdout);
	/* end of print data */
	buf[0] = 1;
	write_block(CARPS_DATA_PRINT, CARPS_BLOCK_PRINT, buf, 1, stdout);
	/* end 2 */
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END2, NULL, 0, stdout);
	/* end 1 */
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END1, NULL, 0, stdout);
	/* end of document */
	buf[0] = 0;
	write_block(CARPS_DATA_CONTROL, CARPS_BLOCK_END, buf, 1, stdout);

	return 0;
}

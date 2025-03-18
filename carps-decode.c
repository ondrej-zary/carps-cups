/* CUPS driver for Canon CARPS printers - decoder */
/* Copyright (c) 2014 Ondrej Zary */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "carps.h"

void print_header(struct carps_header *header) {
	printf("magic1     = 0x%02x %s\n", header->magic1, (header->magic1 == 0xCD) ? "" : "!!!!!!!!");
	printf("magic2     = 0x%02x %s\n", header->magic2, (header->magic2 == 0xCA) ? "" : "!!!!!!!!");
	printf("magic3     = 0x%02x %s\n", header->magic3, (header->magic3 == 0x10) ? "" : "!!!!!!!!");
	printf("data_type  = 0x%02x %s\n", header->data_type, (header->data_type == 0x00 || header->data_type == 0x02) ? "" : "!!!!!!!!");
	printf("zero1      = 0x%02x %s\n", header->zero1, (header->zero1 == 0x00) ? "" : "!!!!!!!!");
	printf("block_type = 0x%02x ", header->block_type);
	if (header->block_type < 0x11 || header->block_type > 0x1a || header->block_type == 0x15)
		printf("!!!!!!!!");
	printf("\n");
	printf("zero2      = 0x%02x %s\n", header->zero2, (header->zero2 == 0x00) ? "" : "!!!!!!!!");
	printf("one        = 0x%02x %s\n", header->one, (header->one == 0x01) ? "" : "!!!!!!!!");
	printf("data_len   = 0x%04x\n", be16_to_cpu(header->data_len));
	printf("empty[10]");
	for (int i = 0; i < 10; i++)
		if (header->empty[i] != 0x00)
			printf("!!!!!!!!");
	printf("\n");
}

void dump_data(u8 *data, u16 len) {
	for (int i = 0; i < len; i++)
		printf("%02hhx ", data[i]);
	printf("\n");
}

void print_time(struct carps_time *time) {
	char *weekday[] = { "", "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
	printf("Time: %04d-%02d-%02d (%s) %02d:%02d:%02d.%d\n",
		(time->year << 4) | (time->year_month >> 4),
		time->year_month & 0x0f,
		time->day >> 3,
		weekday[time->day & 0x7],
		time->hour,
		time->min,
		time->sec_msec >> 2,
		((time->sec_msec & 0x3) << 8) | time->msec);
}

long block_pos;
enum carps_compression compression = COMPRESS_CANON;

#define NO_HEADER	(1 << 0)
int get_block(u8 *buf, FILE *f, int flags) {
	u8 *data;
	static int i = 0;

	if (fread(buf, 1, sizeof(struct carps_header), f) != sizeof(struct carps_header)) {
		if (feof(f)) {
			printf("EOF\n");
			return -1;
		}
		perror("Error reading file");
		return -3;
	}

	if (0) {
		printf("========== BLOCK %d ==========\n", i++);
		print_header((struct carps_header *)buf);
	} else {
		struct carps_header *header = (void *)buf;
		printf("BLOCK %d (len=%d): ", i++, be16_to_cpu(header->data_len));
	}

	struct carps_header *header = (void *)buf;
	u16 len = be16_to_cpu(header->data_len);

	if (flags & NO_HEADER) {
		data = buf;
		fread(data, 1, 1, f);	/* discard the first 0x01 byte */
		if (data[0] != 0x01)
			printf("Invalid data in print block - first byte 0x%02x, expected 0x01\n", data[0]);
		len -= 1;
	} else
		data = buf + sizeof(struct carps_header);

	block_pos = ftell(f);

	if (fread(data, 1, len, f) != len) {
		perror("Error reading file");
		return -3;
	}

	return len;
}

/* get n bits of data */
u8 get_bits(u8 **data, u16 *len, u8 *bitpos, u8 n) {
	u8 bits = 0;
	u8 byte;

	for (int i = 0; i < n; i++) {
		if (*len == 0) {
			printf("%s DATA UNDERFLOW\n", bin_n(bits, i));
			return 0;
		}
		byte = *data[0] ^ PRINT_DATA_XOR;
		bits <<= 1;
		bits |= ((byte << *bitpos) & 0x80) >> 7;
		(*bitpos)++;
		if (*bitpos > 7) {
			(*data)++;
			(*len)--;
			*bitpos = 0;
		}
	}
	printf("%s ", bin_n(bits, n));

	return bits;
}

void go_backward(int num_bits, u8 **data, u16 *len, u8 *bitpos) {
	if (*bitpos >= num_bits)
		*bitpos -= num_bits;
	else {
		(*data)--;
		(*len)++;
		*bitpos = 8 - (num_bits - *bitpos);
	}
}

int count_ones(int max, u8 **data, u16 *len, u8 *bitpos) {
	int i = 0;

	while (i < max && get_bits(data, len, bitpos, 1))
		i++;

	return i;
}

/* decode a number beginning with 00, 01, 10, 110, 1110, 11110, 111110 */
int decode_number(u8 **data, u16 *len, u8 *bitpos) {
	int num_bits;
	printf("decode_number ");

	num_bits = count_ones(6, data, len, bitpos) + 1;
	if (num_bits == 7)
		return 0;

	if (num_bits == 1) {
		if (get_bits(data, len, bitpos, 1))
			num_bits = 1;
		else
			return 1;
	}

	return (1 << num_bits) + (~get_bits(data, len, bitpos, num_bits) & MASK(num_bits));
}

u8 *last_lines[8], *cur_line;
int out_bytes;
u16 line_num, line_pos, line_len;
bool output_header;
long height_pos;

void next_line(void) {
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

void output_byte(u8 byte, u8 *buf, FILE *fout) {
	printf("DICTIONARY=");
	for (int j = 0; j < DICT_SIZE; j++)
		printf("%02X ", buf[j]);
	printf("\n");

	for (int i = 0; i < DICT_SIZE; i++)
		if (buf[i] == byte) {
			memmove(buf + i, buf + i + 1, DICT_SIZE - i);
			break;
		}
	memmove(buf + 1, buf, DICT_SIZE - 1);
	buf[0] = byte;
	fwrite(&byte, 1, 1, fout);
	cur_line[line_pos] = byte;
	printf("BYTE=%x\n", byte);
	out_bytes++;
	line_pos++;
	if (line_pos >= line_len)
		next_line();
}

void output_bytes_last(int count, int offset, FILE *fout) {
	for (int i = 0; i < count; i++) {
		u8 byte;
		if (line_pos - offset < 0)
			byte = last_lines[0][line_len - offset];
		else
			byte = cur_line[line_pos - offset];
		printf("%02x ", byte);
		fwrite(&byte, 1, 1, fout);
		cur_line[line_pos] = byte;
		line_pos++;
	}
	printf("\n");
	out_bytes += count;
	if (line_pos >= line_len)
		next_line();
}

void output_previous(int line, int count, FILE *fout) {
	printf("previous (line=%d): ", line);
	for (int i = 0; i < count; i++)
		printf("%02x ", last_lines[line][line_pos + i]);
	printf("\n");
	fwrite(last_lines[line] + line_pos, 1, count, fout);
	memcpy(cur_line + line_pos, last_lines[line] + line_pos, count);

	out_bytes += count;
	line_pos += count;
	if (line_pos >= line_len)
		next_line();
}

#define TMP_BUFLEN 100

int decode_print_data(u8 *data, u16 len, FILE *f, FILE **fout) {
	bool in_escape = false;
	static bool start_of_strip = true;
	int i;
	int count;
	int base = 0;
	u8 dictionary[DICT_SIZE];
	bool twobyte_flag = false, prev8_flag = false;
	char tmp[TMP_BUFLEN];
	static int width, page = 1;
	int height;
	char filename[30];

	u8 *start = data;

	if (data[0] != 0x01)
		printf("!!!!!!!!");

	if (len == 2 && data[1] == 0x0c) {
		printf("end of page\n");
		start_of_strip = true;
		/* now we know line count so we can fill it in */
		if (compression == COMPRESS_CANON && output_header) {
			fseek(*fout, height_pos, SEEK_SET);
			fprintf(*fout, "%4d", line_num);
		}
		fclose(*fout);
		*fout = NULL;
		page++;
		line_num = 0;
		return 0;
	}

	/* read and display escape sequences at start of strip */
	for (i = 1; start_of_strip && i < len; i++) {
		if (data[i] == ESC) {	/* escape sequence begin */
			if (in_escape)
				printf("\n");
			in_escape = true;
			printf("ESC");
			continue;
		} else if (i == 1)	/* data begins immediately */
			break;
		if (isprint(data[i]))
			printf("%c", data[i]);
		else
			break;
	}

	if (!strncmp((char *)data + 1, "\x1b[;", 3)) {
		if (i > TMP_BUFLEN) {
			printf("ESC sequence too long!\n");
			return 1;
		}
		strncpy(tmp, (char *)data + 3, i);
		tmp[i] = '\0';
		int comp;
		sscanf(tmp, ";%d;%d;%d.", &width, &height, &comp);
		printf(" width=%d, height=%d, compression=%d\n", width, height, comp);
		if (comp != COMPRESS_CANON && comp != COMPRESS_G4)
			printf("UNKNOWN COMPRESSION TYPE!!!!!!!!\n");
		compression = comp;
		if (compression == COMPRESS_CANON) {
			line_len = ROUND_UP_MULTIPLE(DIV_ROUND_UP(width, 8), 4);
			printf("line_len=%d\n", line_len);
			cur_line = realloc(cur_line, line_len);
			for (int i = 0; i < 8; i++)
				last_lines[i] = realloc(last_lines[i], line_len);
		}
	}

	data += i;
	len -= i;

	if (!*fout && len > 0) {
		snprintf(filename, sizeof(filename), "decoded-p%d.%s", page, (compression == COMPRESS_CANON) ? "pbm" : "g4");
		printf("\ncreating output file %s", filename);
		if (compression == COMPRESS_G4)
			printf(" - use 'fax2tiff -4 -8 -X %d %s -o decoded-p%d.tiff' to convert", width, filename, page);
		printf("\n");
		*fout = fopen(filename, "w");
		if (!*fout) {
			perror("Unable to open output file");
			return 2;
		}
		if (compression == COMPRESS_CANON && output_header) {
			fprintf(*fout, "P4\n%d ", line_len * 8);
			height_pos = ftell(*fout);
			fprintf(*fout, "%4d\n", 0); /* we don't know height yet */
		}
	}

	if (len > 0)
		start_of_strip = false;

	if (compression == COMPRESS_G4 && len > 0) {
		printf("%d bytes of G4 data\n", len);
		fwrite(data, 1, len, *fout);
		return 0;
	}
	if (len < sizeof(struct carps_print_header)) {
		printf("\n");
		return -1;
	}

	memset(dictionary, 0xaa, DICT_SIZE);

	struct carps_print_header *header = (void *)data;
	if (header->one != 0x01 || header->two != 0x02 || header->four != 0x04 || header->eight != 0x08 || header->zero1 != 0x0000 || header->magic != 0x50
			|| header->zero2 != 0x00) {
		printf("!!!!!!!");
/*		printf("one=0x%02x\n", header->one);
		printf("two=0x%02x\n", header->two);
		printf("four=0x%02x\n", header->four);
		printf("eight=0x%02x\n", header->eight);
		printf("zero1=0x%04x\n", header->zero1);
		printf("magic=0x%02x\n", header->magic);
		printf("zero2=0x%02x\n", header->zero2);
		printf("data_len=0x%04x\n", le16_to_cpu(header->data_len));
		printf("zero3=0x%04x\n", header->zero3);*/
	} else {
		printf("Data length: %d ", le16_to_cpu(header->data_len));
		data += sizeof(struct carps_print_header);
		len  -= sizeof(struct carps_print_header);
		u8 *data2 = data + len;
		while (len < le16_to_cpu(header->data_len)) {
			int ret;
			printf("we have only %d bytes: reading next block\n", len);
			ret = get_block(data2, f, NO_HEADER);
			if (ret < 0)
				return ret;
			else {
				len += ret;
				data2 += ret;
			}
		}
		printf("ok, we have %d bytes\n", len);
	}
	printf("len=%d", len);
	printf("\n");

	u8 bitpos = 0;

	while (len) {
		printf("out_pos: 0x%x, line_num=%d, line_pos=%d (%d), len=%d, in_pos=0x%x ", out_bytes, line_num, line_pos, line_pos * 8, len, block_pos + data - start);

		u8 bits = get_bits(&data, &len, &bitpos, 1);
		if (bits) { /* 1 */
			bits = get_bits(&data, &len, &bitpos, 1);
			if (bits) { /* 11 */
				bits = get_bits(&data, &len, &bitpos, 2);
				switch (bits) {
				case 0b11: /* 1111 */
					bits = get_bits(&data, &len, &bitpos, 1);
					if (bits) { /* 11111 */
						bits = get_bits(&data, &len, &bitpos, 1);
						if (bits) { /* 111111 */
							bits = get_bits(&data, &len, &bitpos, 2);
							switch (bits) {
							case 0b01:
								printf("zero byte\n");
								output_byte(0, dictionary, *fout);
								break;
							case 0b00:
								count = decode_number(&data, &len, &bitpos);
								printf("PREFIX %d\n", count * 128);
								base = count * 128;
								break;
							case 0b10:
								printf("strip end marker\n");
								start_of_strip = true;
								return 0;
							default:
								printf("!!!!!!!! 0b%s\n", bin_n(bits, 2));
							}
						} else { /* 111110 */
							go_backward(4, &data, &len, &bitpos);
							twobyte_flag = !twobyte_flag;
							printf("twobyte_flag := %d\n", twobyte_flag);
						}
					} else { /* 11110 */
						count = decode_number(&data, &len, &bitpos);
						printf("%d bytes from this line [@-80]\n", count);
						output_bytes_last(count, 80, *fout);
					}
					break;
				case 0b01: /* 1101 */
					bits = get_bits(&data, &len, &bitpos, 8);
					printf("byte immediate 0b%s\n", bin_n(bits, 8));
					output_byte(bits, dictionary, *fout);
					break;
				case 0b00: /* 1100 */
					go_backward(1, &data, &len, &bitpos);
					prev8_flag = !prev8_flag;
					printf("prev8_flag := %d\n", prev8_flag);
					break;
				case 0b10: /* 1110 */
					count = decode_number(&data, &len, &bitpos);
					printf("%d last bytes (+%d)\n", count + base, base);
					output_bytes_last(count + base, twobyte_flag ? 2 : 1, *fout);
					base = 0;
					break;
				}
			} else { /* 10 */
				/* DICTIONARY */
				bits = get_bits(&data, &len, &bitpos, 4);
				printf("[%d] byte from dictionary\n", (~bits & 0b1111));
				output_byte(dictionary[(~bits & 0b1111)], dictionary, *fout);
			}
		} else { /* 0 */
			count = decode_number(&data, &len, &bitpos);
			printf("%d bytes from previous line (+%d)\n", count + base, base);
			output_previous(prev8_flag ? 7 : 3, count + base, *fout);
			base = 0;
		}
	}

	printf("\n");

	return 0;
}

void usage() {
	printf("usage: carps-decode <file> [--header]\n");
}

int main(int argc, char *argv[]) {
	u8 buf[BUF_SIZE];
	struct carps_header *header = (void *)buf;
	u8 *data = buf + sizeof(struct carps_header);
	int ret;
	u16 len;
	FILE *fout = NULL;

	if (argc < 2) {
		usage();
		return 1;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror("Unable to open file");
		return 2;
	}

	if (argc > 2 && !strcmp(argv[2], "--header"))
		output_header = true;

	while (!feof(f)) {
		ret = get_block(buf, f, 0);
		if (ret < 0)
			break;
		else
			len = ret;

		switch (header->block_type) {
		case CARPS_BLOCK_BEGIN: {
			printf("DOCUMENT BEGINNING ");
			u8 begin_data[] = { 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
			if (len != sizeof(begin_data) || memcmp(data, begin_data, sizeof(begin_data)))
				printf("!!!!!!!!");
			printf("\n");
			break;
		}
		case CARPS_BLOCK_DOC_INFO: {
			struct carps_doc_info *info = (void *)data;
			printf("DOCUMENT INFORMATION: ");
			u16 type = be16_to_cpu(info->type);
			u16 unknown = be16_to_cpu(info->unknown);
			switch (type) {
			case CARPS_DOC_INFO_TITLE:
				if (unknown != 0x11)
					printf("!!!!!!!! ");
				data[sizeof(struct carps_doc_info) + info->data_len] = '\x0';
				printf("Title: '%s'\n", data + sizeof(struct carps_doc_info));
				break;
			case CARPS_DOC_INFO_USER:
				if (unknown != 0x11)
					printf("!!!!!!!! ");
				data[sizeof(struct carps_doc_info) + info->data_len] = '\x0';
				printf("User: '%s'\n", data + sizeof(struct carps_doc_info));
				break;
			case CARPS_DOC_INFO_TIME:
				print_time((void *)data + 2);
				break;
			default:
				printf("Unknown: type=0x%x, unknown=0x%x, data_len=0x%x\n", type, unknown, info->data_len);
				break;
			}
			break;
		}
		case CARPS_BLOCK_DOC_INFO_NEW: {
			printf("DOCUMENT INFORMATION (NEW TYPE): ");
			u16 record_count = be16_to_cpu(*(u16 *)data);
			printf("%d records\n", record_count);
			u8 *record = data + 2;
			for (int i = 0; i < record_count; i++) {
				printf(" #%d: ", i + 1);
				struct carps_doc_info_new *info = (void *)record;
				u16 type = be16_to_cpu(info->type);
				u16 data_len = be16_to_cpu(info->data_len);
				switch (type) {
					case CARPS_DOC_INFO_TITLE: {
						u16 unknown = be16_to_cpu(*(u16 *)info->data);
						if (unknown != 0x11)
							printf("!!!!!!!! ");
						record[sizeof(struct carps_doc_info_new) + info->data_len + 3] = '\x0';
						printf("Title: '%s'\n", record + sizeof(struct carps_doc_info_new) + 3);
						break;
					}
					case CARPS_DOC_INFO_USER: {
						u16 unknown = be16_to_cpu(*(u16 *)info->data);
						if (unknown != 0x11)
							printf("!!!!!!!! ");
						record[sizeof(struct carps_doc_info_new) + info->data_len + 3] = '\x0';
						printf("User: '%s'\n", record + sizeof(struct carps_doc_info_new) + 3);
						break;
					}
					case CARPS_DOC_INFO_TIME:
						print_time((void *)info->data);
						break;
					default:
						printf("Unknown: type=0x%x, data_len=0x%x: ", type, data_len);
						dump_data(info->data, data_len);
						break;
				}
				record += sizeof(struct carps_doc_info_new) + data_len;
			}
			break;
		}
		case CARPS_BLOCK_END:
			printf("DOCUMENT END ");
			if (len != 1 || data[0] != 0x00)
				printf("!!!!!!!!");
			printf("\n");
			break;
		case CARPS_BLOCK_BEGIN1:
			printf("BEGIN SOMETHING 1 ");
			if (len != 4 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 || data[3] != 0x00)
				printf("!!!!!!!!");
			printf("\n");
			break;
		case CARPS_BLOCK_END1:
			printf("END SOMETHING 1 ");
			if (len != 0)
				printf("!!!!!!!!");
			printf("\n");
			break;
		case CARPS_BLOCK_BEGIN2:
			printf("BEGIN SOMETHING 2 ");
			if (len != 4 || data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x00 || data[3] != 0x00)
				printf("!!!!!!!!");
			printf("\n");
			break;
		case CARPS_BLOCK_PARAMS: {
			struct carps_print_params *params = (void *)data;
			printf("PRINT PARAMETERS: ");
			if (params->magic != CARPS_PARAM_MAGIC)
				printf("!!!!!!!! ");
			switch (params->param) {
			case CARPS_PARAM_IMAGEREFINE:
				printf("Image refinement: ");
				break;
			case CARPS_PARAM_TONERSAVE:
				printf("Toner save: ");
				break;
			default:
				printf("Unknown param=0x%02x", params->param);
				dump_data(data, len);
				continue;
			}
			if (params->enabled == CARPS_PARAM_DISABLED)
				printf("disabled\n");
			else if (params->enabled == CARPS_PARAM_ENABLED)
				printf("enabled\n");
			else
				printf("invalid value 0x%02x\n", params->enabled);
			break;
		}
		case CARPS_BLOCK_END2:
			printf("END SOMETHING 2 ");
			if (len != 0)
				printf("!!!!!!!!");
			printf("\n");
			break;
		case CARPS_BLOCK_PRINT:
			printf("PRINT DATA 0x%02x ", data[0]);
			decode_print_data(data, len, f, &fout);
			break;
		default:
			printf("UNKNOWN BLOCK 0x%02x !!!!!!!!\n", header->block_type);
			dump_data(data, len);
			break;
		}
	}

	free(cur_line);
	for (int i = 0; i < 8; i++)
		free(last_lines[i]);

	fclose(f);
	return 0;
}

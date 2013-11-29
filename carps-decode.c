#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
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

void dump_data_dexor(u8 *data, u16 len) {
	for (int i = 0; i < len; i++)
		printf("%02hhx ", data[i] ^ PRINT_DATA_XOR);
	printf("\n");
}

const char *bin_n(u16 x, u8 n) {
    static char b[9];
    b[0] = '\0';

    for (u16 i = 1 << (n - 1); i > 0; i >>= 1)
        strcat(b, (x & i) ? "1" : "0");

    return b;
}

const char *bin(u8 x) {
    return bin_n(x, 8);
}

#define NO_HEADER 	(1 << 0)
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
		printf("BLOCK %d: ", i++);
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
//		printf("i=%d, *data=%p, *len=%d, *bitpos=%d\n", i, *data, *len, *bitpos);
		byte = *data[0] ^ PRINT_DATA_XOR;
		bits <<= 1;
		bits |= ((byte << *bitpos) & 0x80) >> 7;
		(*bitpos)++;
		if (*bitpos > 7) {
//			printf("input=0x%02x ", *data[0]);
			(*data)++;
			(*len)--;
			*bitpos = 0;
		}
	}
	printf("%s ", bin_n(bits, n));
//	printf("%s (len=%d)", bin_n(bits, n), *len);

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

/* decode repeat stream beginning with 00, 01, 10, 110, 1110, 11110, 111110 */
int decode_repeat_stream(u8 **data, u16 *len, u8 *bitpos, int base) {
	u8 bits = get_bits(data, len, bitpos, 2);
	printf("decode_repeat_stream base=%d ", base);
	switch (bits) {
	case 0b01:
		bits = get_bits(data, len, bitpos, 1);
		printf("%d bytes ", base + 2 + (~bits & 0b1));
		return base + 2 + (~bits & 0b1);
		break;
	case 0b10:
		bits = get_bits(data, len, bitpos, 2);
		printf("%d bytes ", base + 4 + (~bits & 0b11));
		return base + 4 + (~bits & 0b11);
		break;
	case 0b11:
		bits = get_bits(data, len, bitpos, 1);
		if (bits) {
			bits = get_bits(data, len, bitpos, 1);
			if (bits) {
				bits = get_bits(data, len, bitpos, 1);
				if (bits) {
					bits = get_bits(data, len, bitpos, 1);
					if (bits) {
						printf("%d bytes ", base);
						return base;
					} else {//111110
						bits = get_bits(data, len, bitpos, 6);
						printf("%d bytes ", base+64+(~bits & 0b111111));
						return base + 64 + (~bits & 0b111111);
					}
				} else {//11110
					bits = get_bits(data, len, bitpos, 5);
					printf("%d bytes ", base+32+(~bits & 0b11111));
					return base + 32 + (~bits & 0b11111);
				}
			} else {//1110
				bits = get_bits(data, len, bitpos, 4);
				printf("%d bytes ", base+16+(~bits & 0b1111));
				return base + 16 + (~bits & 0b1111);
			}
		} else {//110
			bits = get_bits(data, len, bitpos, 3);
			printf("%d bytes ", base+8+(~bits & 0b111));
			return base + 8 + (~bits & 0b111);
		}
		break;
	case 0b00:
		printf("%d bytes ", base + 1);
		return base + 1;
	}

	return 0;
}

#define DECODE_BUFSIZE 16
u8 lastbyte = 0;//////remove
u8 last_lines[8][600], cur_line[600];
//, last_line2[600], last_line3[600], last_line4[600], last_line5[600], last_line6[600], last_line7[600], last_line8[600];
int out_bytes = 0;
u16 line_num = 0;
u16 line_pos;
u16 line_len = 591;
bool prev8_flag = false;
bool twobyte_flag = false;

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

void buffer_push(u8 *buf, u8 data) {
//	printf("buffer_push %x\n", data);
	memmove(buf + 1, buf, DECODE_BUFSIZE - 1);
	buf[0] = data;
}

void output_byte(u8 byte, u8 *buf, FILE *fout) {
//	int present = 0;

	printf("DICTIONARY=");
	for (int j = 0; j < DECODE_BUFSIZE; j++)
		printf("%02X ", buf[j]);
	printf("\n");

	for (int i = 0; i < DECODE_BUFSIZE; i++)
		if (buf[i] == byte) {
			memmove(buf + i, buf + i + 1, DECODE_BUFSIZE - i);
			break;
		}
	buffer_push(buf, byte);
	fwrite(&byte, 1, 1, fout);
	cur_line[line_pos] = byte;
	printf("BYTE=%x\n", byte);
	lastbyte = byte;
	out_bytes++;
	line_pos++;
	if (line_pos > line_len)
		next_line();
}

void output_bytes_repeat(int count, u8 *buf, FILE *fout) {
	printf("BYTES=");
	for (int i = 0; i < count; i++) {
		fwrite(buf, 1, 1, fout);
		cur_line[line_pos] = buf[0];
		line_pos++;
		printf("%02x ", buf[0]);
	}
	printf("\n");
	lastbyte = cur_line[line_pos - 1];
	out_bytes += count;
	if (line_pos > line_len)
		next_line();
}

void output_bytes_last2(int count, u8 *buf, FILE *fout) {
	for (int i = 0; i < count; i++) {
		printf("%02x ", cur_line[line_pos - 2]);
		fwrite(&cur_line[line_pos - 2], 1, 1, fout);
		cur_line[line_pos] = cur_line[line_pos - 2];
		line_pos++;
	}
	printf("\n");
	out_bytes += count;
	lastbyte = cur_line[line_pos - 1];
	if (line_pos > line_len)
		next_line();
}

void output_previous(int line, int count, FILE *fout) {
	if (prev8_flag) {
		printf("line += 4\n");
		line += 4;
	}

	printf("previous (line=%d): ", line);
	for (int i = 0; i < count; i++)
		printf("%02x ", last_lines[line][line_pos + i]);
	printf("\n");
	fwrite(last_lines[line] + line_pos, 1, count, fout);
	memcpy(cur_line + line_pos, last_lines[line] + line_pos, count);

//		u8 tmp[600];
//		memcpy(tmp, last_lines[line], line_len);
//		for (int i = 0; i < ((line < 7) ? line : 7); i++)
//			memcpy(last_lines[i+1], last_lines[i], line_len);
//		memcpy(last_lines[0], tmp, line_len);


	out_bytes += count;
	line_pos += count;
	lastbyte = cur_line[line_pos - 1];
	if (line_pos > line_len)
		next_line();
}

int decode_print_data(u8 *data, u16 len, FILE *f, FILE *fout) {
	bool in_escape = false;
	int i;
	int count;
	u8 decode_buf[DECODE_BUFSIZE];
//	u8 decode_bufpos = 0;
	memset(decode_buf, 0xaa, DECODE_BUFSIZE);
	
	if (data[0] != 0x01)
		printf("!!!!!!!!");

	for (i = 1; i < len; i++) {
		if (data[i] == ESC) {	/* escape sequence begin */
			if (in_escape) {
				printf("\n");
			}
			in_escape = true;
			printf("ESC");
			continue;
		} else if (i == 1)	/* data begins immediately */
			break;
		if (isprint(data[i]))
			printf("%c",data[i]);
		else
			break;
	}
	data += i;
	len -= i;
	if (len < sizeof(struct carps_print_header)) {
		printf("\n");
		return -1;
	}
////////////
//	printf("\n");
//	return;
//////////
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
		printf("data_len=0x%04x\n", header->data_len);
		printf("zero3=0x%04x\n", header->zero3);*/
	} else {
		printf("Data length: %d ", header->data_len);
		data += sizeof(struct carps_print_header);
		len  -= sizeof(struct carps_print_header);
		u8 *data2 = data + len;
		while (len < header->data_len) {
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
//	printf("\n");
//	dump_data_dexor(data, len);
	printf("\n");

	u8 bitpos = 0;
/*	printf("%s ", bin(data[0] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[1] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[2] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[3] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[4] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[5] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[6] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[7] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[8] ^ PRINT_DATA_XOR));
	printf("%s ", bin(data[9] ^ PRINT_DATA_XOR));
	printf("\n");*/

	while (len) {
		printf("out_pos: 0x%x, line_num=%d, line_pos=%d (%d), len=%d ", out_bytes, line_num, line_pos, line_pos * 8, len);

		u8 bits = get_bits(&data, &len, &bitpos, 4);

		switch (bits) {
		case 0b1111:
			bits = get_bits(&data, &len, &bitpos, 4);
			switch (bits) {
			case 0b1101:
				printf("zero byte\n");
				output_byte(0, decode_buf, fout);
				break;
			case 0b1100:
				bits = get_bits(&data, &len, &bitpos, 2);
				switch (bits) {
				case 0b00:
					bits = get_bits(&data, &len, &bitpos, 4);
					switch (bits) {
					case 0b1110:
						count = decode_repeat_stream(&data, &len, &bitpos, 128);
						printf("%d repeating bytes (+128)\n", count);
						output_bytes_repeat(count, &lastbyte, fout);
						break;
					case 0b0111:
						go_backward(3, &data, &len, &bitpos);
						count = decode_repeat_stream(&data, &len, &bitpos, 128);
						printf("%d repeating bytes (+128 #2)\n", count);
						output_bytes_repeat(count, &lastbyte, fout);
						break;
					default:
						printf("invalid bits 0b%s\n", bin_n(bits, 4));
					}
					break;
				case 0b01:
					bits = get_bits(&data, &len, &bitpos, 5);
					switch (bits) {
					case 0b11110:
//BAD!!						go_backward(6, &data, &len, &bitpos);
//						count = decode_repeat_stream(&data, &len, &bitpos, 192);
						count = decode_repeat_stream(&data, &len, &bitpos, 256);
						printf("%d repeating bytes\n", count);
						output_bytes_repeat(count, &lastbyte, fout);
						break;
					case 0b01110:
						count = decode_repeat_stream(&data, &len, &bitpos, 384);
						printf("%d repeating bytes\n", count);
						output_bytes_repeat(count, &lastbyte, fout);
						break;
					case 0b00111:
						go_backward(3, &data, &len, &bitpos);
						count = decode_repeat_stream(&data, &len, &bitpos, 384);
/*						bits = get_bits(&data, &len, &bitpos, 2);
						switch (bits) {
						case 0b10:
							bits = get_bits(&data, &len, &bitpos, 5);
							count = 416 + (~bits & 0b11111);
							break;
						case 0b11:
							bits = get_bits(&data, &len, &bitpos, 7);
							count = 384 + (~bits & 0b1111111);
							break;
						case 0b00:
							bits = get_bits(&data, &len, &bitpos, 3);
							count = 400 + (~bits & 0b1111);
							break;
						default:
							printf("!!!!!!!!!!!!!!!!!\n");
							break;
						}*/
						printf("%d bytes from previous line (4,5,7bit)\n", count);
						output_previous(3, count, fout);
						break;
					case 0b01100:
						bits = get_bits(&data, &len, &bitpos, 6);
						if (bits == 0b111110) {
							bits = get_bits(&data, &len, &bitpos, 6);
							count =  448 + (~bits & 0b111111);
							printf("WTF???? %d bytes from previous line (+448)\n", count);
							prev8_flag = !prev8_flag;
							printf("prev8_flag := %d\n", prev8_flag);
							output_previous(3, count, fout);
						} else
							printf("!!!!!!!!\n");
						break;
					case 0b01111:
						go_backward(4, &data, &len, &bitpos);
						printf("?????? 384 repeating bytes\n");
						output_bytes_repeat(384, &lastbyte, fout);
						break;
					case 0b10111:
					case 0b10110:
					case 0b10100:
					case 0b10101:
					case 0b10010:
						go_backward(3, &data, &len, &bitpos);
						count = decode_repeat_stream(&data, &len, &bitpos, 256);
						printf("%d bytes from previous line (+256)??????\n", count);
						output_previous(3, count, fout);
						break;
					case 0b11100:////flag????????////////////////////////////////////
//						go_backward(7, &data, &len, &bitpos);
						count = decode_repeat_stream(&data, &len, &bitpos, 256);
						printf("%d bytes from previous line (+256 w/flag)??????\n", count);
						prev8_flag = !prev8_flag;
						printf("prev8_flag := %d\n", prev8_flag);
						output_previous(3, count, fout);
						break;
					default:
						printf("invalid bits 0b%s\n", bin_n(bits, 5));
					}
					break;
				case 0b10:
					bits = get_bits(&data, &len, &bitpos, 6);
					if (bits == 0b111110) {
						count = decode_repeat_stream(&data, &len, &bitpos, 512);
						printf("%d repeating bytes (+512)\n", count);
						output_bytes_repeat(count, &lastbyte, fout);
					} else if (bits == 0b110111 || bits == 0b110110 || bits == 0b110100 || bits == 0b110101 || bits == 0b110010 || bits == 0b110011 || bits == 0b110000 || bits == 0b110001) {
						go_backward(3, &data, &len, &bitpos);
						count = decode_repeat_stream(&data, &len, &bitpos, 512);
						printf("%d bytes from previous line [3] (+512)\n", count);
						output_previous(3, count, fout);
					} else if (bits == 0b111100) {
						prev8_flag = !prev8_flag;
						printf("prev8_flag := %d\n", prev8_flag);
						count = decode_repeat_stream(&data, &len, &bitpos, 512);
						printf("%d bytes from previous line [3] (+512 w/flag)\n", count);
						output_previous(3, count, fout);
					} else
						printf("invalid bits 0b%s ", bin_n(bits, 6));
					break;
				default:
					printf("invalid bits 0b%s\n", bin_n(bits, 2));
					break;
				}
				break;
			case 0b1011:
			case 0b1010:
			case 0b1001:
				go_backward(6, &data, &len, &bitpos);
				printf("TWOBYTE FLAG\n");
				twobyte_flag = !twobyte_flag;
				printf("twobyte_flag := %d\n", twobyte_flag);
				break;
			case 0b0010:
			case 0b0011:
			case 0b0100:
			case 0b0101:
			case 0b0110:
			case 0b0111:
				go_backward(3, &data, &len, &bitpos);
				count = decode_repeat_stream(&data, &len, &bitpos, 0);

				memcpy(&cur_line[line_pos], &cur_line[line_pos - 80], count);
				fwrite(&cur_line[line_pos - 80], 1, count, fout);
				out_bytes += count;
				line_pos += count;
				lastbyte = cur_line[line_pos - 1];
				
				printf("%d bytes from this line [@-80]\n", count);
				break;
			case 0b1110:
				printf("block end marker??? ");
//				bits = get_bits(&data, &len, &bitpos, 8);
//				bits = get_bits(&data, &len, &bitpos, 8);
				prev8_flag = 0;
				twobyte_flag = 0;
				printf("\n");
				return 0;
			default:
				printf("!!!!!!!! 0b%s\n", bin_n(bits, 4));
			}
			break;
		case 0b1101:
			bits = get_bits(&data, &len, &bitpos, 8);
			printf("byte immediate 0b%s\n", bin(bits));
			output_byte(bits, decode_buf, fout);
			break;
		case 0b1110:
			count = decode_repeat_stream(&data, &len, &bitpos, 0);
			if (twobyte_flag) {
				printf("%d last bytes (by 2)\n", count);
				output_bytes_last2(count, &lastbyte, fout);
			} else {
				printf("%d repeating bytes (by 1)\n", count);
				output_bytes_repeat(count, &lastbyte, fout);
			}
			break;
		/* DICTIONARY */
		case 0b1000:
		case 0b1001:
		case 0b1010:
		case 0b1011:
			go_backward(2, &data, &len, &bitpos);
			bits = get_bits(&data, &len, &bitpos, 4);
			printf("[%d] byte from dictionary\n", (~bits & 0b1111));
			output_byte(decode_buf[(~bits & 0b1111)], decode_buf, fout);
			break;
		/* ????? */
		case 0b0101:
			go_backward(2, &data, &len, &bitpos);
			count = decode_repeat_stream(&data, &len, &bitpos, 2);
			printf("%d bytes from previous line (3bit #2)\n", count);
			output_previous(3, count, fout);
			break;
		case 0b0010:
		case 0b0011:
		case 0b0100:
		case 0b0110:
		case 0b0111:
			go_backward(3, &data, &len, &bitpos);
			count = decode_repeat_stream(&data, &len, &bitpos, 0);
			printf("%d bytes from previous line (short)\n", count);
			output_previous(3, count, fout);
			break;
		case 0b1100:
			go_backward(1, &data, &len, &bitpos);
			printf("PREV8 FLAG???\n");
			prev8_flag = !prev8_flag;
			printf("prev8_flag := %d\n", prev8_flag);
			break;
		default:
			printf("unknown prefix 0b%s\n", bin_n(bits, 4));
			break;
		}
	}

	printf("\n");

 	return 0;
}

void usage() {
	printf("usage: carps-decode <file>\n");
}


int main(int argc, char *argv[]) {
	u8 buf[BUF_SIZE];
	struct carps_header *header = (void *)buf;
	u8 *data = buf + sizeof(struct carps_header);
	int ret;
	u16 len;

	if (argc < 2) {
		usage();
		return 1;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror("Unable to open file");
		return 2;
	}
	FILE *fout = fopen("decoded.pbm", "w");
	if (!fout) {
		perror("Unable to open output file");
		return 2;
	}
	
	while (!feof(f)) {
		ret = get_block(buf, f, 0);
		if (ret < 0)
			return ret;
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
			if (unknown != 0x11)
				printf("!!!!!!!! ");
			switch (type) {
			case CARPS_DOC_INFO_TITLE:
				data[sizeof(struct carps_doc_info) + info->data_len] = '\x0';
				printf("Title: '%s'\n", data + sizeof(struct carps_doc_info));
				break;
			case CARPS_DOC_INFO_USER:
				data[sizeof(struct carps_doc_info) + info->data_len] = '\x0';
				printf("User: '%s'\n", data + sizeof(struct carps_doc_info));
				break;
			default:
				printf("Unknown: type=0x%x, unknown=0x%x, data_len=0x%x\n", type, unknown, info->data_len);
				break;
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
			printf("PRINT DATA 0x%02x", data[0]);
//			dump_data(data, len);
//			printf("\n");
			/* read next blocks until the strip is complete to avoid decoding problems on block boundaries */
/*			if (data[1] != 0x1b) {//fixme
//				do {
					printf("reading next block\n");
					ret = get_block(data + len, f, NO_HEADER);
					if (ret < 0)
						return ret;
					else
						len += ret;
//				} while (buf2[1] != 0x1b);
			}*/
			decode_print_data(data, len, f, fout);
			break;
		default:
			printf("UNKNOWN BLOCK 0x%02x !!!!!!!!\n", header->block_type);
			dump_data(data, len);
			break;
		}
	}

	fclose(fout);
	fclose(f);
	return 0;
}

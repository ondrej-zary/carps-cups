#define u8 unsigned char
#define u16 unsigned short

#define POINTS_PER_INCH 72

#define ESC 0x1b

#define DICT_SIZE 16

#define BUF_SIZE 65536

#define MASK(n)	((1 << n) - 1)
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#if defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN
#define le16_to_cpu(x) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8))
#define cpu_to_le16(x) le16_to_cpu(x)
#define cpu_to_be16(x) (x)
#define be16_to_cpu(x) (x)
#else
#define be16_to_cpu(x) ((((x) >> 8) & 0xff) | (((x) & 0xff) << 8))
#define cpu_to_be16(x) be16_to_cpu(x)
#define cpu_to_le16(x) (x)
#define le16_to_cpu(x) (x)
#endif

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ROUND_UP_MULTIPLE(n, m) (((n) + (m) - 1) & ~((m) - 1))

struct carps_header {
	u8 magic1;	/* 0xCD */
	u8 magic2;	/* 0xCA */
	u8 magic3;	/* 0x10 */
	u8 data_type;	/* 0x00 = control data, 0x02 = print data */
	u8 zero1;	/* 0x00 */
	u8 block_type;	/* 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1a */
	u8 zero2;	/* 0x00 */
	u8 one;		/* 0x01 */
	u16 data_len;	/* number of following data bytes, big endian */
	u8 empty[10];	/* zeros */
} __attribute__((packed));

#define MAX_BLOCK_LEN	4096
#define MAX_DATA_LEN	(MAX_BLOCK_LEN - sizeof(struct carps_header))

#define CARPS_DATA_CONTROL	0x00
#define CARPS_DATA_PRINT	0x02

#define CARPS_BLOCK_BEGIN	0x11
#define CARPS_BLOCK_DOC_INFO	0x12
#define CARPS_BLOCK_END		0x13
#define CARPS_BLOCK_BEGIN1	0x14
#define CARPS_BLOCK_END1	0x16
#define CARPS_BLOCK_BEGIN2	0x17
#define CARPS_BLOCK_PARAMS	0x18
#define CARPS_BLOCK_END2	0x19
#define CARPS_BLOCK_PRINT	0x1a

struct carps_doc_info {
	u16 type;
	u16 unknown;
	u8 data_len;
} __attribute__((packed));
#define CARPS_DOC_INFO_TITLE	0x0004
#define CARPS_DOC_INFO_USER	0x0006
#define CARPS_DOC_INFO_TIME	0x0009

struct carps_time {
	u16 type;	/* 0x0009 */
	u8 year;
	u8 year_month;
	u8 day;
	u8 zero;
	u8 hour;
	u8 min;
	u8 sec_msec;
	u8 msec;
} __attribute__((packed));

struct carps_print_params {
	u8 magic;
	u8 param;
	u8 enabled;
} __attribute__((packed));
#define CARPS_PARAM_MAGIC	0x08
#define CARPS_PARAM_IMAGEREFINE	0x2d
#define CARPS_PARAM_TONERSAVE	0x5a
#define CARPS_PARAM_DISABLED	0x01
#define CARPS_PARAM_ENABLED	0x02

struct carps_print_header {
	u8 one;		/* 0x01 */
	u8 two;		/* 0x02 */
	u8 four;	/* 0x04 */
	u8 eight;	/* 0x08 */
	u16 zero1;	/* 0x0000 */
	u8 magic;	/* 0x50 */
	u8 zero2;	/* 0x00 */
	u8 last;	/* 0x00 = last packet, 0x01 = not last packet */
	u16 data_len;	/* little endian?? */
	u16 zero3;
} __attribute__((packed));

#define PRINT_DATA_XOR 0x43

enum carps_paper_weight {
	WEIGHT_PLAIN_L	= 15,
	WEIGHT_PLAIN	= 20,
	WEIGHT_HEAVY	= 30,
	WEIGHT_HEAVY_H	= 35,
	WEIGHT_TRANSP	= 40,
	WEIGHT_ENVELOPE	= 55,
};

enum carps_paper_size {
	PAPER_A4	= 14,
	PAPER_A5	= 16,
	PAPER_B5	= 26,
	PAPER_LETTER	= 30,
	PAPER_LEGAL	= 32,
	PAPER_EXECUTIVE	= 40,
	PAPER_ENV_MONAR	= 60,
	PAPER_ENV_COM10	= 62,
	PAPER_ENV_DL	= 64,
	PAPER_ENV_C5	= 66,
	PAPER_CUSTOM	= 80,
};

const char *bin_n(u16 x, u8 n) {
	static char b[9];
	b[0] = '\0';

	for (u16 i = 1 << (n - 1); i > 0; i >>= 1)
		strcat(b, (x & i) ? "1" : "0");

	return b;
}

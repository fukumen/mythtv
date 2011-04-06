#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <iconv.h>

/*#define USE_UNICODE_SQUAREDCJK	(1)*/
/*#define DEBUG	(1)*/

#ifdef DEBUG
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>
pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
#endif

#include "isdb_decode_text.h"

typedef struct {
	ISDBTYPE isdb;

	/* for ARIB STD-B24 8bit-character code */
	iconv_t cd;
} _IsdbDecode;

typedef struct {
	const unsigned char *str;
	const unsigned char *p;
	unsigned len;
} IBUF;
#define IBUF_init(ibuf, s, l)		{ (ibuf)->str = (ibuf)->p = (s); (ibuf)->len = (l); }
#define IBUF_isremain(ibuf)			((ibuf)->p < (ibuf)->str + (ibuf)->len)
#define IBUF_isremainn(ibuf, n)		((ibuf)->p + (n) < (ibuf)->str + (ibuf)->len)
#define IBUF_get(ibuf)				(*(ibuf)->p++)
#define IBUF_geti(ibuf, i)			((ibuf)->p[(i)])

typedef struct {
	unsigned char *str;
	unsigned char *p;
	unsigned len;
} OBUF;
#define OBUF_init(obuf, s, l)		{ (obuf)->str = (obuf)->p = (s); (obuf)->len = (l); }
#define OBUF_isremain(obuf)			((obuf)->p + 1 <= (obuf)->str + (obuf)->len)
#define OBUF_isremainn(obuf, n)		((obuf)->p + (n) <= (obuf)->str + (obuf)->len)
#define OBUF_put(obuf, c)			{ *(obuf)->p++ = (c); }
#define OBUF_putp(obuf)				(&(obuf)->p)
#define OBUF_putleft(obuf)			((obuf)->str + (obuf)->len - (obuf)->p)

/* ARIB STD-B24 Table 7-14 */
#define SP		(0x20)

#define APD		(0x0a)
#define LS1		(0x0e)
#define LS0		(0x0f)
#define PAPF	(0x16)
#define SS2		(0x19)
#define ESC		(0x1b)
#define APS		(0x1c)
#define SS3		(0x1d)

#define SZX		(0x8b)
#define COL		(0x90)
#define FLC		(0x91)
#define CDC		(0x92)
#define POL		(0x93)
#define WMM		(0x94)
#define MACRO	(0x95)
#define HLC		(0x97)
#define RPC		(0x98)
#define CSI		(0x9b)
#define TIME	(0x9d)

/* ARIB STD-B24 Table 7-1 */
#define LS2_2	(0x6e)
#define LS3_2	(0x6f)
#define LS1R_2	(0x7e)
#define LS2R_2	(0x7d)
#define LS3R_2	(0x7c)

/* ARIB STD-B24 Table 7-2 */
#define GSET1BYTE_2_G0	(0x28)
#define GSET1BYTE_2_G1	(0x29)
#define GSET1BYTE_2_G2	(0x2a)
#define GSET1BYTE_2_G3	(0x2b)
#define GSET2BYTE_2		(0x24)
#define GSET2BYTE_3_G1	(0x29)
#define GSET2BYTE_3_G2	(0x2a)
#define GSET2BYTE_3_G3	(0x2b)
#define DRCS1BYTE_3		(0x20)	/* non-support */
#define DRCS2BYTE_3_G0	(0x28)	/* non-support */
#define DRCS2BYTE_4		(0x20)	/* non-support */

/* ARIB STD-B24 Table 7-3 */
#define CODESET_KANJI				(0x42)	/* 2byte */
#define CODESET_ALPHANUMERIC		(0x4a)
#define CODESET_HIRAGANA			(0x30)
#define CODESET_KATAKANA			(0x31)
#define CODESET_MOSAIC_A			(0x32)	/* non-support */
#define CODESET_MOSAIC_B			(0x33)	/* non-support */
#define CODESET_MOSAIC_C			(0x34)	/* non-support */
#define CODESET_MOSAIC_D			(0x35)	/* non-support */
#define CODESET_P_ALPHANUMERIC		(0x36)
#define CODESET_P_HIRAGANA			(0x37)
#define CODESET_P_KATAKANA			(0x38)
#define CODESET_JISX0201KATAKANA	(0x49)
#define CODESET_JIS_KANJI1			(0x39)	/* 2byte */
#define CODESET_JIS_KANJI2			(0x3a)	/* 2byte */
#define CODESET_ADDITIONAL_SYMBOLS	(0x3b)	/* 2byte */

/* ABNT NBR 15606-1 Table 15 */
#define CODESET_LATIN_EXTENSION		(0x4b)	/* non-support */
#define CODESET_SPECIAL_CHARACTER	(0x4c)	/* non-support */

#define ISGL(c)	(0x21 <= (c) && (c) <= 0x7e)
#define ISGR(c)	(0xa1 <= (c) && (c) <= 0xfe)
#define ISADDITIONAL_SYMBOLS(row)	(90 <= row && row <= 94)
#define ISADDITIONAL_KANJI(row)		(85 <= row && row <= 86)

typedef enum { GLRnull, GL, GR } GLR;
typedef enum { Gnull, G0, G1, G2, G3, } GSET;
typedef enum {
	Cnull, Ckanji,
	Calpha, Chiragana, Ckatakana,
	CPalpha, CPhiragana, CPkatakana,
	CJkatakana, CJkanji1, CJkanji2, Cadd,
	Cextension, Cspecial,
} CODESET;

typedef struct {
	GSET gl;					/* GL: locking shift */
	GSET gr;					/* GR: locking shift */
	GSET ss;					/* GL: single shift */
	CODESET g0, g1, g2, g3;
} SELECTINFO;

#define REPLACEMENT_CHARACTER "\xef\xbf\xbd"

static const char codeset_Alphanumeric[93] =
	"!\"#$%&'()*+,-./"
	"0123456789:;<=>?"
	"@ABCDEFGHIJKLMNO"
	"PQRSTUVWXYZ[\xa5]^_"
	"`abcdefghijklmno"
	"pqrstuvwxyz{|}"	/* 0x7e: over line U+203E=\xe2\x80\xbe */
	;

static const char codeset_Hiragana[94*3] =
	"ぁあぃいぅうぇえぉおかがきぎく"
	"ぐけげこごさざしじすずせぜそぞた"
	"だちぢっつづてでとどなにぬねのは"
	"ばぱひびぴふぶぷへべぺほぼぽまみ"
	"むめもゃやゅゆょよらりるれろゎわ"
	"ゐゑをん　　　ゝゞー。「」、・"
	;

static const char codeset_Katakana[94*3] =
	"ァアィイゥウェエォオカガキギク"
	"グケゲコゴサザシジスズセゼソゾタ"
	"ダチヂッツヅテデトドナニヌネノハ"
	"バパヒビピフブプヘベペホボポマミ"
	"ムメモャヤュユョヨラリルレロヮワ"
	"ヰヱヲンヴヵヶヽヾー。「」、・"
	;

static const char codeset_JISX0201katakana[63*3] =
	"｡｢｣､･ｦｧｨｩｪｫｬｭｮｯ"
	"ｰｱｲｳｴｵｶｷｸｹｺｻｼｽｾｿ"
	"ﾀﾁﾂﾃﾄﾅﾆﾇﾈﾉﾊﾋﾌﾍﾎﾏ"
	"ﾐﾑﾒﾓﾔﾕﾖﾗﾘﾙﾚﾛﾜﾝﾞﾟ"
	;

/* Additional symbols */
static const char * const codeset_Additional_symbols90[5][94] = {
	{
		/* row:90 */
		"\xe2\x9b\x8c",	/* cell:1 */
		"\xe2\x9b\x8d",	/* cell:2 */
		"\xe2\x9d\x97",	/* cell:3 */
		"\xe2\x9b\x8f",	/* cell:4 */
		"\xe2\x9b\x90",	/* cell:5 */
		"\xe2\x9b\x91",	/* cell:6 */
		REPLACEMENT_CHARACTER,	/* cell:7 */
		"\xe2\x9b\x92",	/* cell:8 */
		"\xe2\x9b\x95",	/* cell:9 */
		"\xe2\x9b\x93",	/* cell:10 */
		"\xe2\x9b\x94",	/* cell:11 */
		REPLACEMENT_CHARACTER,	/* cell:12 */
		REPLACEMENT_CHARACTER,	/* cell:13 */
		REPLACEMENT_CHARACTER,	/* cell:14 */
		REPLACEMENT_CHARACTER,	/* cell:15 */
		"\xf0\x9f\x85\xbf",	/* cell:16 */
		"\xf0\x9f\x86\x8a",	/* cell:17 */
		REPLACEMENT_CHARACTER,	/* cell:18 */
		REPLACEMENT_CHARACTER,	/* cell:19 */
		"\xe2\x9b\x96",	/* cell:20 */
		"\xe2\x9b\x97",	/* cell:21 */
		"\xe2\x9b\x98",	/* cell:22 */
		"\xe2\x9b\x99",	/* cell:23 */
		"\xe2\x9b\x9a",	/* cell:24 */
		"\xe2\x9b\x9b",	/* cell:25 */
		"\xe2\x9b\x9c",	/* cell:26 */
		"\xe2\x9b\x9d",	/* cell:27 */
		"\xe2\x9b\x9e",	/* cell:28 */
		"\xe2\x9b\x9f",	/* cell:29 */
		"\xe2\x9b\xa0",	/* cell:30 */
		"\xe2\x9b\xa1",	/* cell:31 */
		"\xe2\xad\x95",	/* cell:32 */
		"\xe3\x89\x88",	/* cell:33 */
		"\xe3\x89\x89",	/* cell:34 */
		"\xe3\x89\x8a",	/* cell:35 */
		"\xe3\x89\x8b",	/* cell:36 */
		"\xe3\x89\x8c",	/* cell:37 */
		"\xe3\x89\x8d",	/* cell:38 */
		"\xe3\x89\x8e",	/* cell:39 */
		"\xe3\x89\x8f",	/* cell:40 */
		REPLACEMENT_CHARACTER,	/* cell:41 */
		REPLACEMENT_CHARACTER,	/* cell:42 */
		REPLACEMENT_CHARACTER,	/* cell:43 */
		REPLACEMENT_CHARACTER,	/* cell:44 */
		"\xe2\x92\x91",	/* cell:45 */
		"\xe2\x92\x92",	/* cell:46 */
		"\xe2\x92\x93",	/* cell:47 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x85\x8a",	/* cell:48 */
		"\xf0\x9f\x85\x8c",	/* cell:49 */
		"\xf0\x9f\x84\xbf",	/* cell:50 */
		"\xf0\x9f\x85\x86",	/* cell:51 */
		"\xf0\x9f\x85\x8b",	/* cell:52 */
		"\xf0\x9f\x88\x90",	/* cell:53 */
		"\xf0\x9f\x88\x91",	/* cell:54 */
		"\xf0\x9f\x88\x92",	/* cell:55 */
		"\xf0\x9f\x88\x93",	/* cell:56 */
		"\xf0\x9f\x85\x82",	/* cell:57 */
		"\xf0\x9f\x88\x94",	/* cell:58 */
		"\xf0\x9f\x88\x95",	/* cell:59 */
		"\xf0\x9f\x88\x96",	/* cell:60 */
		"\xf0\x9f\x85\x8d",	/* cell:61 */
		"\xf0\x9f\x84\xb1",	/* cell:62 */
		"\xf0\x9f\x84\xbd",	/* cell:63 */
#else
		"[HV]",	/* cell:48 */
		"[SD]",	/* cell:49 */
		"[P]",	/* cell:50 */
		"[W]",	/* cell:51 */
		"[MV]",	/* cell:52 */
		"[\xe6\x89\x8b]",	/* cell:53 */
		"[\xe5\xad\x97]",	/* cell:54 */
		"[\xe5\x8f\x8c]",	/* cell:55 */
		"[\xe3\x83\x87]",	/* cell:56 */
		"[S]",	/* cell:57 */
		"[\xe4\xba\x8c]",	/* cell:58 */
		"[\xe5\xa4\x9a]",	/* cell:59 */
		"[\xe8\xa7\xa3]",	/* cell:60 */
		"[SS]",	/* cell:61 */
		"[B]",	/* cell:62 */
		"[N]",	/* cell:63 */
#endif
		"\xe2\xac\x9b",	/* cell:64 */
		"\xe2\xac\xa4",	/* cell:65 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x88\x97",	/* cell:66 */
		"\xf0\x9f\x88\x98",	/* cell:67 */
		"\xf0\x9f\x88\x99",	/* cell:68 */
		"\xf0\x9f\x88\x9a",	/* cell:69 */
		"\xf0\x9f\x88\x9b",	/* cell:70 */
#else
		"[\xe5\xa4\xa9]",	/* cell:66 */
		"[\xe4\xba\xa4]",	/* cell:67 */
		"[\xe6\x98\xa0]",	/* cell:68 */
		"[\xe7\x84\xa1]",	/* cell:69 */
		"[\xe6\x96\x99]",	/* cell:70 */
#endif
		"\xe2\x9a\xbf",	/* cell:71 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x88\x9c",	/* cell:72 */
		"\xf0\x9f\x88\x9d",	/* cell:73 */
		"\xf0\x9f\x88\x9e",	/* cell:74 */
		"\xf0\x9f\x88\x9f",	/* cell:75 */
		"\xf0\x9f\x88\xa0",	/* cell:76 */
		"\xf0\x9f\x88\xa1",	/* cell:77 */
		"\xf0\x9f\x88\xa2",	/* cell:78 */
		"\xf0\x9f\x88\xa3",	/* cell:79 */
		"\xf0\x9f\x88\xa4",	/* cell:80 */
		"\xf0\x9f\x88\xa5",	/* cell:81 */
		"\xf0\x9f\x85\x8e",	/* cell:82 */
#else
		"[\xe5\x89\x8d]",	/* cell:72 */
		"[\xe5\xbe\x8c]",	/* cell:73 */
		"[\xe5\x86\x8d]",	/* cell:74 */
		"[\xe6\x96\xb0]",	/* cell:75 */
		"[\xe5\x88\x9d]",	/* cell:76 */
		"[\xe7\xb5\x82]",	/* cell:77 */
		"[\xe7\x94\x9f]",	/* cell:78 */
		"[\xe8\xb2\xa9]",	/* cell:79 */
		"[\xe5\xa3\xb0]",	/* cell:80 */
		"[\xe5\x90\xb9]",	/* cell:81 */
		"[PPV]",	/* cell:82 */
#endif
		"\xe3\x8a\x99",	/* cell:83 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x88\x80",	/* cell:84 */
#else
		"\xe3\x81\xbb\xe3\x81\x8b",	/* cell:84 */
#endif
		REPLACEMENT_CHARACTER,	/* cell:85 */
		REPLACEMENT_CHARACTER,	/* cell:86 */
		REPLACEMENT_CHARACTER,	/* cell:87 */
		REPLACEMENT_CHARACTER,	/* cell:88 */
		REPLACEMENT_CHARACTER,	/* cell:89 */
		REPLACEMENT_CHARACTER,	/* cell:90 */
		REPLACEMENT_CHARACTER,	/* cell:91 */
		REPLACEMENT_CHARACTER,	/* cell:92 */
		REPLACEMENT_CHARACTER,	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}, {
		/* row:91 */
		"\xe2\x9b\xa3",	/* cell:1 */
		"\xe2\xad\x96",	/* cell:2 */
		"\xe2\xad\x97",	/* cell:3 */
		"\xe2\xad\x98",	/* cell:4 */
		"\xe2\xad\x99",	/* cell:5 */
		"\xe2\x98\x93",	/* cell:6 */
		"\xe3\x8a\x8b",	/* cell:7 */
		"\xe3\x80\x92",	/* cell:8 */
		"\xe2\x9b\xa8",	/* cell:9 */
		"\xe3\x89\x86",	/* cell:10 */
		"\xe3\x89\x85",	/* cell:11 */
		"\xe2\x9b\xa9",	/* cell:12 */
		"\xe5\x8d\x8d",	/* cell:13 */
		"\xe2\x9b\xaa",	/* cell:14 */
		"\xe2\x9b\xab",	/* cell:15 */
		"\xe2\x9b\xac",	/* cell:16 */
		"\xe2\x99\xa8",	/* cell:17 */
		"\xe2\x9b\xad",	/* cell:18 */
		"\xe2\x9b\xae",	/* cell:19 */
		"\xe2\x9b\xaf",	/* cell:20 */
		"\xe2\x9a\x93",	/* cell:21 */
		"\xe2\x9c\x88",	/* cell:22 */
		"\xe2\x9b\xb0",	/* cell:23 */
		"\xe2\x9b\xb1",	/* cell:24 */
		"\xe2\x9b\xb2",	/* cell:25 */
		"\xe2\x9b\xb3",	/* cell:26 */
		"\xe2\x9b\xb4",	/* cell:27 */
		"\xe2\x9b\xb5",	/* cell:28 */
		"\xf0\x9f\x85\x97",	/* cell:29 */
		"\xe2\x92\xb9",	/* cell:30 */
		"\xe2\x93\x88",	/* cell:31 */
		"\xe2\x9b\xb6",	/* cell:32 */
		"\xf0\x9f\x85\x9f",	/* cell:33 */
		"\xf0\x9f\x86\x8b",	/* cell:34 */
		"\xf0\x9f\x86\x8d",	/* cell:35 */
		"\xf0\x9f\x86\x8c",	/* cell:36 */
		"\xf0\x9f\x85\xb9",	/* cell:37 */
		"\xe2\x9b\xb7",	/* cell:38 */
		"\xe2\x9b\xb8",	/* cell:39 */
		"\xe2\x9b\xb9",	/* cell:40 */
		"\xe2\x9b\xba",	/* cell:41 */
		"\xf0\x9f\x85\xbb",	/* cell:42 */
		"\xe2\x98\x8e",	/* cell:43 */
		"\xe2\x9b\xbb",	/* cell:44 */
		"\xe2\x9b\xbc",	/* cell:45 */
		"\xe2\x9b\xbd",	/* cell:46 */
		"\xe2\x9b\xbe",	/* cell:47 */
		"\xf0\x9f\x85\xbc",	/* cell:48 */
		"\xe2\x9b\xbf",	/* cell:49 */
		REPLACEMENT_CHARACTER,	/* cell:50 */
		REPLACEMENT_CHARACTER,	/* cell:51 */
		REPLACEMENT_CHARACTER,	/* cell:52 */
		REPLACEMENT_CHARACTER,	/* cell:53 */
		REPLACEMENT_CHARACTER,	/* cell:54 */
		REPLACEMENT_CHARACTER,	/* cell:55 */
		REPLACEMENT_CHARACTER,	/* cell:56 */
		REPLACEMENT_CHARACTER,	/* cell:57 */
		REPLACEMENT_CHARACTER,	/* cell:58 */
		REPLACEMENT_CHARACTER,	/* cell:59 */
		REPLACEMENT_CHARACTER,	/* cell:60 */
		REPLACEMENT_CHARACTER,	/* cell:61 */
		REPLACEMENT_CHARACTER,	/* cell:62 */
		REPLACEMENT_CHARACTER,	/* cell:63 */
		REPLACEMENT_CHARACTER,	/* cell:64 */
		REPLACEMENT_CHARACTER,	/* cell:65 */
		REPLACEMENT_CHARACTER,	/* cell:66 */
		REPLACEMENT_CHARACTER,	/* cell:67 */
		REPLACEMENT_CHARACTER,	/* cell:68 */
		REPLACEMENT_CHARACTER,	/* cell:69 */
		REPLACEMENT_CHARACTER,	/* cell:70 */
		REPLACEMENT_CHARACTER,	/* cell:71 */
		REPLACEMENT_CHARACTER,	/* cell:72 */
		REPLACEMENT_CHARACTER,	/* cell:73 */
		REPLACEMENT_CHARACTER,	/* cell:74 */
		REPLACEMENT_CHARACTER,	/* cell:75 */
		REPLACEMENT_CHARACTER,	/* cell:76 */
		REPLACEMENT_CHARACTER,	/* cell:77 */
		REPLACEMENT_CHARACTER,	/* cell:78 */
		REPLACEMENT_CHARACTER,	/* cell:79 */
		REPLACEMENT_CHARACTER,	/* cell:80 */
		REPLACEMENT_CHARACTER,	/* cell:81 */
		REPLACEMENT_CHARACTER,	/* cell:82 */
		REPLACEMENT_CHARACTER,	/* cell:83 */
		REPLACEMENT_CHARACTER,	/* cell:84 */
		REPLACEMENT_CHARACTER,	/* cell:85 */
		REPLACEMENT_CHARACTER,	/* cell:86 */
		REPLACEMENT_CHARACTER,	/* cell:87 */
		REPLACEMENT_CHARACTER,	/* cell:88 */
		REPLACEMENT_CHARACTER,	/* cell:89 */
		REPLACEMENT_CHARACTER,	/* cell:90 */
		REPLACEMENT_CHARACTER,	/* cell:91 */
		REPLACEMENT_CHARACTER,	/* cell:92 */
		REPLACEMENT_CHARACTER,	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}, {
		/* row:92 */
		"\xe2\x9e\xa1",	/* cell:1 */
		"\xe2\xac\x85",	/* cell:2 */
		"\xe2\xac\x86",	/* cell:3 */
		"\xe2\xac\x87",	/* cell:4 */
		"\xe2\xac\xaf",	/* cell:5 */
		"\xe2\xac\xae",	/* cell:6 */
		"\xe5\xb9\xb4",	/* cell:7 */
		"\xe6\x9c\x88",	/* cell:8 */
		"\xe6\x97\xa5",	/* cell:9 */
		"\xe5\x86\x86",	/* cell:10 */
		"\xe3\x8e\xa1",	/* cell:11 */
		"\xe3\x8e\xa5",	/* cell:12 */
		"\xe3\x8e\x9d",	/* cell:13 */
		"\xe3\x8e\xa0",	/* cell:14 */
		"\xe3\x8e\xa4",	/* cell:15 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x84\x80",	/* cell:16 */
#else
		"0.",	/* cell:16 */
#endif
		"\xe2\x92\x88",	/* cell:17 */
		"\xe2\x92\x89",	/* cell:18 */
		"\xe2\x92\x8a",	/* cell:19 */
		"\xe2\x92\x8b",	/* cell:20 */
		"\xe2\x92\x8c",	/* cell:21 */
		"\xe2\x92\x8d",	/* cell:22 */
		"\xe2\x92\x8e",	/* cell:23 */
		"\xe2\x92\x8f",	/* cell:24 */
		"\xe2\x92\x90",	/* cell:25 */
		"\xee\x8a\x90",	/* cell:26 */
		"\xee\x8a\x91",	/* cell:27 */
		"\xee\x8a\x92",	/* cell:28 */
		"\xee\x8a\x93",	/* cell:29 */
		"\xee\x8a\x94",	/* cell:30 */
		"\xee\x8a\x95",	/* cell:31 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x84\x81",	/* cell:32 */
		"\xf0\x9f\x84\x82",	/* cell:33 */
		"\xf0\x9f\x84\x83",	/* cell:34 */
		"\xf0\x9f\x84\x84",	/* cell:35 */
		"\xf0\x9f\x84\x85",	/* cell:36 */
		"\xf0\x9f\x84\x86",	/* cell:37 */
		"\xf0\x9f\x84\x87",	/* cell:38 */
		"\xf0\x9f\x84\x88",	/* cell:39 */
		"\xf0\x9f\x84\x89",	/* cell:40 */
		"\xf0\x9f\x84\x8a",	/* cell:41 */
#else
		"0,",	/* cell:32 */
		"1,",	/* cell:33 */
		"2,",	/* cell:34 */
		"3,",	/* cell:35 */
		"4,",	/* cell:36 */
		"5,",	/* cell:37 */
		"6,",	/* cell:38 */
		"7,",	/* cell:39 */
		"8,",	/* cell:40 */
		"9,",	/* cell:41 */
#endif
		"\xe3\x88\xb3",	/* cell:42 */
		"\xe3\x88\xb6",	/* cell:43 */
		"\xe3\x88\xb2",	/* cell:44 */
		"\xe3\x88\xb1",	/* cell:45 */
		"\xe3\x88\xb9",	/* cell:46 */
		"\xe3\x89\x84",	/* cell:47 */
		"\xe2\x96\xb6",	/* cell:48 */
		"\xe2\x97\x80",	/* cell:49 */
		"\xe3\x80\x96",	/* cell:50 */
		"\xe3\x80\x97",	/* cell:51 */
		"\xe2\x9f\x90",	/* cell:52 */
		"\xc2\xb2",	/* cell:53 */
		"\xc2\xb3",	/* cell:54 */
		"\xf0\x9f\x84\xad",	/* cell:55 */
		"\xee\x8a\xa5",	/* cell:56 */
		"\xee\x8a\xa6",	/* cell:57 */
		"\xee\x8a\xa7",	/* cell:58 */
		"\xee\x8a\xa8",	/* cell:59 */
		"\xee\x8a\xa9",	/* cell:60 */
		"\xee\x8a\xaa",	/* cell:61 */
		"\xee\x8a\xab",	/* cell:62 */
		"\xee\x8a\xac",	/* cell:63 */
		"\xee\x8a\xad",	/* cell:64 */
		"\xee\x8a\xae",	/* cell:65 */
		"\xee\x8a\xaf",	/* cell:66 */
		"\xee\x8a\xb0",	/* cell:67 */
		"\xee\x8a\xb1",	/* cell:68 */
		"\xee\x8a\xb2",	/* cell:69 */
		"\xee\x8a\xb3",	/* cell:70 */
		"\xee\x8a\xb4",	/* cell:71 */
		"\xee\x8a\xb5",	/* cell:72 */
		"\xee\x8a\xb6",	/* cell:73 */
		"\xee\x8a\xb7",	/* cell:74 */
		"\xee\x8a\xb8",	/* cell:75 */
		"\xee\x8a\xb9",	/* cell:76 */
		"\xee\x8a\xba",	/* cell:77 */
		"\xee\x8a\xbb",	/* cell:78 */
		"\xee\x8a\xbc",	/* cell:79 */
		"\xee\x8a\xbd",	/* cell:80 */
		"\xee\x8a\xbe",	/* cell:81 */
		"\xee\x8a\xbf",	/* cell:82 */
		"\xee\x8b\x80",	/* cell:83 */
		"\xee\x8b\x81",	/* cell:84 */
		"\xee\x8b\x82",	/* cell:85 */
		"\xf0\x9f\x84\xac",	/* cell:86 */
		"\xf0\x9f\x84\xab",	/* cell:87 */
		"\xe3\x89\x87",	/* cell:88 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x86\x90",	/* cell:89 */
		"\xf0\x9f\x88\xa6",	/* cell:90 */
#else
		"DJ",	/* cell:89 */
		"[\xe6\xbc\x94]",	/* cell:90 */
#endif
		"\xe2\x84\xbb",	/* cell:91 */
		REPLACEMENT_CHARACTER,	/* cell:92 */
		REPLACEMENT_CHARACTER,	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}, {
		/* row:93 */
		"\xe3\x88\xaa",	/* cell:1 */
		"\xe3\x88\xab",	/* cell:2 */
		"\xe3\x88\xac",	/* cell:3 */
		"\xe3\x88\xad",	/* cell:4 */
		"\xe3\x88\xae",	/* cell:5 */
		"\xe3\x88\xaf",	/* cell:6 */
		"\xe3\x88\xb0",	/* cell:7 */
		"\xe3\x88\xb7",	/* cell:8 */
		"\xe3\x8d\xbe",	/* cell:9 */
		"\xe3\x8d\xbd",	/* cell:10 */
		"\xe3\x8d\xbc",	/* cell:11 */
		"\xe3\x8d\xbb",	/* cell:12 */
		"\xe2\x84\x96",	/* cell:13 */
		"\xe2\x84\xa1",	/* cell:14 */
		"\xe3\x80\xb6",	/* cell:15 */
		"\xe2\x9a\xbe",	/* cell:16 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x89\x80",	/* cell:17 */
		"\xf0\x9f\x89\x81",	/* cell:18 */
		"\xf0\x9f\x89\x82",	/* cell:19 */
		"\xf0\x9f\x89\x83",	/* cell:20 */
		"\xf0\x9f\x89\x84",	/* cell:21 */
		"\xf0\x9f\x89\x85",	/* cell:22 */
		"\xf0\x9f\x89\x86",	/* cell:23 */
		"\xf0\x9f\x89\x87",	/* cell:24 */
		"\xf0\x9f\x89\x88",	/* cell:25 */
		"\xf0\x9f\x84\xaa",	/* cell:26 */
		"\xf0\x9f\x88\xa7",	/* cell:27 */
		"\xf0\x9f\x88\xa8",	/* cell:28 */
		"\xf0\x9f\x88\xa9",	/* cell:29 */
		"\xf0\x9f\x88\x94",	/* cell:30 */
		"\xf0\x9f\x88\xaa",	/* cell:31 */
		"\xf0\x9f\x88\xab",	/* cell:32 */
		"\xf0\x9f\x88\xac",	/* cell:33 */
		"\xf0\x9f\x88\xad",	/* cell:34 */
		"\xf0\x9f\x88\xae",	/* cell:35 */
		"\xf0\x9f\x88\xaf",	/* cell:36 */
		"\xf0\x9f\x88\xb0",	/* cell:37 */
		"\xf0\x9f\x88\xb1",	/* cell:38 */
#else
		"[\xe6\x9c\xac]",	/* cell:17 */
		"[\xe4\xb8\x89]",	/* cell:18 */
		"[\xe4\xba\x8c]",	/* cell:19 */
		"[\xe5\xae\x89]",	/* cell:20 */
		"[\xe7\x82\xb9]",	/* cell:21 */
		"[\xe6\x89\x93]",	/* cell:22 */
		"[\xe7\x9b\x97]",	/* cell:23 */
		"[\xe5\x8b\x9d]",	/* cell:24 */
		"[\xe6\x95\x97]",	/* cell:25 */
		"[S]",	/* cell:26 */
		"[\xe6\x8a\x95]",	/* cell:27 */
		"[\xe6\x8d\x95]",	/* cell:28 */
		"[\xe4\xb8\x80]",	/* cell:29 */
		"[\xe4\xba\x8c]",	/* cell:30 */
		"[\xe4\xb8\x89]",	/* cell:31 */
		"[\xe9\x81\x8a]",	/* cell:32 */
		"[\xe5\xb7\xa6]",	/* cell:33 */
		"[\xe4\xb8\xad]",	/* cell:34 */
		"[\xe5\x8f\xb3]",	/* cell:35 */
		"[\xe6\x8c\x87]",	/* cell:36 */
		"[\xe8\xb5\xb0]",	/* cell:37 */
		"[\xe6\x89\x93]",	/* cell:38 */
#endif
		"\xe2\x84\x93",	/* cell:39 */
		"\xe3\x8e\x8f",	/* cell:40 */
		"\xe3\x8e\x90",	/* cell:41 */
		"\xe3\x8f\x8a",	/* cell:42 */
		"\xe3\x8e\x9e",	/* cell:43 */
		"\xe3\x8e\xa2",	/* cell:44 */
		"\xe3\x8d\xb1",	/* cell:45 */
		REPLACEMENT_CHARACTER,	/* cell:46 */
		REPLACEMENT_CHARACTER,	/* cell:47 */
		"\xc2\xbd",	/* cell:48 */
		"\xe2\x86\x89",	/* cell:49 */
		"\xe2\x85\x93",	/* cell:50 */
		"\xe2\x85\x94",	/* cell:51 */
		"\xc2\xbc",	/* cell:52 */
		"\xc2\xbe",	/* cell:53 */
		"\xe2\x85\x95",	/* cell:54 */
		"\xe2\x85\x96",	/* cell:55 */
		"\xe2\x85\x97",	/* cell:56 */
		"\xe2\x85\x98",	/* cell:57 */
		"\xe2\x85\x99",	/* cell:58 */
		"\xe2\x85\x9a",	/* cell:59 */
		"\xe2\x85\x90",	/* cell:60 */
		"\xe2\x85\x9b",	/* cell:61 */
		"\xe2\x85\x91",	/* cell:62 */
		"\xe2\x85\x92",	/* cell:63 */
		"\xe2\x98\x80",	/* cell:64 */
		"\xe2\x98\x81",	/* cell:65 */
		"\xe2\x98\x82",	/* cell:66 */
		"\xe2\x9b\x84",	/* cell:67 */
		"\xe2\x98\x96",	/* cell:68 */
		"\xe2\x98\x97",	/* cell:69 */
		"\xe2\x9b\x89",	/* cell:70 */
		"\xe2\x9b\x8a",	/* cell:71 */
		"\xe2\x99\xa6",	/* cell:72 */
		"\xe2\x99\xa5",	/* cell:73 */
		"\xe2\x99\xa3",	/* cell:74 */
		"\xe2\x99\xa0",	/* cell:75 */
		"\xe2\x9b\x8b",	/* cell:76 */
		"\xe2\x98\x89",	/* cell:77 */
		"\xe2\x80\xbc",	/* cell:78 */
		"\xe2\x81\x89",	/* cell:79 */
		"\xe2\x9b\x85",	/* cell:80 */
		"\xe2\x98\x94",	/* cell:81 */
		"\xe2\x9b\x86",	/* cell:82 */
		"\xe2\x98\x83",	/* cell:83 */
		"\xe2\x9b\x87",	/* cell:84 */
		"\xe2\x98\x87",	/* cell:85 */
		"\xe2\x9b\x88",	/* cell:86 */
		REPLACEMENT_CHARACTER,	/* cell:87 */
		"\xe2\x9a\x9e",	/* cell:88 */
		"\xe2\x9a\x9f",	/* cell:89 */
		"\xe2\x99\xac",	/* cell:90 */
		"\xe2\x98\x8e",	/* cell:91 */
		REPLACEMENT_CHARACTER,	/* cell:92 */
		REPLACEMENT_CHARACTER,	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}, {
		/* row:94 */
		"\xe2\x85\xa0",	/* cell:1 */
		"\xe2\x85\xa1",	/* cell:2 */
		"\xe2\x85\xa2",	/* cell:3 */
		"\xe2\x85\xa3",	/* cell:4 */
		"\xe2\x85\xa4",	/* cell:5 */
		"\xe2\x85\xa5",	/* cell:6 */
		"\xe2\x85\xa6",	/* cell:7 */
		"\xe2\x85\xa7",	/* cell:8 */
		"\xe2\x85\xa8",	/* cell:9 */
		"\xe2\x85\xa9",	/* cell:10 */
		"\xe2\x85\xaa",	/* cell:11 */
		"\xe2\x85\xab",	/* cell:12 */
		"\xe2\x91\xb0",	/* cell:13 */
		"\xe2\x91\xb1",	/* cell:14 */
		"\xe2\x91\xb2",	/* cell:15 */
		"\xe2\x91\xb3",	/* cell:16 */
		"\xe2\x91\xb4",	/* cell:17 */
		"\xe2\x91\xb5",	/* cell:18 */
		"\xe2\x91\xb6",	/* cell:19 */
		"\xe2\x91\xb7",	/* cell:20 */
		"\xe2\x91\xb8",	/* cell:21 */
		"\xe2\x91\xb9",	/* cell:22 */
		"\xe2\x91\xba",	/* cell:23 */
		"\xe2\x91\xbb",	/* cell:24 */
		"\xe2\x91\xbc",	/* cell:25 */
		"\xe2\x91\xbd",	/* cell:26 */
		"\xe2\x91\xbe",	/* cell:27 */
		"\xe2\x91\xbf",	/* cell:28 */
		"\xe3\x89\x91",	/* cell:29 */
		"\xe3\x89\x92",	/* cell:30 */
		"\xe3\x89\x93",	/* cell:31 */
		"\xe3\x89\x94",	/* cell:32 */
#ifdef USE_UNICODE_SQUAREDCJK
		"\xf0\x9f\x84\x90",	/* cell:33 */
		"\xf0\x9f\x84\x91",	/* cell:34 */
		"\xf0\x9f\x84\x92",	/* cell:35 */
		"\xf0\x9f\x84\x93",	/* cell:36 */
		"\xf0\x9f\x84\x94",	/* cell:37 */
		"\xf0\x9f\x84\x95",	/* cell:38 */
		"\xf0\x9f\x84\x96",	/* cell:39 */
		"\xf0\x9f\x84\x97",	/* cell:40 */
		"\xf0\x9f\x84\x98",	/* cell:41 */
		"\xf0\x9f\x84\x99",	/* cell:42 */
		"\xf0\x9f\x84\x9a",	/* cell:43 */
		"\xf0\x9f\x84\x9b",	/* cell:44 */
		"\xf0\x9f\x84\x9c",	/* cell:45 */
		"\xf0\x9f\x84\x9d",	/* cell:46 */
		"\xf0\x9f\x84\x9e",	/* cell:47 */
		"\xf0\x9f\x84\x9f",	/* cell:48 */
		"\xf0\x9f\x84\xa0",	/* cell:49 */
		"\xf0\x9f\x84\xa1",	/* cell:50 */
		"\xf0\x9f\x84\xa2",	/* cell:51 */
		"\xf0\x9f\x84\xa3",	/* cell:52 */
		"\xf0\x9f\x84\xa4",	/* cell:53 */
		"\xf0\x9f\x84\xa5",	/* cell:54 */
		"\xf0\x9f\x84\xa6",	/* cell:55 */
		"\xf0\x9f\x84\xa7",	/* cell:56 */
		"\xf0\x9f\x84\xa8",	/* cell:57 */
		"\xf0\x9f\x84\xa9",	/* cell:58 */
#else
		"(A)",	/* cell:33 */
		"(B)",	/* cell:34 */
		"(C)",	/* cell:35 */
		"(D)",	/* cell:36 */
		"(E)",	/* cell:37 */
		"(F)",	/* cell:38 */
		"(G)",	/* cell:39 */
		"(H)",	/* cell:40 */
		"(I)",	/* cell:41 */
		"(J)",	/* cell:42 */
		"(K)",	/* cell:43 */
		"(L)",	/* cell:44 */
		"(M)",	/* cell:45 */
		"(N)",	/* cell:46 */
		"(O)",	/* cell:47 */
		"(P)",	/* cell:48 */
		"(Q)",	/* cell:49 */
		"(R)",	/* cell:50 */
		"(S)",	/* cell:51 */
		"(T)",	/* cell:52 */
		"(U)",	/* cell:53 */
		"(V)",	/* cell:54 */
		"(W)",	/* cell:55 */
		"(X)",	/* cell:56 */
		"(Y)",	/* cell:57 */
		"(Z)",	/* cell:58 */
#endif
		"\xe3\x89\x95",	/* cell:59 */
		"\xe3\x89\x96",	/* cell:60 */
		"\xe3\x89\x97",	/* cell:61 */
		"\xe3\x89\x98",	/* cell:62 */
		"\xe3\x89\x99",	/* cell:63 */
		"\xe3\x89\x9a",	/* cell:64 */
		"\xe2\x91\xa0",	/* cell:65 */
		"\xe2\x91\xa1",	/* cell:66 */
		"\xe2\x91\xa2",	/* cell:67 */
		"\xe2\x91\xa3",	/* cell:68 */
		"\xe2\x91\xa4",	/* cell:69 */
		"\xe2\x91\xa5",	/* cell:70 */
		"\xe2\x91\xa6",	/* cell:71 */
		"\xe2\x91\xa7",	/* cell:72 */
		"\xe2\x91\xa8",	/* cell:73 */
		"\xe2\x91\xa9",	/* cell:74 */
		"\xe2\x91\xaa",	/* cell:75 */
		"\xe2\x91\xab",	/* cell:76 */
		"\xe2\x91\xac",	/* cell:77 */
		"\xe2\x91\xad",	/* cell:78 */
		"\xe2\x91\xae",	/* cell:79 */
		"\xe2\x91\xaf",	/* cell:80 */
		"\xe2\x9d\xb6",	/* cell:81 */
		"\xe2\x9d\xb7",	/* cell:82 */
		"\xe2\x9d\xb8",	/* cell:83 */
		"\xe2\x9d\xb9",	/* cell:84 */
		"\xe2\x9d\xba",	/* cell:85 */
		"\xe2\x9d\xbb",	/* cell:86 */
		"\xe2\x9d\xbc",	/* cell:87 */
		"\xe2\x9d\xbd",	/* cell:88 */
		"\xe2\x9d\xbe",	/* cell:89 */
		"\xe2\x9d\xbf",	/* cell:90 */
		"\xe2\x93\xab",	/* cell:91 */
		"\xe2\x93\xac",	/* cell:92 */
		"\xe3\x89\x9b",	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}
};

/* Additional kanji */
static const char * const codeset_Additional_symbols85[2][94] = {
	{
		/* row:85 */
		"\xe3\x90\x82",	/* cell:1 */
		"\xf0\xa0\x85\x98",	/* cell:2 */
		"\xe4\xbb\xbd",	/* cell:3 */
		"\xe4\xbb\xbf",	/* cell:4 */
		"\xe4\xbe\x9a",	/* cell:5 */
		"\xe4\xbf\x89",	/* cell:6 */
		"\xe5\x82\x9c",	/* cell:7 */
		"\xe5\x84\x9e",	/* cell:8 */
		"\xe5\x86\xbc",	/* cell:9 */
		"\xe3\x94\x9f",	/* cell:10 */
		"\xe5\x8c\x87",	/* cell:11 */
		"\xe5\x8d\xa1",	/* cell:12 */
		"\xe5\x8d\xac",	/* cell:13 */
		"\xe8\xa9\xb9",	/* cell:14 */
		"\xf0\xa0\xae\xb7",	/* cell:15 */
		"\xe5\x91\x8d",	/* cell:16 */
		"\xe5\x92\x96",	/* cell:17 */
		"\xe5\x92\x9c",	/* cell:18 */
		"\xe5\x92\xa9",	/* cell:19 */
		"\xe5\x94\x8e",	/* cell:20 */
		"\xe5\x95\x8a",	/* cell:21 */
		"\xe5\x99\xb2",	/* cell:22 */
		"\xe5\x9b\xa4",	/* cell:23 */
		"\xe5\x9c\xb3",	/* cell:24 */
		"\xe5\x9c\xb4",	/* cell:25 */
		"\xef\xa8\x90",	/* cell:26 */
		"\xe5\xa2\x80",	/* cell:27 */
		"\xe5\xa7\xa4",	/* cell:28 */
		"\xe5\xa8\xa3",	/* cell:29 */
		"\xe5\xa9\x95",	/* cell:30 */
		"\xe5\xaf\xac",	/* cell:31 */
		"\xef\xa8\x91",	/* cell:32 */
		"\xe3\x9f\xa2",	/* cell:33 */
		"\xe5\xba\xac",	/* cell:34 */
		"\xe5\xbc\xb4",	/* cell:35 */
		"\xe5\xbd\x85",	/* cell:36 */
		"\xe5\xbe\xb7",	/* cell:37 */
		"\xe6\x80\x97",	/* cell:38 */
		"\xef\xa9\xab",	/* cell:39 */
		"\xe6\x84\xb0",	/* cell:40 */
		"\xe6\x98\xa4",	/* cell:41 */
		"\xe6\x9b\x88",	/* cell:42 */
		"\xe6\x9b\x99",	/* cell:43 */
		"\xe6\x9b\xba",	/* cell:44 */
		"\xe6\x9b\xbb",	/* cell:45 */
		"\xe6\xa1\x92",	/* cell:46 */
		"\xe9\xbf\x84",	/* cell:47 */
		"\xe6\xa4\x91",	/* cell:48 */
		"\xe6\xa4\xbb",	/* cell:49 */
		"\xe6\xa9\x85",	/* cell:50 */
		"\xe6\xaa\x91",	/* cell:51 */
		"\xe6\xab\x9b",	/* cell:52 */
		"\xf0\xa3\x8f\x8c",	/* cell:53 */
		"\xf0\xa3\x8f\xbe",	/* cell:54 */
		"\xf0\xa3\x97\x84",	/* cell:55 */
		"\xe6\xaf\xb1",	/* cell:56 */
		"\xe6\xb3\xa0",	/* cell:57 */
		"\xe6\xb4\xae",	/* cell:58 */
		"\xef\xa9\x85",	/* cell:59 */
		"\xe6\xb6\xbf",	/* cell:60 */
		"\xe6\xb7\x8a",	/* cell:61 */
		"\xe6\xb7\xb8",	/* cell:62 */
		"\xef\xa9\x86",	/* cell:63 */
		"\xe6\xbd\x9e",	/* cell:64 */
		"\xe6\xbf\xb9",	/* cell:65 */
		"\xe7\x81\xa4",	/* cell:66 */
		"\xef\xa9\xac",	/* cell:67 */
		"\xf0\xa4\x8b\xae",	/* cell:68 */
		"\xe7\x85\x87",	/* cell:69 */
		"\xe7\x87\x81",	/* cell:70 */
		"\xe7\x88\x80",	/* cell:71 */
		"\xe7\x8e\x9f",	/* cell:72 */
		"\xe7\x8e\xa8",	/* cell:73 */
		"\xe7\x8f\x89",	/* cell:74 */
		"\xe7\x8f\x96",	/* cell:75 */
		"\xe7\x90\x9b",	/* cell:76 */
		"\xe7\x90\xa1",	/* cell:77 */
		"\xef\xa9\x8a",	/* cell:78 */
		"\xe7\x90\xa6",	/* cell:79 */
		"\xe7\x90\xaa",	/* cell:80 */
		"\xe7\x90\xac",	/* cell:81 */
		"\xe7\x90\xb9",	/* cell:82 */
		"\xe7\x91\x8b",	/* cell:83 */
		"\xe3\xbb\x9a",	/* cell:84 */
		"\xe7\x95\xb5",	/* cell:85 */
		"\xe7\x96\x81",	/* cell:86 */
		"\xe7\x9d\xb2",	/* cell:87 */
		"\xe4\x82\x93",	/* cell:88 */
		"\xe7\xa3\x88",	/* cell:89 */
		"\xe7\xa3\xa0",	/* cell:90 */
		"\xe7\xa5\x87",	/* cell:91 */
		"\xe7\xa6\xae",	/* cell:92 */
		"\xe9\xbf\x86",	/* cell:93 */
		"\xe4\x84\x83",	/* cell:94 */
	}, {
		/* row:86 */
		"\xe9\xbf\x85",	/* cell:1 */
		"\xe7\xa7\x9a",	/* cell:2 */
		"\xe7\xa8\x9e",	/* cell:3 */
		"\xe7\xad\xbf",	/* cell:4 */
		"\xe7\xb0\xb1",	/* cell:5 */
		"\xe4\x89\xa4",	/* cell:6 */
		"\xe7\xb6\x8b",	/* cell:7 */
		"\xe7\xbe\xa1",	/* cell:8 */
		"\xe8\x84\x98",	/* cell:9 */
		"\xe8\x84\xba",	/* cell:10 */
		"\xef\xa9\xad",	/* cell:11 */
		"\xe8\x8a\xae",	/* cell:12 */
		"\xe8\x91\x9b",	/* cell:13 */
		"\xe8\x93\x9c",	/* cell:14 */
		"\xe8\x93\xac",	/* cell:15 */
		"\xe8\x95\x99",	/* cell:16 */
		"\xe8\x97\x8e",	/* cell:17 */
		"\xe8\x9d\x95",	/* cell:18 */
		"\xe8\x9f\xac",	/* cell:19 */
		"\xe8\xa0\x8b",	/* cell:20 */
		"\xe8\xa3\xb5",	/* cell:21 */
		"\xe8\xa7\x92",	/* cell:22 */
		"\xe8\xab\xb6",	/* cell:23 */
		"\xe8\xb7\x8e",	/* cell:24 */
		"\xe8\xbe\xbb",	/* cell:25 */
		"\xe8\xbf\xb6",	/* cell:26 */
		"\xe9\x83\x9d",	/* cell:27 */
		"\xe9\x84\xa7",	/* cell:28 */
		"\xe9\x84\xad",	/* cell:29 */
		"\xe9\x86\xb2",	/* cell:30 */
		"\xe9\x88\xb3",	/* cell:31 */
		"\xe9\x8a\x88",	/* cell:32 */
		"\xe9\x8c\xa1",	/* cell:33 */
		"\xe9\x8d\x88",	/* cell:34 */
		"\xe9\x96\x92",	/* cell:35 */
		"\xe9\x9b\x9e",	/* cell:36 */
		"\xe9\xa4\x83",	/* cell:37 */
		"\xe9\xa5\x80",	/* cell:38 */
		"\xe9\xab\x99",	/* cell:39 */
		"\xe9\xaf\x96",	/* cell:40 */
		"\xe9\xb7\x97",	/* cell:41 */
		"\xe9\xba\xb4",	/* cell:42 */
		"\xe9\xba\xb5",	/* cell:43 */
		REPLACEMENT_CHARACTER,	/* cell:44 */
		REPLACEMENT_CHARACTER,	/* cell:45 */
		REPLACEMENT_CHARACTER,	/* cell:46 */
		REPLACEMENT_CHARACTER,	/* cell:47 */
		REPLACEMENT_CHARACTER,	/* cell:48 */
		REPLACEMENT_CHARACTER,	/* cell:49 */
		REPLACEMENT_CHARACTER,	/* cell:50 */
		REPLACEMENT_CHARACTER,	/* cell:51 */
		REPLACEMENT_CHARACTER,	/* cell:52 */
		REPLACEMENT_CHARACTER,	/* cell:53 */
		REPLACEMENT_CHARACTER,	/* cell:54 */
		REPLACEMENT_CHARACTER,	/* cell:55 */
		REPLACEMENT_CHARACTER,	/* cell:56 */
		REPLACEMENT_CHARACTER,	/* cell:57 */
		REPLACEMENT_CHARACTER,	/* cell:58 */
		REPLACEMENT_CHARACTER,	/* cell:59 */
		REPLACEMENT_CHARACTER,	/* cell:60 */
		REPLACEMENT_CHARACTER,	/* cell:61 */
		REPLACEMENT_CHARACTER,	/* cell:62 */
		REPLACEMENT_CHARACTER,	/* cell:63 */
		REPLACEMENT_CHARACTER,	/* cell:64 */
		REPLACEMENT_CHARACTER,	/* cell:65 */
		REPLACEMENT_CHARACTER,	/* cell:66 */
		REPLACEMENT_CHARACTER,	/* cell:67 */
		REPLACEMENT_CHARACTER,	/* cell:68 */
		REPLACEMENT_CHARACTER,	/* cell:69 */
		REPLACEMENT_CHARACTER,	/* cell:70 */
		REPLACEMENT_CHARACTER,	/* cell:71 */
		REPLACEMENT_CHARACTER,	/* cell:72 */
		REPLACEMENT_CHARACTER,	/* cell:73 */
		REPLACEMENT_CHARACTER,	/* cell:74 */
		REPLACEMENT_CHARACTER,	/* cell:75 */
		REPLACEMENT_CHARACTER,	/* cell:76 */
		REPLACEMENT_CHARACTER,	/* cell:77 */
		REPLACEMENT_CHARACTER,	/* cell:78 */
		REPLACEMENT_CHARACTER,	/* cell:79 */
		REPLACEMENT_CHARACTER,	/* cell:80 */
		REPLACEMENT_CHARACTER,	/* cell:81 */
		REPLACEMENT_CHARACTER,	/* cell:82 */
		REPLACEMENT_CHARACTER,	/* cell:83 */
		REPLACEMENT_CHARACTER,	/* cell:84 */
		REPLACEMENT_CHARACTER,	/* cell:85 */
		REPLACEMENT_CHARACTER,	/* cell:86 */
		REPLACEMENT_CHARACTER,	/* cell:87 */
		REPLACEMENT_CHARACTER,	/* cell:88 */
		REPLACEMENT_CHARACTER,	/* cell:89 */
		REPLACEMENT_CHARACTER,	/* cell:90 */
		REPLACEMENT_CHARACTER,	/* cell:91 */
		REPLACEMENT_CHARACTER,	/* cell:92 */
		REPLACEMENT_CHARACTER,	/* cell:93 */
		REPLACEMENT_CHARACTER,	/* cell:94 */
	}
};

static const char * const codeset_Latin_extension[94] = {
	"\xc2\xa1",	/* cell:1 */
	"\xc2\xa2",	/* cell:2 */
	"\xc2\xa3",	/* cell:3 */
	"\xe2\x82\xac",	/* cell:4 */
	"\xc2\xa5",	/* cell:5 */
	"\xc5\xa0",	/* cell:6 */
	"\xc2\xa7",	/* cell:7 */
	"\xc5\xa1",	/* cell:8 */
	"\xc2\xa9",	/* cell:9 */
	"\xc2\xaa",	/* cell:10 */
	"\xc2\xab",	/* cell:11 */
	"\xc2\xac",	/* cell:12 */
	"\xc2\xad",	/* cell:13 */
	"\xc2\xae",	/* cell:14 */
	"\xc2\xaf",	/* cell:15 */
	"\xc2\xb0",	/* cell:16 */
	"\xc2\xb1",	/* cell:17 */
	"\xc2\xb2",	/* cell:18 */
	"\xc2\xb3",	/* cell:19 */
	"\xc5\xbd",	/* cell:20 */
	"\xc2\xb5",	/* cell:21 */
	"\xc2\xb6",	/* cell:22 */
	"\xc2\xb7",	/* cell:23 */
	"\xc5\xbe",	/* cell:24 */
	"\xc2\xb9",	/* cell:25 */
	"\xc2\xba",	/* cell:26 */
	"\xc2\xbb",	/* cell:27 */
	"\xc5\x92",	/* cell:28 */
	"\xc5\x93",	/* cell:29 */
	"\xc5\xb8",	/* cell:30 */
	"\xc2\xbf",	/* cell:31 */
	"\xc3\x80",	/* cell:32 */
	"\xc3\x81",	/* cell:33 */
	"\xc3\x82",	/* cell:34 */
	"\xc3\x83",	/* cell:35 */
	"\xc3\x84",	/* cell:36 */
	"\xc3\x85",	/* cell:37 */
	"\xc3\x86",	/* cell:38 */
	"\xc3\x87",	/* cell:39 */
	"\xc3\x88",	/* cell:40 */
	"\xc3\x89",	/* cell:41 */
	"\xc3\x8a",	/* cell:42 */
	"\xc3\x8b",	/* cell:43 */
	"\xc3\x8c",	/* cell:44 */
	"\xc3\x8d",	/* cell:45 */
	"\xc3\x8e",	/* cell:46 */
	"\xc3\x8f",	/* cell:47 */
	"\xc3\x90",	/* cell:48 */
	"\xc3\x91",	/* cell:49 */
	"\xc3\x92",	/* cell:50 */
	"\xc3\x93",	/* cell:51 */
	"\xc3\x94",	/* cell:52 */
	"\xc3\x95",	/* cell:53 */
	"\xc3\x96",	/* cell:54 */
	"\xc3\x97",	/* cell:55 */
	"\xc3\x98",	/* cell:56 */
	"\xc3\x99",	/* cell:57 */
	"\xc3\x9a",	/* cell:58 */
	"\xc3\x9b",	/* cell:59 */
	"\xc3\x9c",	/* cell:60 */
	"\xc3\x9d",	/* cell:61 */
	"\xc3\x9e",	/* cell:62 */
	"\xc3\x9f",	/* cell:63 */
	"\xc3\xa0",	/* cell:64 */
	"\xc3\xa1",	/* cell:65 */
	"\xc3\xa2",	/* cell:66 */
	"\xc3\xa3",	/* cell:67 */
	"\xc3\xa4",	/* cell:68 */
	"\xc3\xa5",	/* cell:69 */
	"\xc3\xa6",	/* cell:70 */
	"\xc3\xa7",	/* cell:71 */
	"\xc3\xa8",	/* cell:72 */
	"\xc3\xa9",	/* cell:73 */
	"\xc3\xaa",	/* cell:74 */
	"\xc3\xab",	/* cell:75 */
	"\xc3\xac",	/* cell:76 */
	"\xc3\xad",	/* cell:77 */
	"\xc3\xae",	/* cell:78 */
	"\xc3\xaf",	/* cell:79 */
	"\xc3\xb0",	/* cell:80 */
	"\xc3\xb1",	/* cell:81 */
	"\xc3\xb2",	/* cell:82 */
	"\xc3\xb3",	/* cell:83 */
	"\xc3\xb4",	/* cell:84 */
	"\xc3\xb5",	/* cell:85 */
	"\xc3\xb6",	/* cell:86 */
	"\xc3\xb7",	/* cell:87 */
	"\xc3\xb8",	/* cell:88 */
	"\xc3\xb9",	/* cell:89 */
	"\xc3\xba",	/* cell:90 */
	"\xc3\xbb",	/* cell:91 */
	"\xc3\xbc",	/* cell:92 */
	"\xc3\xbd",	/* cell:93 */
	"\xc3\xbe",	/* cell:94 */
};

static const char * const codeset_Special_character1[1] = {
	"\xe2\x99\xaa",
};

static const char * const codeset_Special_character2[8] = {
	"\xc2\xa4",
	"\xc2\xa6",
	"\xc2\xa8",
	"\xc2\xb4",
	"\xc2\xb8",
	"\xc2\xbc",
	"\xc2\xbd",
	"\xc2\xbe",
};

static const char * const codeset_Special_character3[12] = {
	"\xe2\x80\xa6",
	"\xe2\x96\xae",
	"\xe2\x80\x98",
	"\xe2\x80\x99",
	"\xe2\x80\x9c",
	"\xe2\x80\x9d",
	"\xe2\x80\xa2",
	"\xe2\x84\xa2",
	"\xe2\x85\x9b",
	"\xe2\x85\x9c",
	"\xe2\x85\x9d",
	"\xe2\x85\x9e",
};

IsdbDecode isdb_decode_open(ISDBTYPE isdb)
{
	_IsdbDecode *_handle;

	_handle = malloc(sizeof(*_handle));
	assert(NULL != _handle);

	_handle->isdb = isdb;

	_handle->cd = iconv_open("UTF-8", "ISO-2022-JP-3");
	assert((iconv_t)0 <= _handle->cd);

	return (IsdbDecode)_handle;
}

void isdb_decode_close(IsdbDecode handle)
{
	_IsdbDecode *_handle;

	_handle = (_IsdbDecode *)handle;
	assert(NULL != _handle);

	iconv_close(_handle->cd);
	free(_handle);
}

static CODESET convert_codeset1(unsigned char code)
{
	switch (code) {
	case CODESET_ALPHANUMERIC:	return Calpha;
	case CODESET_HIRAGANA:	return Chiragana;
	case CODESET_KATAKANA:	return Ckatakana;
	case CODESET_P_ALPHANUMERIC:	return CPalpha;
	case CODESET_P_HIRAGANA:	return CPhiragana;
	case CODESET_P_KATAKANA:	return CPkatakana;
	case CODESET_JISX0201KATAKANA:	return CJkatakana;
	}

	return Cnull;
}

static CODESET convert_codeset2(unsigned char code)
{
	switch (code) {
	case CODESET_KANJI:	return Ckanji;
	case CODESET_JIS_KANJI1:	return CJkanji1;
	case CODESET_JIS_KANJI2:	return CJkanji2;
	case CODESET_ADDITIONAL_SYMBOLS:	return Cadd;
	}

	return Cnull;
}

static void update_selectinfo_drcs2_5(SELECTINFO *si, IBUF *ibuf)
{
	unsigned char c;

	if (!IBUF_isremain(ibuf))
		return;
	si = si;

	c = IBUF_get(ibuf);
}

static void update_selectinfo_drcs1_4(SELECTINFO *si, IBUF *ibuf)
{
	unsigned char c;

	if (!IBUF_isremain(ibuf))
		return;
	si = si;

	c = IBUF_get(ibuf);
}

static void update_selectinfo_gset2_4(SELECTINFO *si, IBUF *ibuf)
{
	if (!IBUF_isremain(ibuf))
		return;

	switch (IBUF_get(ibuf)) {
	case DRCS2BYTE_4:
		update_selectinfo_drcs2_5(si, ibuf);
		break;

	default:
		switch (IBUF_geti(ibuf, (-2))) {
		case GSET2BYTE_3_G1:
			si->g1 = convert_codeset2(IBUF_geti(ibuf, (-1)));
			break;

		case GSET2BYTE_3_G2:
			si->g2 = convert_codeset2(IBUF_geti(ibuf, (-1)));
			break;

		case GSET2BYTE_3_G3:
			si->g3 = convert_codeset2(IBUF_geti(ibuf, (-1)));
			break;
		}
		break;
	}
}

static void update_selectinfo_gset2_3(SELECTINFO *si, IBUF *ibuf)
{
	if (!IBUF_isremain(ibuf))
		return;

	switch (IBUF_get(ibuf)) {
	case GSET2BYTE_3_G1:
	case GSET2BYTE_3_G2:
	case GSET2BYTE_3_G3:
	case DRCS2BYTE_3_G0:
		update_selectinfo_gset2_4(si, ibuf);
		break;

	default:
		si->g0 = convert_codeset2(IBUF_geti(ibuf, (-1)));
		break;
	}
}

static void update_selectinfo_gset1_3(SELECTINFO *si, IBUF *ibuf)
{
	if (!IBUF_isremain(ibuf))
		return;

	switch (IBUF_get(ibuf)) {
	case DRCS1BYTE_3:
		update_selectinfo_drcs1_4(si, ibuf);
		break;

	default:
		switch (IBUF_geti(ibuf, (-2))) {
		case GSET1BYTE_2_G0:
			si->g0 = convert_codeset1(IBUF_geti(ibuf, (-1)));
			break;

		case GSET1BYTE_2_G1:
			si->g1 = convert_codeset1(IBUF_geti(ibuf, (-1)));
			break;

		case GSET1BYTE_2_G2:
			si->g2 = convert_codeset1(IBUF_geti(ibuf, (-1)));
			break;

		case GSET1BYTE_2_G3:
			si->g3 = convert_codeset1(IBUF_geti(ibuf, (-1)));
			break;
		}
		break;
	}
}

static void update_selectinfo_esc(SELECTINFO *si, IBUF *ibuf)
{
	if (!IBUF_isremain(ibuf))
		return;

	switch (IBUF_get(ibuf)) {
	case LS2_2:
		si->gl = G2;
		break;

	case LS3_2:
		si->gl = G3;
		break;

	case LS1R_2:
		si->gr = G1;
		break;

	case LS2R_2:
		si->gr = G2;
		break;

	case LS3R_2:
		si->gr = G3;
		break;

	case GSET1BYTE_2_G0:
	case GSET1BYTE_2_G1:
	case GSET1BYTE_2_G2:
	case GSET1BYTE_2_G3:
		update_selectinfo_gset1_3(si, ibuf);
		break;

	case GSET2BYTE_2:
		update_selectinfo_gset2_3(si, ibuf);
		break;

	default:
		break;
	}
}

static int update_selectinfo(SELECTINFO *si, IBUF *ibuf)
{
	unsigned char c;

	switch (IBUF_geti(ibuf, 0)) {
	case LS0:
		c = IBUF_get(ibuf);
		si->gl = G0;
		break;

	case LS1:
		c = IBUF_get(ibuf);
		si->gl = G1;
		break;

	case ESC:
		c = IBUF_get(ibuf);
		update_selectinfo_esc(si, ibuf);
		break;

	case SS2:
		c = IBUF_get(ibuf);
		si->ss = G2;
		break;

	case SS3:
		c = IBUF_get(ibuf);
		si->ss = G3;
		break;

	default:
		return 0;
	}

	return 1;
}

static void getcodeset(GLR glr, SELECTINFO *si, CODESET *cs, GSET *ss)
{
	GSET gs;

	*ss = si->ss;
	if (GL == glr) {
		if (G2 == si->ss) {
			gs = si->ss;
			si->ss = Gnull;
		}
		else if (G3 == si->ss) {
			gs = si->ss;
			si->ss = Gnull;
		}
		else {
			gs = si->gl;
		}
	}
	else {
		gs = si->gr;
	}

	*cs = Cnull;
	switch (gs) {
	case Gnull:
		break;

	case G0:
		*cs = si->g0;
		break;

	case G1:
		*cs = si->g1;
		break;

	case G2:
		*cs = si->g2;
		break;

	case G3:
		*cs = si->g3;
		break;
	}
}

static void convert_str_replacement_character(OBUF *obuf)
{
	const unsigned char *code;

	code = (const unsigned char *)REPLACEMENT_CHARACTER;
	while (*code) {
		assert(OBUF_isremain(obuf));
		OBUF_put(obuf, *code++);
	}
}

static void convert_str_kanji(iconv_t cd, GLR glr, CODESET cs,
							  GSET ss, unsigned char c,
							  IBUF *ibuf, OBUF *obuf)
{
	int row, cell;
	const unsigned char *code;
	unsigned char c1;
	char buf[3 + 1024 * 2], *p;
	size_t size, left, rslt;

	if (!IBUF_isremain(ibuf))
		return;

	p = buf;
	switch (cs) {
	case Ckanji:
		row = c - 0x20;
		if (ISADDITIONAL_SYMBOLS(row)) {
			/* Additional symbols */
			cell = (IBUF_get(ibuf) & 0x7f) - 0x20;
			if (90 == row &&
				((45 <= cell && cell <= 63) ||
				 (66 <= cell && cell <= 84))) {
				/* ARIB STD-B3 */
				convert_str_replacement_character(obuf);
			}

			code = (const unsigned char *)codeset_Additional_symbols90[row - 90][cell - 1];
			while (*code) {
				assert(OBUF_isremain(obuf));
				OBUF_put(obuf, *code++);
			}
			return;
		}
		*p++ = ESC;
		*p++ = '$';
		*p++ = 'B';
		break;

	case CJkanji1:
		*p++ = ESC;
		*p++ = '$';
		*p++ = '(';
		*p++ = 'Q';
		break;

	case CJkanji2:
		*p++ = ESC;
		*p++ = '$';
		*p++ = '(';
		*p++ = 'P';
		break;

	default:
		return;
	}

	*p++ = (char)c;
	*p++ = (char)(IBUF_get(ibuf) & 0x7f);
	if (Gnull == ss) {
		/* Locking shift */
		while (IBUF_isremainn(ibuf, 1) &&
			   p < buf + sizeof(buf)) {
			c1 = IBUF_geti(ibuf, 0);
			if (GL == glr && !ISGL(c1))
				break;
			if (GR == glr && !ISGR(c1))
				break;
			if (Ckanji == cs) {
				c1 &= 0x7f;
				row = c1 - 0x20;
				if (ISADDITIONAL_SYMBOLS(row)) {
					/* Additional symbols */
					break;
				}
			}

			*p++ = (char)(IBUF_get(ibuf) & 0x7f);
			*p++ = (char)(IBUF_get(ibuf) & 0x7f);
		}

	}
	else {
		/* Single shift */
	}
	size = p - buf;
	p = &buf[0];
	left = (size_t)OBUF_putleft(obuf);
	rslt = iconv(cd, &p, &size,
				 (char **)OBUF_putp(obuf), &left);
	assert(!((size_t)(-1) == rslt && E2BIG == errno));
}

static void convert_str_alpha(unsigned char c1, OBUF *obuf)
{
	if (c1 == 0x7e) {
		/* over line */
		assert(OBUF_isremainn(obuf, 3));
		OBUF_put(obuf, (unsigned char)0xe2);
		OBUF_put(obuf, (unsigned char)0x80);
		OBUF_put(obuf, (unsigned char)0xbe);
		return;
	}

	assert(OBUF_isremain(obuf));
	OBUF_put(obuf, (unsigned char)codeset_Alphanumeric[c1 - 0x21]);
}

static void convert_str_hiragana(const char codeset[], int size,
								 unsigned char c1, OBUF *obuf)
{
	int ind;

	if (c1 - 0x21 >= size) {
		convert_str_replacement_character(obuf);
		return;
	}
	ind = ((int)c1 - 0x21) * 3;
	assert(OBUF_isremainn(obuf, 3));
	OBUF_put(obuf, (unsigned char)codeset[ind++]);
	OBUF_put(obuf, (unsigned char)codeset[ind++]);
	OBUF_put(obuf, (unsigned char)codeset[ind++]);
}

static void convert_str_add(unsigned char c1, unsigned char c2, OBUF *obuf)
{
	int row, cell;
	const unsigned char *code;

	row = c1 - 0x20;
	cell = c2 - 0x20;
	if (ISADDITIONAL_SYMBOLS(row)) {
		/* Additional symbols */
		code = (const unsigned char *)codeset_Additional_symbols90[row - 90][cell - 1];
	}
	else if (ISADDITIONAL_KANJI(row)) {
		/* Additional kanji */
		code = (const unsigned char *)codeset_Additional_symbols85[row - 85][cell - 1];
	}
	else {
		convert_str_replacement_character(obuf);
		return;
	}
	while (*code) {
		assert(OBUF_isremain(obuf));
		OBUF_put(obuf, *code++);
	}
}

static void convert_str_extension(unsigned char c1, OBUF *obuf)
{
	const unsigned char *code;

	code = (const unsigned char *)codeset_Latin_extension[c1 - 0x21];
	while (*code) {
		assert(OBUF_isremain(obuf));
		OBUF_put(obuf, *code++);
	}
}

static void convert_str_special(unsigned char c1, OBUF *obuf)
{
	const unsigned char *code;

	if (0x21 <= c1 && c1 <= 0x21)
		code = (const unsigned char *)codeset_Special_character1[c1 - 0x21];
	else if (0x30 <= c1 && c1 <= 0x37)
		code = (const unsigned char *)codeset_Special_character2[c1 - 0x30];
	else if (0x40 <= c1 && c1 <= 0x4b)
		code = (const unsigned char *)codeset_Special_character2[c1 - 0x40];
	else {
		convert_str_replacement_character(obuf);
		return;
	}
	while (*code) {
		assert(OBUF_isremain(obuf));
		OBUF_put(obuf, *code++);
	}
}

static void convert_str_codeset(iconv_t cd, GLR glr, CODESET cs,
								GSET ss, unsigned char c,
								IBUF *ibuf, OBUF *obuf)
{
	unsigned char c2;

	switch (cs) {
	case Ckanji:
	case CJkanji1:
	case CJkanji2:
		convert_str_kanji(cd, glr, cs, ss, c, ibuf, obuf);
		break;

	case Calpha:
	case CPalpha:
		convert_str_alpha(c, obuf);
		break;

	case Chiragana:
	case CPhiragana:
		convert_str_hiragana(codeset_Hiragana, 94, c, obuf);
		break;

	case Ckatakana:
	case CPkatakana:
		convert_str_hiragana(codeset_Katakana, 94, c, obuf);
		break;

	case CJkatakana:
		convert_str_hiragana(codeset_JISX0201katakana, 63, c, obuf);
		break;

	case Cadd:
		if (!IBUF_isremain(ibuf))
			break;
		c2 = IBUF_get(ibuf);
		convert_str_add(c, (c2 & 0x7f), obuf);
		break;

	case Cextension:
		convert_str_extension(c, obuf);
		break;

	case Cspecial:
		convert_str_special(c, obuf);
		break;

	default:
		break;
	}
}

static void skip_cs0(unsigned char c, IBUF *ibuf)
{
	switch (c) {
	case PAPF:
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		break;

	case APS:
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		break;

	default:
		break;
	}
}

static void skip_cs1(unsigned char c, IBUF *ibuf)
{
	switch (c) {
	case POL:
	case SZX:
	case FLC:
	case WMM:
	case RPC:
	case HLC:
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		break;

	case COL:
	case CDC:
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		if (c == 0x20) {
			if (!IBUF_isremain(ibuf)) break;
			c = IBUF_get(ibuf);
		}
		break;

	case TIME:
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		if (c == 0x20) {
			if (!IBUF_isremain(ibuf)) break;
			c = IBUF_get(ibuf);
		}
		else if (c == 0x28) {
			if (!IBUF_isremain(ibuf)) break;
			c = IBUF_get(ibuf);
		}
		else if (c == 0x29) {
			while (IBUF_isremain(ibuf)) {
				c = IBUF_get(ibuf);
				if (0x40 <= c && c <= 0x42)
					break;
			}
		}
		break;

	case MACRO:
		while (IBUF_isremain(ibuf)) {
			c = IBUF_get(ibuf);
			if (c == MACRO)
				break;
		}
		if (!IBUF_isremain(ibuf)) break;
		c = IBUF_get(ibuf);
		break;

	case CSI:
		while (IBUF_isremain(ibuf)) {
			c = IBUF_get(ibuf);
			if (0x40 <= c && c <= 0x6f)
				break;
		}
		break;

	default:
		break;
	}
}

unsigned int isdb_decode_text(IsdbDecode handle,
							  const unsigned char *src, unsigned int src_len,
							  unsigned char *dst, unsigned int dst_len)
{
	_IsdbDecode *_handle;
	unsigned int rslt;
	SELECTINFO si;
	CODESET cs;
	GSET ss;
	IBUF ibuf;
	OBUF obuf;
	unsigned char c;

	_handle = (_IsdbDecode *)handle;
	assert(NULL != _handle);

	rslt = 0;
	switch (_handle->isdb) {
	case ISDB_ARIB:
		si.gl = G0;
		si.gr = G2;
		si.ss = Gnull;
		si.g0 = Ckanji;
		si.g1 = Calpha;
		si.g2 = Chiragana;
		si.g3 = Ckatakana;	/* macro??? */
		break;

	case ISDB_ABNT:
		si.gl = G0;
		si.gr = G1;
		si.g0 = Calpha;
		si.g1 = Cextension;
		break;
	}

	IBUF_init(&ibuf, src, src_len);
	OBUF_init(&obuf, dst, dst_len);
	while (IBUF_isremain(&ibuf)) {
		while (update_selectinfo(&si, &ibuf) &&
			   IBUF_isremain(&ibuf))
			;
		if (!IBUF_isremain(&ibuf))
			break;

		c = IBUF_get(&ibuf);
		if (c < 0x80) {
			if (ISGL(c)) {
				getcodeset(GL, &si, &cs, &ss);
				convert_str_codeset(_handle->cd, GL, cs, ss, c, &ibuf, &obuf);
			}
			else if (c == SP ||
					 c == APD) {
				assert(OBUF_isremain(&obuf));
				OBUF_put(&obuf, c);
			}
			else {
				skip_cs0(c, &ibuf);
			}
		}
		else {
			if (ISGR(c)) {
				getcodeset(GR, &si, &cs, &ss);
				convert_str_codeset(_handle->cd, GR, cs, ss, (c & 0x7f), &ibuf, &obuf);
			}
			else {
				skip_cs1(c, &ibuf);
			}
		}
	}
	rslt = dst_len - (unsigned int)OBUF_putleft(&obuf);

#ifdef DEBUG
	{
		struct timeval tv;
		struct tm *tm;
		FILE *fpsrc, *fpsrc2, *fpdst;
		char mark[128];
		unsigned int i;

		gettimeofday(&tv, NULL);
		tm = localtime(&tv.tv_sec);
		sprintf(mark, "%02d:%02d:%02d.%06ld ",
				tm->tm_hour, tm->tm_min, tm->tm_sec, (long int)tv.tv_usec);

		pthread_mutex_lock(&mut);
		fpsrc = fopen("/tmp/isdb_decode_src.txt", "a");
		fpsrc2 = fopen("/tmp/isdb_decode_src2.txt", "a");
		fpdst = fopen("/tmp/isdb_decode_dst.txt", "a");
		assert(fpsrc && fpdst);

		fputs(mark, fpsrc);
		for (i = 0; i < src_len; i++) {
			if (i % 20 == 0 && i > 0)
				fputs("\n", fpsrc);
			fprintf(fpsrc, "%02x ", src[i]);
		}
		fputs("\n", fpsrc);

		fputs(mark, fpsrc2);
		fwrite(src, (size_t)src_len, (size_t)1, fpsrc2);
		fputs("\n", fpsrc2);

		fputs(mark, fpdst);
		fwrite(dst, (size_t)rslt, (size_t)1, fpdst);
		fputs("\n", fpdst);

		fclose(fpdst);
		fclose(fpsrc2);
		fclose(fpsrc);
		pthread_mutex_unlock(&mut);
	}
#endif

	return rslt;
}

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * tab-width: 4
 * End:
 */

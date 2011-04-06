#ifndef ISDB_DECODE_TEXT_H
#define ISDB_DECODE_TEXT_H

typedef enum { ISDB_ARIB, ISDB_ABNT } ISDBTYPE;
typedef void *IsdbDecode;

#ifdef __cplusplus
extern "C" {
#endif

IsdbDecode isdb_decode_open(ISDBTYPE isdb);
void isdb_decode_close(IsdbDecode handle);
unsigned int isdb_decode_text(IsdbDecode handle,
							  const unsigned char *src, unsigned int src_len,
							  unsigned char *dst, unsigned int dst_len);
#ifdef __cplusplus
}
#endif

#endif	/* ISDB_DECODE_TEXT_H */

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * tab-width: 4
 * End:
 */

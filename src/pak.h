/* Engine-native pak files (docs/adr-001-data-formats.md).
 *
 *   header   "WFPK" u32 version u32 nsections u32 header_crc
 *   sections nsections x { char name[56]; u32 off; u32 len; u32 crc32; u32 pad }
 *   data     bytes, each section 16-byte aligned
 *
 * All integers little-endian (host is x86-64; the reader refuses a foreign
 * byte order by checking the magic). The reader loads the whole file, checks
 * every CRC and hands out pointers into its buffer; the buffer lives until
 * pak_close(). */
#ifndef WF_PAK_H
#define WF_PAK_H
#include <stdint.h>

#define PAK_VERSION 1
#define PAK_NAME_MAX 56

typedef struct pak pak;
typedef struct pak_writer pak_writer;

pak           *pak_open(const char *path);          /* NULL + stderr on any error */
const uint8_t *pak_section(const pak *p, const char *name, uint32_t *len);
int            pak_section_count(const pak *p);
const char    *pak_section_name(const pak *p, int i);
void           pak_close(pak *p);

pak_writer *pak_writer_new(void);
int         pak_writer_add(pak_writer *w, const char *name, const uint8_t *bytes, uint32_t len);
int         pak_writer_save(pak_writer *w, const char *path);   /* writes, verifies by re-open */
void        pak_writer_free(pak_writer *w);
#endif

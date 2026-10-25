/* Strict SSN stored/single-gzip-member decoder; caller hashes decoded bytes. */
#ifndef RUNESCAPE_PIECE_H
#define RUNESCAPE_PIECE_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../../addon-worker/third_party/miniz/miniz_tinfl.h"
#ifndef RS_PIECE_MAX
#define RS_PIECE_MAX 4194304u
#endif
static uint32_t rs_le32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint32_t rs_crc(const unsigned char *p,size_t n)
{
    static const uint32_t table[16]={0x00000000u,0x1db71064u,0x3b6e20c8u,0x26d930acu,
        0x76dc4190u,0x6b6b51f4u,0x4db26158u,0x5005713cu,0xedb88320u,0xf00f9344u,
        0xd6d6a3e8u,0xcb61b38cu,0x9b64c2b0u,0x86d3d2d4u,0xa00ae278u,0xbdbdf21cu};
    uint32_t crc=~0u;
    while(n--) { crc^=*p++;crc=(crc>>4)^table[crc&15];crc=(crc>>4)^table[crc&15]; }
    return ~crc;
}
static int rs_unpack(const unsigned char *wire,size_t bytes,unsigned char *out,size_t *length)
{
    static const unsigned char magic[]={0x6e,0x2b,0x3b,0x61,0};
    if(bytes<=6 || memcmp(wire,magic,sizeof(magic))) return EINVAL;
    /* Method 0 stores the payload verbatim; method 2 is one gzip member.
     * Both variants are bounded and still require the manifest SHA-256. */
    if(wire[5]==0) {
        if(bytes-6>RS_PIECE_MAX) return EINVAL;
        memcpy(out,wire+6,bytes-6); *length=bytes-6; return 0;
    }
    static const unsigned char gzip_header[]={0x1f,0x8b,8,0};
    if(wire[5]!=2 || bytes<24 || memcmp(wire+6,gzip_header,sizeof(gzip_header))) return EINVAL;
    /* Refuse optional fields, concatenation/trailing data and zip bombs. */
    tinfl_decompressor inflater; tinfl_init(&inflater);
    size_t source=bytes-24, output=RS_PIECE_MAX;
    tinfl_status status=tinfl_decompress(&inflater,wire+16,&source,out,out,&output,TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if(status!=TINFL_STATUS_DONE || source!=bytes-24 || !output || output>RS_PIECE_MAX ||
       rs_le32(wire+bytes-4)!=output || rs_le32(wire+bytes-8)!=rs_crc(out,output)) return EINVAL;
    *length=output; return 0;
}
#endif

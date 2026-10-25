/* Real strict parser contracts; no Jagex payloads or network. */
#include <assert.h>
#include "runescape-ssn.h"
static struct rs_json json;
static struct rs_manifest manifest;
static const char id[]="0000000000000000000000000000000000000000000000000000000000000000";
static int parse_manifest(const char *files,const char *algorithm,const char *padding,const char *digest)
{
    char text[4096];
    snprintf(text,sizeof(text),"{\"algorithm\":\"%s\",\"id\":\"%s\",\"version\":\"3.0.0\",\"files\":%s,\"pieces\":{\"algorithm\":\"sha256\",\"hashPadding\":%s,\"digests\":[\"%s\"]}}",algorithm,id,files,padding,digest);
    return rs_parse(&json,text,strlen(text)) || rs_manifest_parse(&json,&manifest,id);
}
int main(void)
{
    static const char *bad_paths[]={"","/absolute","../bad","a/../bad","C:bad","C:/bad","//server/file","a\\b","a//b","a/./b","a/","a.","a ","CON","con.txt","LPT1.dll","a:b","a?b"};
    for(unsigned i=0;i<sizeof(bad_paths)/sizeof(*bad_paths);i++) assert(!rs_path(bad_paths[i]));
    assert(rs_path("JagexLauncher.exe") && rs_path("locales/en-GB.pak"));
    assert(!rs_path("a/b/c/d/e/f/g/h/i"));
    assert(rs_parse(&json,"{}",RS_JSON_MAX+1));
    assert(rs_id(id) && !rs_id("not-an-id"));
    assert(rs_case_equal("LocalES/EN.pak","locales/en.PAK"));
    char digest[65];
    assert(!rs_digest("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",digest) && !strcmp(digest,id));
    assert(rs_digest("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB=",digest));
    assert(rs_digest("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA!",digest));
    assert(rs_digest("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==",digest));
    const char *zero="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
    assert(!parse_manifest("[{\"attr\":0,\"name\":\"JagexLauncher.exe\",\"size\":3},{\"attr\":0,\"name\":\"empty\",\"size\":0}]","sha256","false",zero));
    assert(manifest.files==2 && manifest.pieces==1 && manifest.bytes==3);
    assert(parse_manifest("[{\"attr\":0,\"name\":\"../escape\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"INSTALLER.exe\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\".ssn-lengths\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":1},{\"attr\":0,\"name\":\"A\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":1},{\"attr\":0,\"name\":\"A/b\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":1,\"name\":\"a\",\"size\":1}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":2147483649}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":18446744073709551616}]","sha256","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":1}]","sha1","false",zero));
    assert(parse_manifest("[{\"attr\":0,\"name\":\"a\",\"size\":1}]","sha256","true",zero));
    assert(rs_parse(&json,"{\"a\":1,\"a\":2}",13));
    assert(rs_parse(&json,"{\"a\":01}",8));
    assert(rs_parse(&json,"{\"a\":-1}",8));
    assert(rs_parse(&json,"{\"a\":\"\\u002e\"}",14));
    assert(rs_parse(&json,"{}x",3));
    assert(rs_parse(&json,"{\"a\":[1,]}",10));
    const char *alias="{\"launcher-win-x86_64.production\":\"ok\",\"launcher-win-x86_64.preview\":\"wrong\"}";
    assert(!rs_parse(&json,alias,strlen(alias)));
    assert(rs_equal(&json,rs_field(&json,0,"launcher-win-x86_64.production"),"ok"));
    assert(rs_field(&json,0,"launcher-win.production")==-1);
    puts("runescape SSN parser contracts passed");
    return 0;
}

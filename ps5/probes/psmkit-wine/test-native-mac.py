#!/usr/bin/env python3
"""Actual B1 parent/child MAC forwarding; no console or hardware-address logs."""
import argparse
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
here = Path(__file__).resolve().parent
spec = spec_from_file_location('mac_runtime_imports', here / 'runtime_imports.py')
imports = module_from_spec(spec)
spec.loader.exec_module(imports)
assert imports.B1_NATIVE_FORWARDED.get('sceNetGetMacAddress') == 'pw_net_get_mac_address', 'MAC must use parent/forwarded provider, not an absent child libSceNet import'
spec = spec_from_file_location('mac_extract', here / 'build-desktop-surface-test.py')
extract = module_from_spec(spec)
spec.loader.exec_module(extract)
forwarder = extract.definition(here / 'native-entry.c', 'pw_net_get_mac_address')
dispatcher = extract.definition(here / 'native-entry.c', 'pw_native_execute')
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"
static const uint8_t fixture_address[6]={2,17,34,51,68,85};
static int calls, expected_flags, native_status, native_errno;
static uint8_t *expected_pointer;
#ifdef PW_PARENT_ROLE
int sceNetGetMacAddress(uint8_t *address,int flags) {
    assert(flags==expected_flags);calls++;
    if(!address) return -66;
    if(!native_status) memcpy(address,fixture_address,6);
    return native_status;
}
int *sceNetErrnoLoc(void) { return &native_errno; }
int sceNetPoolCreate(const char *n,int s,int f) { (void)n;(void)s;(void)f;assert(0);return -1; }
int sceNetPoolDestroy(int p) { (void)p;assert(0);return -1; }
int sceNetResolverCreate(const char *n,int p,int f) { (void)n;(void)p;(void)f;assert(0);return -1; }
int sceNetResolverDestroy(int p) { (void)p;assert(0);return -1; }
static int pw_resolver_ntoa_timed(int r,const char *n,uint32_t *a,int t,int y,int f,const char *v) {
    (void)r;(void)n;(void)a;(void)t;(void)y;(void)f;(void)v;assert(0);return -1;
}
int sceNetResolverStartAton(int r,const uint32_t *a,char *n,int s,int t,int y,int f) {
    (void)r;(void)a;(void)n;(void)s;(void)t;(void)y;(void)f;assert(0);return -1;
}
int sceNetCtlInit(void) { assert(0);return -1; }
int sceNetCtlGetInfo(int c,void *o) { (void)c;(void)o;assert(0);return -1; }
int sceSystemServiceParamGetString(int c,char *o,size_t s) { (void)c;(void)o;(void)s;assert(0);return -1; }
#else
static int pw_child_native(const struct pw_native_request *request,struct pw_native_reply *reply) {
    assert(request->op==PW_NET_GET_MAC_ADDRESS && request->arg[0]==expected_flags);
    assert(request->arg[1]==!!expected_pointer);
    for(unsigned i=2;i<6;i++) assert(!request->arg[i]);
    for(unsigned i=0;i<PW_NATIVE_BYTES;i++) assert(!request->text[i]);
    calls++;memset(reply,0x7f,sizeof(*reply));memcpy(reply->data,fixture_address,6);
    reply->result=expected_pointer ? native_status : -66;reply->net_errno=native_errno;
    return reply->result;
}
#endif
'''
cases = r'''
int main(void) {
    struct { uint32_t before;uint8_t address[6];uint8_t after[4]; } value={0x12345678,{0},{9,8,7,6}};
    assert(PW_NET_POOL_CREATE==1 && PW_NET_CTL_GET_INFO==8 && PW_SS_PARAM_GET_STRING==9 && PW_NET_GET_MAC_ADDRESS==10);
    expected_pointer=value.address;expected_flags=7;
    assert(!pw_net_get_mac_address(value.address,expected_flags) && calls==1);
    assert(!memcmp(value.address,fixture_address,6) && value.before==0x12345678 && !memcmp(value.after,(uint8_t[]){9,8,7,6},4));
    memset(value.address,0x5a,6);native_status=-77;native_errno=55;
    assert(pw_net_get_mac_address(value.address,expected_flags)==-77 && calls==2);
    assert(!memcmp(value.address,(uint8_t[]){0x5a,0x5a,0x5a,0x5a,0x5a,0x5a},6));
    expected_pointer=NULL;assert(pw_net_get_mac_address(NULL,expected_flags)==-66 && calls==3);
#ifdef PW_PARENT_ROLE
    struct pw_native_request request={.op=PW_NET_GET_MAC_ADDRESS,.arg={7,1}};
    struct pw_native_reply reply;native_status=0;
    assert(!pw_native_execute(&request,&reply) && calls==4 && !reply.result && reply.net_errno==55);
    assert(!memcmp(reply.data,fixture_address,6));
    for(unsigned i=6;i<PW_NATIVE_BYTES;i++) assert(!reply.data[i]);
    request.arg[1]=0;assert(!pw_native_execute(&request,&reply) && calls==5 && reply.result==-66);
    puts("NATIVE_MAC_PARENT_HOST_PASS real-dispatch six-byte-bound flags null status no-hardware-logs");
#else
    puts("NATIVE_MAC_CHILD_HOST_PASS forwarded-only six-byte-bound flags null failure-output-preserved");
#endif
    return 0;
}
'''
args.out.mkdir(parents=True, exist_ok=True)
for parent in (True, False):
    source = args.out / ('test-native-mac-' + ('parent' if parent else 'child') + '.c')
    source.write_text(harness + '\n' + (dispatcher if parent else '') + '\n' + forwarder + '\n' + cases)
    binary = source.with_suffix('')
    subprocess.run(shlex.split(args.cc) + ['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
        *(['-DPW_PARENT_ROLE'] if parent else []), '-I',str(here),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

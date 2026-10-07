/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>

/* The suite is POSIX-shaped (mkdtemp/symlink); MinGW has direct.h/io.h equivalents and no
 * symlink, so the overlay link is made a real copy there. The code under test is unchanged
 * either way — runtime_file.c already has its own _WIN32 branches. */
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <stdlib.h>
/* MinGW has no /tmp; the test only needs a writable directory it can delete afterwards. */
static const char* bb_tempdir(void) {
    const char *tmp = getenv("TEMP");
    return tmp && tmp[0] ? tmp : ".";
}
/* MinGW has neither mkdtemp (it only rewrites the template, it does not create anything) nor
 * /tmp. Build a uniquely named directory under TEMP instead: the pid keeps concurrent runs
 * apart, and the loop covers the (practically impossible) case of a stale leftover. */
static int bb_mkdtemp(char *path, size_t size) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        snprintf(path, size, "%s/bbport-mod-files-%lu-%d", bb_tempdir(),
                 (unsigned long)_getpid(), attempt);
        if (_mkdir(path) == 0) return 0;
    }
    return -1;
}
#define bb_rmdir _rmdir
static int bb_mkdir(const char *p) { return _mkdir(p); }
static int bb_unlink(const char *p) { return _unlink(p); }
static int bb_symlink(const char *target, const char *linkpath) {
    /* No symlink on Windows. The guest only ever reads through the link, so materialising it
     * as a copy exercises the same code path; a real link would additionally need
     * privileges or developer mode. */
    FILE *in = fopen(target, "rb");
    if (!in) return -1;
    FILE *out = fopen(linkpath, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    return fclose(out) == 0 ? 0 : -1;
}
#else
#include <unistd.h>
#define bb_mkdtemp(p) mkdtemp(p)
#define bb_rmdir(p) rmdir(p)
static int bb_mkdir(const char *p) { return mkdir(p, 0755); }
static int bb_unlink(const char *p) { return unlink(p); }
static int bb_symlink(const char *target, const char *linkpath) { return symlink(target, linkpath); }
static const char* bb_tempdir(void) { return "/tmp"; }
#endif

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    char root[512];
    assert(!bb_mkdtemp(root, sizeof root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!bb_mkdir(game));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
    assert(!bb_symlink(source,link));
    runtime_file_configure(game,user);
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    { int rc = bb_unlink(link); assert(!rc);
      rc = bb_unlink(source); assert(!rc);
      rc = bb_rmdir(game); assert(!rc); }
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!bb_rmdir(p)); }
    assert(!bb_rmdir(user) && !bb_rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, readonly assets and writable saves PASS");
}

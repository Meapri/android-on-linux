/*
 * libalr_fakeroot.c — ALR fakeroot LD_PRELOAD .so (root/uid emulation).
 *
 * PROBLEM
 * -------
 * On non-root Android (untrusted_app, no CAP_*), `apt install` stalls with
 *   dpkg: error: requires superuser privilege
 * (docs/evidence/2026-06-02-round10-step2-inproc-remap-mapjump.md). dpkg refuses
 * to unpack because it is not uid 0 and its chown()/mknod() of unpacked files
 * would EPERM. This is INDEPENDENT of exec-re-entry (which R11/v144 device-proved)
 * — it is a credentials/permissions gate, the same gate proot solves with its
 * "fake uid/gid 0" (fakeroot) feature.
 *
 * MECHANISM (standard libfakeroot, adapted to non-root in-process Android)
 * -----------------------------------------------------------------------
 *  - getuid/geteuid/getgid/getegid (+ getres*id, getgroups) report 0 so dpkg
 *    believes it is root and never bails on the privilege check.
 *  - chown/lchown/fchown/fchownat: NO-OP the real syscall (it would EPERM) but
 *    RECORD (dev,ino)->uid/gid in a shared metadata DB. dpkg sees success.
 *  - chmod/fchmod/fchmodat: apply the real chmod where the kernel allows it
 *    (the guest owns its own rootfs files, so most chmods succeed) AND record
 *    the intended mode in the DB so a subsequent stat reflects exactly what dpkg
 *    set, including setuid/setgid/sticky bits the kernel may have stripped for a
 *    non-root owner.
 *  - mknod/mknodat for char/block specials: the kernel forbids them for a
 *    non-root app, so we create a regular placeholder file (or skip) and record
 *    a (mode=S_IFCHR/BLK, rdev) DB entry; stat then reports it as the special
 *    device dpkg expects. (Most device nodes live in the `udev`/`makedev`
 *    postinst, not the data.tar, so this is rarely on the unpack hot path.)
 *  - stat/lstat/fstat/fstatat/newfstatat/statx (+ *64 LFS and legacy __xstat*
 *    compat): real stat first, then OVERLAY the DB's uid/gid/mode/rdev so dpkg
 *    reads back the root-owned metadata it "set". statx is the modern entry
 *    coreutils/dpkg-deb bind to on noble (glibc 2.39); we overlay its split
 *    major/minor rdev fields via fr_makedev's inverse.
 *
 * The DB (alr_fakeroot_db.h) is a single mmap(MAP_SHARED) file keyed by
 * (st_dev, st_ino), shared across dpkg's fork()+exec() of maintainer scripts —
 * replacing libfakeroot's faked daemon + SysV msg queue (neither available to
 * an untrusted_app). See that header for the concurrency/overflow contract.
 *
 * COEXISTENCE WITH libalr_interpose.so  (THE load-bearing contract — ADR-fakeroot §5)
 * ---------------------------------------------------------------------------------
 * Two LD_PRELOAD .so's cannot BOTH win symbol interposition for the same symbol:
 * the dynamic linker binds an undefined ref in the executable to the FIRST
 * library in search order that defines it (LD_PRELOAD libs are searched in
 * listed order, before the executable's own DT_NEEDED libc). So the
 * responsibilities are PARTITIONED by symbol, never overlapped:
 *
 *   PATH-rewriting symbols  -> OWNED BY libalr_interpose.so ONLY.
 *       open/openat/creat, the whole stat family, access, readlink, opendir,
 *       mkdir, unlink, rename, link/symlink, utimensat, realpath. fakeroot does
 *       NOT define these (except the small set below) so the interposer's
 *       rewrite (guest path -> $ALR_ROOTFS/path, RESOLVE_IN_ROOT) is unaffected.
 *
 *   CREDENTIAL + METADATA-mutating symbols  -> OWNED BY libalr_fakeroot.so ONLY.
 *       getuid/geteuid/getgid/getegid/getgroups/getres[ug]id, setuid/setgid/
 *       seteuid/setegid (succeed-as-0), chown/lchown/fchown/fchownat,
 *       chmod/fchmod/fchmodat, mknod/mknodat, umask passthrough.
 *       The interposer ALSO defines getuid..getegid (memoized to 0-cost) and
 *       chmod/mknod (for PATH rewrite only). To avoid a double-definition tie,
 *       LD_PRELOAD MUST list libalr_fakeroot.so BEFORE libalr_interpose.so so
 *       fakeroot wins these — see the envp ordering contract in §5b. fakeroot's
 *       chmod/mknod/chown wrappers THEMSELVES rewrite the path (using the SAME
 *       $ALR_ROOTFS prefix rule, re-implemented here) before the real syscall,
 *       so partitioning the symbol away from the interposer does NOT lose path
 *       mediation for it.
 *
 *   THE stat OVERLAY  -> the one genuinely shared symbol family.
 *       The interposer needs stat to REWRITE the path; fakeroot needs stat to
 *       OVERLAY metadata AFTER the kernel fills the buffer. We resolve this WITHOUT
 *       both defining `stat`:  fakeroot defines the stat family and wins (listed
 *       first); inside, fakeroot does NOT itself rewrite the path — it calls
 *       dlsym(RTLD_NEXT, "stat"), which resolves to the INTERPOSER's `stat`
 *       (the next definition in search order), so the interposer still does its
 *       rewrite + trampoline-emitted newfstatat; on return fakeroot overlays the
 *       DB. Net chain:  guest stat -> fakeroot(stat) -> [RTLD_NEXT] interposer(stat)
 *       [rewrite+emit] -> kernel -> interposer fills buf -> fakeroot overlays DB.
 *       Path responsibility stays 100% with the interposer; metadata overlay is
 *       100% fakeroot. If the interposer is absent, RTLD_NEXT falls through to
 *       libc's real stat — fakeroot still overlays, only without rootfs rewrite
 *       (acceptable: that is the interposer's job, not ours).
 *
 * HARD CONSTRAINTS (unchanged from the ALR charter):
 *   - non-root, public Android API only, no SELinux bypass, W^X-safe (this .so
 *     is ordinary file-backed r-x; no runtime codegen, no anonymous PROT_EXEC).
 *   - in-process; degrades gracefully (DB unset => credential spoof still works,
 *     metadata overlay simply no-ops).
 *
 * Build (host-verified cross-compile):
 *   zig cc --target=aarch64-linux-gnu.2.36 -shared -fPIC -O2 \
 *       -o libalr_fakeroot.so libalr_fakeroot.c
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>     /* getenv */
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include "alr_fakeroot_db.h"

#pragma GCC diagnostic ignored "-Wpointer-bool-conversion"
#pragma GCC diagnostic ignored "-Wtautological-pointer-compare"

/* ============================ tiny byte helpers ============================ */
static size_t fr_len(const char *s){ size_t n=0; if(s) while(s[n]) ++n; return n; }
static int fr_under(const char *p, const char *d){
    size_t i=0; while(d[i]){ if(p[i]!=d[i]) return 0; ++i; }
    return p[i]=='/'||p[i]=='\0';
}

/* ============================ rootfs prefix (mirror of interposer) =========
 * fakeroot's OWN wrappers (chown/chmod/mknod) must rewrite the path because we
 * partitioned them away from the interposer. Identical rule to libalr_interpose
 * rw(): /proc,/sys,/dev and already-under-rootfs pass through; else prepend. */
static char   g_rootfs[1024];
static size_t g_rootfs_len = 0;
static int    g_inited = 0;

static void fr_init_rootfs(void){
    const char *r = getenv("ALR_ROOTFS");
    g_rootfs_len = 0;
    if(!r || r[0] != '/') return;
    size_t n = fr_len(r);
    while(n>1 && r[n-1]=='/') --n;
    if(n>=sizeof(g_rootfs)) n=sizeof(g_rootfs)-1;
    for(size_t i=0;i<n;++i) g_rootfs[i]=r[i];
    g_rootfs[n]='\0';
    g_rootfs_len = (n==1 && g_rootfs[0]=='/') ? 0 : n;
}
static const char *fr_rw(const char *p, char *buf, size_t buflen){
    if(!p || p[0]!='/') return p;
    if(g_rootfs_len==0) return p;
    if(fr_under(p,"/proc")||fr_under(p,"/sys")||fr_under(p,"/dev")) return p;
    if(fr_under(p,g_rootfs)) return p;
    size_t plen=fr_len(p);
    if(g_rootfs_len+plen+1>buflen) return p;
    size_t i=0; for(;i<g_rootfs_len;++i) buf[i]=g_rootfs[i];
    for(size_t j=0;j<=plen;++j) buf[i+j]=p[j];
    return buf;
}
#define FR_PBUF 2304

/* ============================ metadata DB (shared mmap) ==================== */
static alr_fr_header *g_db = NULL;       /* mmap base, or NULL if DB disabled */
static alr_fr_entry  *g_slots = NULL;
static uint64_t       g_nslots = 0;

/* futex lock on g_db->lock */
static void fr_lock(void){
    if(!g_db) return;
    for(;;){
        uint32_t expected = 0;
        if(__atomic_compare_exchange_n(&g_db->lock,&expected,1u,0,
               __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
        syscall(SYS_futex,&g_db->lock,FUTEX_WAIT,1,NULL,NULL,0);
    }
}
static void fr_unlock(void){
    if(!g_db) return;
    __atomic_store_n(&g_db->lock,0u,__ATOMIC_RELEASE);
    syscall(SYS_futex,&g_db->lock,FUTEX_WAKE,1,NULL,NULL,0);
}

/* Map (or create) the DB file named by ALR_FAKEROOT_DB. Best-effort: on any
 * failure g_db stays NULL and every DB op no-ops (credential spoof still works). */
static void fr_init_db(void){
    const char *path = getenv("ALR_FAKEROOT_DB");
    if(!path || !path[0]) return;
    uint64_t slots = 262144; /* default capacity */
    const char *s = getenv("ALR_FAKEROOT_DB_SLOTS");
    if(s && s[0]){ uint64_t v=0; for(const char*c=s;*c>='0'&&*c<='9';++c) v=v*10+(*c-'0'); if(v>=1024) slots=v; }
    /* round to power of two */
    uint64_t p2=1; while(p2<slots) p2<<=1; slots=p2;

    size_t total = ALR_FR_SLOTS_OFF + slots*sizeof(alr_fr_entry);
    int fd = open(path, O_RDWR|O_CREAT|O_CLOEXEC, 0600);
    if(fd<0) return;
    /* size it (first creator wins; ftruncate is idempotent) */
    if(ftruncate(fd,(off_t)total)!=0){ close(fd); return; }
    void *m = mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    close(fd);
    if(m==MAP_FAILED) return;
    alr_fr_header *h = (alr_fr_header*)m;
    /* initialize header once (CAS magic). A racing creator just re-stores the
     * same constants; n_slots is fixed by the file size. */
    uint64_t expect=0;
    if(__atomic_compare_exchange_n(&h->magic,&expect,ALR_FR_MAGIC,0,
           __ATOMIC_ACQ_REL,__ATOMIC_RELAXED)){
        h->version=ALR_FR_VERSION; h->n_slots=slots; h->lock=0; h->n_used=0;
    }
    if(h->magic!=ALR_FR_MAGIC || h->n_slots==0){ munmap(m,total); return; }
    g_db=h; g_slots=(alr_fr_entry*)((char*)m+ALR_FR_SLOTS_OFF); g_nslots=h->n_slots;
}

/* find slot for (dev,ino); if create, returns an EMPTY slot to fill (caller
 * holds the lock). Returns NULL on table-full (fail-safe: no override). */
static alr_fr_entry *fr_find(uint64_t dev, uint64_t ino, int create){
    if(!g_slots || ino==0) return NULL;
    uint64_t mask=g_nslots-1;
    uint64_t i=alr_fr_hash(dev,ino)&mask;
    for(uint64_t n=0;n<g_nslots;++n){
        alr_fr_entry *e=&g_slots[(i+n)&mask];
        uint64_t k=__atomic_load_n(&e->key_ino,__ATOMIC_ACQUIRE);
        if(k==ino && e->key_dev==dev) return e;
        if(k==0){
            if(!create) return NULL;
            e->key_dev=dev; /* fill value first... */
            return e;       /* ...caller sets fields then publishes key_ino */
        }
    }
    return NULL; /* full */
}

/* record a uid/gid (chown). real chown already attempted/no-op'd. */
static void fr_put_owner(uint64_t dev, uint64_t ino, uint32_t uid, uint32_t gid){
    if(!g_db) return;
    fr_lock();
    alr_fr_entry *e=fr_find(dev,ino,1);
    if(e){
        e->f_uid=uid; e->f_gid=gid; e->f_flags|=ALR_FR_F_HAVE_UIDGID;
        __atomic_store_n(&e->key_ino,ino,__ATOMIC_RELEASE); /* publish */
    }
    fr_unlock();
}
static void fr_put_mode(uint64_t dev, uint64_t ino, uint32_t mode){
    if(!g_db) return;
    fr_lock();
    alr_fr_entry *e=fr_find(dev,ino,1);
    if(e){
        e->f_mode=mode; e->f_flags|=ALR_FR_F_HAVE_MODE;
        __atomic_store_n(&e->key_ino,ino,__ATOMIC_RELEASE);
    }
    fr_unlock();
}
static void fr_put_node(uint64_t dev, uint64_t ino, uint32_t mode, uint64_t rdev){
    if(!g_db) return;
    fr_lock();
    alr_fr_entry *e=fr_find(dev,ino,1);
    if(e){
        e->f_mode=mode; e->f_rdev_lo=(uint32_t)rdev; e->f_rdev_hi=(uint32_t)(rdev>>32);
        e->f_flags|=ALR_FR_F_HAVE_MODE|ALR_FR_F_HAVE_RDEV;
        __atomic_store_n(&e->key_ino,ino,__ATOMIC_RELEASE);
    }
    fr_unlock();
}

/* overlay DB metadata onto a filled struct stat (lock-free read). */
static void fr_overlay_stat(struct stat *st){
    if(!g_db || !st) return;
    alr_fr_entry *e=fr_find((uint64_t)st->st_dev,(uint64_t)st->st_ino,0);
    if(!e) return;
    uint32_t fl=e->f_flags;
    if(fl & ALR_FR_F_HAVE_UIDGID){ st->st_uid=e->f_uid; st->st_gid=e->f_gid; }
    if(fl & ALR_FR_F_HAVE_MODE){ st->st_mode=e->f_mode; }
    if(fl & ALR_FR_F_HAVE_RDEV){
        st->st_rdev=((uint64_t)e->f_rdev_hi<<32)|e->f_rdev_lo;
    }
}

/* overlay DB metadata onto a filled struct statx (the modern stat ABI that
 * dpkg-deb/coreutils use on noble). statx keys off (stx_dev_major:minor,
 * stx_ino) and splits rdev into major/minor — so we recompose st_dev/st_rdev
 * the same way the kernel's makedev does (glibc gnu_dev_make/major/minor). */
static uint64_t fr_makedev(uint32_t maj, uint32_t min){
    return ((uint64_t)(min & 0xffu))
         | (((uint64_t)(maj & 0xfffu)) << 8)
         | (((uint64_t)(min & ~0xffu)) << 12)
         | (((uint64_t)(maj & ~0xfffu)) << 32);
}
static void fr_overlay_statx(struct statx *sx){
    if(!g_db || !sx) return;
    uint64_t dev = fr_makedev(sx->stx_dev_major, sx->stx_dev_minor);
    alr_fr_entry *e=fr_find(dev,(uint64_t)sx->stx_ino,0);
    if(!e) return;
    uint32_t fl=e->f_flags;
    if(fl & ALR_FR_F_HAVE_UIDGID){ sx->stx_uid=e->f_uid; sx->stx_gid=e->f_gid; }
    if(fl & ALR_FR_F_HAVE_MODE){ sx->stx_mode=(uint16_t)e->f_mode; }
    if(fl & ALR_FR_F_HAVE_RDEV){
        uint64_t rdev=((uint64_t)e->f_rdev_hi<<32)|e->f_rdev_lo;
        /* split rdev back into major/minor (inverse of fr_makedev) */
        sx->stx_rdev_major=(uint32_t)(((rdev>>8)&0xfffu)|((rdev>>32)&~0xfffu));
        sx->stx_rdev_minor=(uint32_t)((rdev&0xffu)|((rdev>>12)&~0xffu));
    }
}

/* ============================ RTLD_NEXT cache ============================== */
#define FR_REAL(slot,type,sym) do{ if(!(slot)) (slot)=(type)dlsym(RTLD_NEXT,sym); }while(0)

/* ============================ credentials => root ========================= */
/* The whole point: dpkg's "requires superuser privilege" check reads geteuid()
 * and must see 0. fork()ed maintainer scripts inherit the same .so, so they too
 * see 0 — no shared state needed (these are pure constants). */
uid_t getuid(void){ return 0; }
uid_t geteuid(void){ return 0; }
gid_t getgid(void){ return 0; }
gid_t getegid(void){ return 0; }

int getresuid(uid_t *r,uid_t *e,uid_t *s){ if(r)*r=0; if(e)*e=0; if(s)*s=0; return 0; }
int getresgid(gid_t *r,gid_t *e,gid_t *s){ if(r)*r=0; if(e)*e=0; if(s)*s=0; return 0; }

/* setuid/setgid family: dpkg/maintainer scripts may setuid(0) (a no-op for real
 * root). We report success without a real syscall (which would EPERM for us). */
int setuid(uid_t u){ (void)u; return 0; }
int seteuid(uid_t u){ (void)u; return 0; }
int setgid(gid_t g){ (void)g; return 0; }
int setegid(gid_t g){ (void)g; return 0; }
int setreuid(uid_t r,uid_t e){ (void)r;(void)e; return 0; }
int setregid(gid_t r,gid_t e){ (void)r;(void)e; return 0; }

/* getgroups: report just the root group (0). dpkg uses this only cosmetically. */
int getgroups(int size, gid_t list[]){
    if(size==0) return 1;
    if(size<1){ errno=EINVAL; return -1; }
    if(list) list[0]=0;
    return 1;
}

/* ============================ chown family (record-only) ================== */
/* We do NOT issue the real chown (it EPERMs for a non-root owner trying to set
 * a different uid). We stat the (rewritten) path to key the DB by (dev,ino),
 * record the requested uid/gid, and report success. */
static void fr_chown_record(const char *hostpath, int dirfd, int at_flags,
                            uint32_t uid, uint32_t gid){
    struct stat st;
    static int (*real_fstatat)(int,const char*,struct stat*,int);
    FR_REAL(real_fstatat,int(*)(int,const char*,struct stat*,int),"fstatat");
    if(real_fstatat && real_fstatat(dirfd,hostpath,&st,at_flags)==0)
        fr_put_owner((uint64_t)st.st_dev,(uint64_t)st.st_ino,uid,gid);
}
int chown(const char *path, uid_t uid, gid_t gid){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF]; const char *p=fr_rw(path,b,sizeof b);
    fr_chown_record(p,AT_FDCWD,0,uid,gid);
    return 0;
}
int lchown(const char *path, uid_t uid, gid_t gid){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF]; const char *p=fr_rw(path,b,sizeof b);
    fr_chown_record(p,AT_FDCWD,AT_SYMLINK_NOFOLLOW,uid,gid);
    return 0;
}
int fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF];
    const char *p=(path&&path[0]=='/')?fr_rw(path,b,sizeof b):path;
    fr_chown_record(p,dirfd,flags,uid,gid);
    return 0;
}
int fchown(int fd, uid_t uid, gid_t gid){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    struct stat st;
    static int (*real_fstat)(int,struct stat*);
    FR_REAL(real_fstat,int(*)(int,struct stat*),"fstat");
    if(real_fstat && real_fstat(fd,&st)==0)
        fr_put_owner((uint64_t)st.st_dev,(uint64_t)st.st_ino,uid,gid);
    return 0;
}

/* ============================ chmod family (apply + record) =============== */
/* chmod on a file the guest owns usually SUCCEEDS for the perm bits; but the
 * kernel strips setuid/setgid when a non-root owner sets them. We attempt the
 * real chmod (best effort) and always record the FULL requested mode so stat
 * reflects exactly what dpkg asked for (incl. su/sg/sticky). */
int chmod(const char *path, mode_t mode){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF]; const char *p=fr_rw(path,b,sizeof b);
    struct stat st;
    static int (*real_chmod)(const char*,mode_t);
    static int (*real_stat)(const char*,struct stat*);
    FR_REAL(real_chmod,int(*)(const char*,mode_t),"chmod");
    FR_REAL(real_stat,int(*)(const char*,struct stat*),"stat");
    if(real_chmod) real_chmod(p,mode);            /* best effort, ignore EPERM */
    if(real_stat && real_stat(p,&st)==0){
        uint32_t full=(uint32_t)((st.st_mode & S_IFMT) | (mode & 07777));
        fr_put_mode((uint64_t)st.st_dev,(uint64_t)st.st_ino,full);
    }
    return 0;
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF];
    const char *p=(path&&path[0]=='/')?fr_rw(path,b,sizeof b):path;
    struct stat st;
    static int (*real_fchmodat)(int,const char*,mode_t,int);
    static int (*real_fstatat)(int,const char*,struct stat*,int);
    FR_REAL(real_fchmodat,int(*)(int,const char*,mode_t,int),"fchmodat");
    FR_REAL(real_fstatat,int(*)(int,const char*,struct stat*,int),"fstatat");
    if(real_fchmodat) real_fchmodat(dirfd,p,mode,flags);
    if(real_fstatat && real_fstatat(dirfd,p,&st,flags)==0){
        uint32_t full=(uint32_t)((st.st_mode & S_IFMT) | (mode & 07777));
        fr_put_mode((uint64_t)st.st_dev,(uint64_t)st.st_ino,full);
    }
    return 0;
}
int fchmod(int fd, mode_t mode){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    struct stat st;
    static int (*real_fchmod)(int,mode_t);
    static int (*real_fstat)(int,struct stat*);
    FR_REAL(real_fchmod,int(*)(int,mode_t),"fchmod");
    FR_REAL(real_fstat,int(*)(int,struct stat*),"fstat");
    if(real_fchmod) real_fchmod(fd,mode);
    if(real_fstat && real_fstat(fd,&st)==0){
        uint32_t full=(uint32_t)((st.st_mode & S_IFMT) | (mode & 07777));
        fr_put_mode((uint64_t)st.st_dev,(uint64_t)st.st_ino,full);
    }
    return 0;
}

/* ============================ mknod family (fake special files) ========== */
/* For regular/FIFO/socket nodes the kernel lets a non-root app create them, so
 * we forward (after path rewrite). For char/block specials the kernel EPERMs;
 * we create a 0-length placeholder regular file and record (mode,rdev) so stat
 * reports the device dpkg expects. */
static void fr_mknod_emul(const char *hostpath, int dirfd, mode_t mode, dev_t dev){
    static int (*real_mknodat)(int,const char*,mode_t,dev_t);
    static int (*real_openat)(int,const char*,int,...);
    static int (*real_fstatat)(int,const char*,struct stat*,int);
    FR_REAL(real_mknodat,int(*)(int,const char*,mode_t,dev_t),"mknodat");
    FR_REAL(real_openat,int(*)(int,const char*,int,...),"openat");
    FR_REAL(real_fstatat,int(*)(int,const char*,struct stat*,int),"fstatat");
    int is_special = S_ISCHR(mode)||S_ISBLK(mode);
    if(!is_special){
        if(real_mknodat) real_mknodat(dirfd,hostpath,mode,dev);
    } else if(real_openat){
        int fd=real_openat(dirfd,hostpath,O_CREAT|O_WRONLY|O_CLOEXEC,0600);
        if(fd>=0){ static int (*real_close)(int); FR_REAL(real_close,int(*)(int),"close"); if(real_close) real_close(fd); }
    }
    struct stat st;
    if(real_fstatat && real_fstatat(dirfd,hostpath,&st,0)==0)
        fr_put_node((uint64_t)st.st_dev,(uint64_t)st.st_ino,(uint32_t)mode,(uint64_t)dev);
}
int mknod(const char *path, mode_t mode, dev_t dev){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF]; const char *p=fr_rw(path,b,sizeof b);
    fr_mknod_emul(p,AT_FDCWD,mode,dev);
    return 0;
}
int mknodat(int dirfd, const char *path, mode_t mode, dev_t dev){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    char b[FR_PBUF];
    const char *p=(path&&path[0]=='/')?fr_rw(path,b,sizeof b):path;
    fr_mknod_emul(p,dirfd,mode,dev);
    return 0;
}

/* ============================ stat overlay =============================== *
 * fakeroot is listed FIRST in LD_PRELOAD so these win the symbol. Inside, we do
 * NOT rewrite the path; we call RTLD_NEXT, which resolves to the INTERPOSER's
 * same-named wrapper (next in search order) — so the interposer does the rootfs
 * rewrite + trampoline emit. On return we overlay DB metadata. If the interposer
 * is absent, RTLD_NEXT lands on libc and we overlay without rootfs rewrite.
 * Only the `struct stat`/`stat64` forms carry st_uid/gid/mode/rdev, so those are
 * the only ones we overlay; statx is overlaid via its own fields. */
#define FR_WRAP_STAT_PATH(name, sname)                                       \
    int name(const char *path, struct stat *buf){                            \
        if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }         \
        static int (*real)(const char*,struct stat*);                        \
        FR_REAL(real,int(*)(const char*,struct stat*),sname);                \
        int r = real ? real(path,buf) : -1;                                  \
        if(r==0) fr_overlay_stat(buf);                                       \
        return r;                                                            \
    }
FR_WRAP_STAT_PATH(stat,  "stat")
FR_WRAP_STAT_PATH(lstat, "lstat")

int fstat(int fd, struct stat *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,struct stat*);
    FR_REAL(real,int(*)(int,struct stat*),"fstat");
    int r = real ? real(fd,buf) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}
int fstatat(int dirfd, const char *path, struct stat *buf, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,struct stat*,int);
    FR_REAL(real,int(*)(int,const char*,struct stat*,int),"fstatat");
    int r = real ? real(dirfd,path,buf,flags) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}
int newfstatat(int dirfd, const char *path, struct stat *buf, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,struct stat*,int);
    FR_REAL(real,int(*)(int,const char*,struct stat*,int),"newfstatat");
    int r = real ? real(dirfd,path,buf,flags) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}

/* statx — the modern entry coreutils/dpkg-deb bind to on noble. Same RTLD_NEXT
 * chain (interposer rewrites the path); we overlay the statx-shaped fields. */
int statx(int dirfd, const char *path, int flags, unsigned int mask,
          struct statx *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,int,unsigned int,struct statx*);
    FR_REAL(real,int(*)(int,const char*,int,unsigned int,struct statx*),"statx");
    int r = real ? real(dirfd,path,flags,mask,buf) : -1;
    if(r==0) fr_overlay_statx(buf);
    return r;
}

/* LFS *64 variants: glibc types these as `struct stat64`, which on aarch64 has
 * the SAME layout as `struct stat` (the arch is natively 64-bit off_t), so the
 * st_dev/st_ino/st_uid/st_gid/st_mode/st_rdev fields line up and fr_overlay_stat
 * applies verbatim via a cast. Some binaries bind the *64 symbol explicitly;
 * define them so we still win + overlay. */
static void fr_overlay_stat64(struct stat64 *st){
    fr_overlay_stat((struct stat*)st);
}
int stat64(const char *path, struct stat64 *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(const char*,struct stat64*);
    FR_REAL(real,int(*)(const char*,struct stat64*),"stat64");
    int r = real ? real(path,buf) : -1;
    if(r==0) fr_overlay_stat64(buf);
    return r;
}
int lstat64(const char *path, struct stat64 *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(const char*,struct stat64*);
    FR_REAL(real,int(*)(const char*,struct stat64*),"lstat64");
    int r = real ? real(path,buf) : -1;
    if(r==0) fr_overlay_stat64(buf);
    return r;
}
int fstat64(int fd, struct stat64 *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,struct stat64*);
    FR_REAL(real,int(*)(int,struct stat64*),"fstat64");
    int r = real ? real(fd,buf) : -1;
    if(r==0) fr_overlay_stat64(buf);
    return r;
}
int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,struct stat64*,int);
    FR_REAL(real,int(*)(int,const char*,struct stat64*,int),"fstatat64");
    int r = real ? real(dirfd,path,buf,flags) : -1;
    if(r==0) fr_overlay_stat64(buf);
    return r;
}

/* Legacy __xstat* compat (pre-2.33 glibc / old binaries that bind the versioned
 * stat shims instead of stat directly). The `ver` arg selects the struct ABI;
 * we forward it untouched and overlay the same struct stat. Noble's own binaries
 * bind modern stat, so these are a fallback for older guest binaries only. */
int __xstat(int ver, const char *path, struct stat *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,struct stat*);
    FR_REAL(real,int(*)(int,const char*,struct stat*),"__xstat");
    int r = real ? real(ver,path,buf) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}
int __lxstat(int ver, const char *path, struct stat *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,const char*,struct stat*);
    FR_REAL(real,int(*)(int,const char*,struct stat*),"__lxstat");
    int r = real ? real(ver,path,buf) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}
int __fxstat(int ver, int fd, struct stat *buf){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,int,struct stat*);
    FR_REAL(real,int(*)(int,int,struct stat*),"__fxstat");
    int r = real ? real(ver,fd,buf) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}
int __fxstatat(int ver, int dirfd, const char *path, struct stat *buf, int flags){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
    static int (*real)(int,int,const char*,struct stat*,int);
    FR_REAL(real,int(*)(int,int,const char*,struct stat*,int),"__fxstatat");
    int r = real ? real(ver,dirfd,path,buf,flags) : -1;
    if(r==0) fr_overlay_stat(buf);
    return r;
}

/* ============================ constructor ================================ */
/* Priority 100 sorts BEFORE the interposer's constructor(101) only if fakeroot
 * is listed first; ordering across .so's is by LD_PRELOAD order then priority.
 * The ctor just primes rootfs + DB so the first credential/stat call is ready;
 * everything degrades to no-op if env is unset (never aborts). */
__attribute__((constructor(100)))
static void fr_ctor(void){
    if(!g_inited){ fr_init_rootfs(); fr_init_db(); g_inited=1; }
}

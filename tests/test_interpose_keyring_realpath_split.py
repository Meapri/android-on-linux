"""Regression guard for the authenticated-apt keyring path-split fix.

Device bug (CR-2 authenticated apt): noble apt's gpgv method execs apt-key, which
mints a guest GPGHOMEDIR via `mktemp` (a "/tmp/apt-key-gpghome.XXX" guest path),
then `readlink -f`/realpath's it and builds its keyring + hands gpgv
`--homedir`/`--keyring` off that value. The ALR LD_PRELOAD interposer
(app/src/main/cpp/alr_interpose/libalr_interpose.c) resolves realpath against the
real host tree, so WITHOUT a fix the canonical comes back as the HOST spelling
"<rootfs>/tmp/apt-key-gpghome.XXX" — a path-split between the keyring apt-key
writes and the one gpgv opens -> gpgv NODATA -> "is not signed".

FIX: realpath()/canonicalize_file_name()/__realpath_chk() now run guest_canon() on
the result to strip the rootfs (or /data/data canon-alias) prefix back off, so the
canonical is the GUEST spelling and both sides AGREE. This must NOT regress the
dpkg-status fix (9aa644c / g_rootfs_canon): the resolve still succeeds and the
guest result re-opens through rw() to the same host file.

This test (1) asserts the source wiring stays present, and (2) compiles a tiny C
harness using the rw()/guest_canon()/alr_init() logic and asserts the actual
string behavior end-to-end (split is closed; dpkg-status re-open still maps back).
"""

import shutil
import subprocess
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
INTERPOSE = ROOT / "app/src/main/cpp/alr_interpose/libalr_interpose.c"


# --------------------------------------------------------------------------- #
# (1) Source-wiring guards — cheap, run everywhere.
# --------------------------------------------------------------------------- #

def test_guest_canon_helper_exists():
    text = INTERPOSE.read_text()
    assert "static void guest_canon(char *path)" in text, (
        "guest_canon() helper (the realpath-result rootfs-prefix strip) must exist"
    )
    # It must recognize BOTH the rootfs form and its /data/data canonical alias,
    # mirroring rw()'s idempotency test, or the per-user /data/data spelling that
    # realpath returns (after following /data/user/0->/data/data) won't be stripped.
    assert "a_under(path, g_rootfs)" in text
    assert "a_under(path, g_rootfs_canon)" in text


def test_all_three_realpath_wrappers_call_guest_canon():
    text = INTERPOSE.read_text()
    # Each of realpath / canonicalize_file_name / __realpath_chk must apply the
    # guest-consistency strip to a non-NULL result.
    for sym in ("char *realpath(",
                "char *canonicalize_file_name(",
                "char *__realpath_chk("):
        idx = text.find(sym)
        assert idx != -1, f"missing wrapper {sym!r}"
        body = text[idx:idx + 700]
        assert "guest_canon(" in body, (
            f"{sym!r} must call guest_canon() on its result (guest-consistent canonical)"
        )


def test_plain_readlink_is_not_guest_canon_stripped():
    """A bare readlink/readlinkat returns the symlink's stored target verbatim
    (guest/relative) — it must NOT be guest_canon()'d (only full canonicalization
    via realpath produces a host path). Guard that the strip stays realpath-only."""
    text = INTERPOSE.read_text()
    idx = text.find("alr_readlink_emit(")
    assert idx != -1
    # the emit body (up to the next wrapper) must not call guest_canon
    body = text[idx:text.find("ssize_t readlink(", idx)]
    assert "guest_canon(" not in body, (
        "plain readlink path must not strip — only realpath-family canonicalizes to host"
    )


# --------------------------------------------------------------------------- #
# (2) Behavioral guard — compile + run the actual rw()/guest_canon() logic.
# --------------------------------------------------------------------------- #

_HARNESS = r"""
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

static size_t a_len(const char *s){size_t n=0;if(s){while(s[n])++n;}return n;}
static int a_under(const char *p,const char *d){size_t i=0;while(d[i]){if(p[i]!=d[i])return 0;++i;}return p[i]=='/'||p[i]=='\0';}
static char g_rootfs[1024];static size_t g_rootfs_len=0;
static char g_rootfs_canon[1024];static size_t g_rootfs_canon_len=0;
static int g_inited=0;static __thread int g_rw_suppress=0;
static void alr_init(void){
  if(g_inited)return;g_inited=1;const char*r=getenv("ALR_ROOTFS");
  if(r&&r[0]=='/'){size_t n=a_len(r);while(n>1&&r[n-1]=='/')--n;if(n>=sizeof(g_rootfs))n=sizeof(g_rootfs)-1;
    for(size_t i=0;i<n;++i)g_rootfs[i]=r[i];g_rootfs[n]='\0';g_rootfs_len=(n==1&&g_rootfs[0]=='/')?0:n;g_rootfs_canon_len=0;
    {static const char pu_s[]="/data/user/0/",ca_s[]="/data/data/";const size_t pu=sizeof(pu_s)-1,ca=sizeof(ca_s)-1;int m=(g_rootfs_len>pu);
     for(size_t i=0;m&&i<pu;++i)if(g_rootfs[i]!=pu_s[i])m=0;
     if(m){const size_t rest=g_rootfs_len-pu;if(ca+rest<sizeof(g_rootfs_canon)){size_t i=0;for(;i<ca;++i)g_rootfs_canon[i]=ca_s[i];
       for(size_t j=0;j<rest;++j)g_rootfs_canon[ca+j]=g_rootfs[pu+j];g_rootfs_canon[ca+rest]='\0';g_rootfs_canon_len=ca+rest;}}}
  }else{g_rootfs_len=0;g_rootfs_canon_len=0;}
}
static const char *rw(const char *p,char *buf,size_t buflen){
  if(!p||p[0]!='/')return p;if(g_rw_suppress)return p;if(!g_inited)alr_init();if(g_rootfs_len==0)return p;
  if(a_under(p,"/proc")||a_under(p,"/sys")||a_under(p,"/dev"))return p;
  if(a_under(p,g_rootfs))return p;if(g_rootfs_canon_len&&a_under(p,g_rootfs_canon))return p;
  size_t plen=a_len(p);if(g_rootfs_len+plen+1>buflen)return p;size_t i=0;for(;i<g_rootfs_len;++i)buf[i]=g_rootfs[i];
  for(size_t j=0;j<=plen;++j)buf[i+j]=p[j];return buf;
}
#define ALR_PBUF 2304
static void guest_canon(char *path){
  if(!path||path[0]!='/')return;const char*pfx=NULL;size_t plen=0;
  if(g_rootfs_len&&a_under(path,g_rootfs)){pfx=g_rootfs;plen=g_rootfs_len;}
  else if(g_rootfs_canon_len&&a_under(path,g_rootfs_canon)){pfx=g_rootfs_canon;plen=g_rootfs_canon_len;}
  if(!pfx)return;size_t i=0;while(path[plen+i]){path[i]=path[plen+i];++i;}path[i]='\0';
  if(path[0]=='\0'){path[0]='/';path[1]='\0';}
}
static char *alr_realpath(const char *path,char *resolved){
  char b[ALR_PBUF];const char*in=rw(path,b,sizeof b);g_rw_suppress++;char*res=realpath(in,resolved);g_rw_suppress--;
  if(res)guest_canon(res);return res;
}
static int fail=0;
static void ck(const char*n,int ok){printf("%s %s\n",ok?"PASS":"FAIL",n);if(!ok)fail=1;}
int main(void){
  /* device-faithful: ALR_ROOTFS is already-canonical (the only device symlink,
   * /data/user/0->/data/data, is modeled by g_rootfs_canon; no /private indirection). */
  const char *root=getenv("ALR_ROOTFS");
  alr_init();
  /* (A) keyring split closed: guest GPGHOMEDIR realpath -> SAME guest string */
  char a[PATH_MAX];char *ra=alr_realpath("/tmp/apt-key-gpghome.AbCdEf1234",a);
  ck("keyring-realpath-succeeds",ra!=NULL);
  ck("keyring-no-split",ra&&strcmp(ra,"/tmp/apt-key-gpghome.AbCdEf1234")==0);
  /* (B) dpkg-status no-regression: host-pinned status realpath succeeds, returns
   *     the guest path, and re-opens (rw) back to the host file. */
  char hs[PATH_MAX];snprintf(hs,sizeof hs,"%s/var/lib/dpkg/status",root);
  char b2[PATH_MAX];char *rb=alr_realpath(hs,b2);
  ck("dpkgstatus-realpath-succeeds",rb!=NULL);
  ck("dpkgstatus-returns-guest-path",rb&&strcmp(rb,"/var/lib/dpkg/status")==0);
  char ob[ALR_PBUF];const char*re=rb?rw(rb,ob,sizeof ob):NULL;
  char want[PATH_MAX];snprintf(want,sizeof want,"%s/var/lib/dpkg/status",root);
  ck("dpkgstatus-reopen-maps-back-to-host",re&&strcmp(re,want)==0);
  /* (C) outside-rootfs / proc are never stripped */
  char pp[64]="/proc/self/exe";guest_canon(pp);ck("proc-unstripped",strcmp(pp,"/proc/self/exe")==0);
  return fail;
}
"""


@pytest.mark.skipif(shutil.which("cc") is None and shutil.which("clang") is None,
                    reason="no C compiler")
def test_guest_canon_closes_split_without_regressing_dpkg_status(tmp_path):
    cc = shutil.which("cc") or shutil.which("clang")
    src = tmp_path / "h.c"
    src.write_text(textwrap.dedent(_HARNESS))
    exe = tmp_path / "h"
    subprocess.run([cc, "-O2", "-o", str(exe), str(src)], check=True)

    # device-faithful rootfs: a CANONICAL base (resolve any /tmp->/private symlink
    # the host may have, so the result is under g_rootfs exactly as on device).
    root = (tmp_path / "root").resolve()
    (root / "tmp" / "apt-key-gpghome.AbCdEf1234").mkdir(parents=True)
    (root / "tmp" / "apt-key-gpghome.AbCdEf1234" / "pubring.gpg").touch()
    (root / "var" / "lib" / "dpkg").mkdir(parents=True)
    (root / "var" / "lib" / "dpkg" / "status").touch()

    env = {"ALR_ROOTFS": str(root)}
    out = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    assert out.returncode == 0, (
        "guest_canon behavioral check failed:\n" + out.stdout + out.stderr
    )
    # explicit: the split must be closed AND dpkg-status must still map back.
    assert "PASS keyring-no-split" in out.stdout
    assert "PASS dpkgstatus-returns-guest-path" in out.stdout
    assert "PASS dpkgstatus-reopen-maps-back-to-host" in out.stdout

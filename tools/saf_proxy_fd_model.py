"""SAF 직통 프록시 v2 — fd-주입 강등 정책 순수 모델.

`tools/saf_bridge_model.py` 의 `decide()` 는 "이 게스트 openat 트랩이 어느
decision(FALLTHROUGH / REWRITE_PATH / SUBSTITUTE_FD / DENY)인가"를 _경로·모드만_
으로 고른다. 그러나 `SUBSTITUTE_FD`(직통 프록시) 가 실제로 가능한지는 런타임에서
SAF 가 연 `ParcelFileDescriptor`(PFD) 의 _실제 능력_ 에 종속된다:

  - PFD 가 **pipe-backed**(네트워크/클라우드 DocumentsProvider) 면 lseek/pread/
    pwrite/mmap 이 ESPIPE/ENODEV 로 깨진다 → 라이브 fd 치환 불가.
  - 게스트가 그 fd 로 **seek/mmap 을 요구**하면(랜덤 액세스·sqlite mmap I/O·
    실행 매핑) pipe fd 는 못 버틴다.
  - **디렉터리 open** 은 SAF 에 "디렉터리 fd" 개념이 없어 fd 치환 자체가 불가 →
    `listFiles()` 합성(getdents) 경로로 분기.

이 모듈은 그 **2차 결정(강등)** 의 순수 로직만 담는다 — `decide()` 가 1차로
`SUBSTITUTE_FD` 를 낸 뒤, 런타임이 PFD 를 열어보고 그 능력(seekable/mmappable/
pipe 여부, 디렉터리 여부)을 모델에 먹이면, 이 모델이 라이브 fd / memfd 복제 /
copy 폴백 / getdents 합성 / deny 중 무엇으로 갈지 결정한다.

Android API 호출 0 — PFD 를 실제로 열지도, 복사하지도, 주입하지도 않는다.
"어떤 능력 프로파일 + 어떤 접근 의도 → 어떤 강등 액션 + 어떤 fd-주입 요건" 만
고정한다. 실 fd/PFD/SCM_RIGHTS 트램펄린은 런타임(supervisor) 책임이며 device
에서만 검증된다(saf-proxy.md §6 게이트).

설계 문서: docs/design/saf-proxy.md (§3-B/§3-C/§3-D/§4/§5-F).
선행 모델: tools/saf_bridge_model.py (`decide` 1차 분기).
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional

from saf_bridge_model import (
    BridgeDecision,
    ProxyTarget,
    SafBridge,
    SafResolution,
    decide,
)


# --- errno 상수(런타임이 게스트에 반환) ---------------------------------------
EACCES = 13   # 정책 위반(escape/unknown label)
EROFS = 30    # read-only 마운트에 쓰기
ENOTDIR = 20  # 파일을 디렉터리로 / 디렉터리를 파일로 열려 함
EISDIR = 21   # 디렉터리를 일반 파일 open 의도로 열려 함


class FdBacking(Enum):
    """SAF `ParcelFileDescriptor` 의 실제 backing(런타임이 PFD 를 열어 관찰).

      REGULAR_FILE = 로컬 DocumentsProvider(externalstorage) 가 연 실파일 fd.
                     seekable + mmappable(대부분). 라이브 fd 치환 최적.
      SEEKABLE     = seekable 이지만 mmap 보장 안 됨(일부 provider). pread/pwrite OK.
      PIPE         = 네트워크/클라우드 provider 의 pipe-backed fd. lseek=ESPIPE,
                     mmap 불가. 순차 read/write 만.
      UNKNOWN      = 아직 관찰 못 함 / 판별 불가(보수적으로 PIPE 취급).
    """

    REGULAR_FILE = "regular_file"
    SEEKABLE = "seekable"
    PIPE = "pipe"
    UNKNOWN = "unknown"

    @property
    def is_seekable(self) -> bool:
        return self in (FdBacking.REGULAR_FILE, FdBacking.SEEKABLE)

    @property
    def is_mmappable(self) -> bool:
        # 실파일만 안전하게 mmap 가능(write-back 의미 포함은 §3-D 한계).
        return self is FdBacking.REGULAR_FILE


class GuestAccess(Enum):
    """게스트가 그 fd 로 하려는 접근 패턴(런타임이 openat flags + 후속 syscall
    힌트로 추정). 강등 판정의 입력.

      SEQUENTIAL = 순차 read/write 만(스트리밍). pipe fd 로도 충분.
      RANDOM     = lseek/pread/pwrite 임의접근. seekable fd 필요.
      MMAP       = mmap 매핑(파일 매핑 I/O / 실행). 실파일 fd 필요.
    """

    SEQUENTIAL = "sequential"
    RANDOM = "random"
    MMAP = "mmap"


class ProxyAction(Enum):
    """fd-주입 2차 강등 결정.

      SUBSTITUTE_LIVE_FD = SAF 라이브 fd 를 그대로 게스트에 SCM_RIGHTS 주입(직통).
      MEMFD_CLONE        = PFD 내용을 memfd 로 복제해 주입(seek/mmap 가능한 익명
                           fd). close 시 write-back(rw). "프록시 외형 + copy 일관성".
      COPY_FALLBACK      = §saf_bridge_model REWRITE_PATH 로 강등(rootfs 임시파일).
                           대용량/라이브성 포기, 최대 호환.
      GETDENTS_SYNTH     = 디렉터리: listFiles() 결과를 getdents64 버퍼로 합성
                           (fd 치환 아님 — 가상 엔트리).
      DENY               = 정책/구조 위반(ro 쓰기, dir↔file 불일치 등).
    """

    SUBSTITUTE_LIVE_FD = "substitute_live_fd"
    MEMFD_CLONE = "memfd_clone"
    COPY_FALLBACK = "copy_fallback"
    GETDENTS_SYNTH = "getdents_synth"
    DENY = "deny"


@dataclass(frozen=True)
class FdInjection:
    """SUBSTITUTE_LIVE_FD / MEMFD_CLONE 시 supervisor 에 요구하는 fd-주입 요건
    (saf-proxy.md §3-C / §5-F kSubstituteFd 계약의 host 절반).

    런타임(supervisor)이 이 값으로 트랩 핸들러에서:
      1. (live) SAF PFD.getFd() / (memfd) memfd_create+복제 로 host_fd 확보,
      2. 부팅 시 상속한 socketpair 로 `sendmsg(SCM_RIGHTS=[host_fd])`,
      3. 게스트를 대신해 트램펄린 `recvmsg` → 게스트 fd 테이블에 새 fd 생성,
      4. openat 트랩의 syscall-exit `regs[0]` 를 그 새 fd 번호로 치환.

    이 모델은 _요건_(어떤 source, ro 여부, write-back 필요 여부)만 고정한다 —
    실제 fd 번호/SCM_RIGHTS/single-step 은 런타임이 채우고 device 로 검증한다.
    """

    # fd 의 출처: "saf_pfd"(라이브 PFD) 또는 "memfd"(복제 익명 fd).
    source: str
    read_only: bool          # ro 마운트면 PFD 를 "r" 로 연다.
    # memfd 복제이고 쓰기 의도면 close 에서 PFD 로 write-back 필요.
    writeback_on_close: bool
    # fd 가 seekable 여야 게스트 의도를 만족하나(런타임이 보장 점검에 사용).
    requires_seek: bool
    requires_mmap: bool


@dataclass(frozen=True)
class DirSynth:
    """GETDENTS_SYNTH 시 supervisor 에 요구하는 디렉터리 합성 요건.

    런타임이 `DocumentFile.fromTreeUri(tree_uri)` 에서 `rel_path` 를 따라간 뒤
    `listFiles()` 로 자식 엔트리를 얻어 게스트 getdents64 버퍼로 합성한다.
    fd 치환이 아니라 _가상 디렉터리 스트림_ — fd 는 게스트가 연 그대로 둔다
    (openat 은 통과시키되 getdents 트랩을 가로채는 별도 경로).
    """

    tree_uri: str
    label: str
    mount_prefix: str
    rel_path: str
    read_only: bool


@dataclass(frozen=True)
class ProxyFdResolution:
    """`decide_proxy()` 결과 = 2차 강등 액션 + 모드별 요건.

      SUBSTITUTE_LIVE_FD / MEMFD_CLONE : injection 채움.
      GETDENTS_SYNTH                   : dir_synth 채움.
      COPY_FALLBACK                    : 둘 다 None — saf_bridge REWRITE_PATH 경로.
      DENY                             : deny_errno.

    `degraded_from_live` = 게스트가 직통(라이브 fd)을 희망했으나 PFD 능력/접근
    의도 때문에 memfd/copy 로 떨어진 경우 True(런타임 로깅·effective_mode 기록용).
    """

    action: ProxyAction
    injection: Optional[FdInjection] = None
    dir_synth: Optional[DirSynth] = None
    deny_errno: Optional[int] = None
    degraded_from_live: bool = False
    reason: str = ""


def decide_proxy(
    bridge: SafBridge,
    guest_path: str,
    *,
    write: bool = False,
    create: bool = False,
    is_dir: bool = False,
    fd_backing: FdBacking = FdBacking.UNKNOWN,
    access: GuestAccess = GuestAccess.SEQUENTIAL,
) -> ProxyFdResolution:
    """게스트 openat(프록시 마운트) + PFD 실제 능력 → fd-주입 2차 강등 결정.

    1차로 `saf_bridge_model.decide()` 를 호출해 경로/모드 분기를 그대로 재사용한다
    (escape/ro/unknown-label 정책이 _여기서도_ 모드보다 먼저 강제됨 — 불변식 3).
    그 1차가 `SUBSTITUTE_FD` 가 _아니면_ 이 함수는 강등 판정 없이 그 결과를 그대로
    전달한다:
      FALLTHROUGH  -> COPY_FALLBACK? 아님. FALLTHROUGH 는 SAF 밖이므로 여기선
                      애초에 호출 대상이 아니지만, 방어적으로 COPY_FALLBACK 으로
                      넘기지 않고 DENY 도 아닌 — 호출자가 프록시 마운트에만 부르는
                      계약이라 FALLTHROUGH 는 "프록시 비대상"으로 COPY_FALLBACK
                      이 아니라 그대로 전달(reason 에 표시).
      REWRITE_PATH -> COPY_FALLBACK(이미 copy 마운트).
      DENY         -> DENY(같은 errno).
    1차가 `SUBSTITUTE_FD` 면 비로소 PFD 능력 + 접근 의도 + 디렉터리 여부로 강등:

      디렉터리 open                    -> GETDENTS_SYNTH (fd 치환 불가)
      파일인데 게스트가 디렉터리로 기대 / 반대  -> (런타임 ENOTDIR/EISDIR; 여기선
                                          is_dir 한 축만 받으므로 dir↔file 충돌은
                                          런타임 책임 — 모델은 is_dir 로만 분기)
      seekable + (mmap 불요 or 실파일)  -> SUBSTITUTE_LIVE_FD (직통)
      pipe + 순차접근만                 -> SUBSTITUTE_LIVE_FD (순차는 pipe 로 충분)
      pipe + (seek/mmap 요구)           -> MEMFD_CLONE (강등)
      seekable이나 mmap요구+mmap불가     -> MEMFD_CLONE (강등)

    memfd 도 라이브성(즉시 양방향)을 일부 잃으므로, write-back 으로 일관성 보강.
    memfd 마저 부적합한 초대용량은 호출자가 COPY_FALLBACK 을 강제할 수 있게
    `force_copy` 는 두지 않고(정책 단순화) memfd 를 기본 강등으로 둔다 — copy
    폴백은 1차가 REWRITE_PATH(copy 마운트)일 때의 경로다.
    """
    first: SafResolution = decide(bridge, guest_path, write=write, create=create)

    if first.decision is BridgeDecision.DENY:
        return ProxyFdResolution(
            ProxyAction.DENY,
            deny_errno=first.deny_errno,
            reason="bridge policy deny (ro/escape/unknown-label)",
        )
    if first.decision is BridgeDecision.FALLTHROUGH:
        # 프록시 마운트에만 부르는 계약 위반(또는 traversal 로 SAF 밖). fd-주입
        # 비대상 — copy 로 강등하지 않고 그대로 전달(런타임이 rootfs 로 폴백).
        return ProxyFdResolution(
            ProxyAction.COPY_FALLBACK,
            reason="not a SAF proxy path (fallthrough) — runtime rootfs mediation",
        )
    if first.decision is BridgeDecision.REWRITE_PATH:
        # 1차에서 이미 copy 마운트로 분류 — 2차 강등 없이 copy.
        return ProxyFdResolution(
            ProxyAction.COPY_FALLBACK,
            reason="copy-mode mount (rewrite_path)",
        )

    # 여기 도달 = 1차 SUBSTITUTE_FD. proxy_target 보장.
    target: ProxyTarget = first.proxy_target  # type: ignore[assignment]
    assert target is not None

    # (1) 디렉터리 open: fd 치환 불가 → listFiles() getdents 합성.
    if is_dir:
        return ProxyFdResolution(
            ProxyAction.GETDENTS_SYNTH,
            dir_synth=DirSynth(
                tree_uri=target.tree_uri,
                label=target.label,
                mount_prefix=target.mount_prefix,
                rel_path=target.rel_path,
                read_only=target.read_only,
            ),
            reason="directory open — synthesize getdents from DocumentFile.listFiles()",
        )

    mutates = write or create
    needs_seek = access in (GuestAccess.RANDOM, GuestAccess.MMAP)
    needs_mmap = access is GuestAccess.MMAP

    # (2) 라이브 fd 치환이 게스트 의도를 만족하나?
    seek_ok = (not needs_seek) or fd_backing.is_seekable
    mmap_ok = (not needs_mmap) or fd_backing.is_mmappable
    live_ok = seek_ok and mmap_ok

    if live_ok:
        return ProxyFdResolution(
            ProxyAction.SUBSTITUTE_LIVE_FD,
            injection=FdInjection(
                source="saf_pfd",
                read_only=target.read_only,
                writeback_on_close=False,  # 라이브 fd 는 즉시 반영 — 별도 write-back 0.
                requires_seek=needs_seek,
                requires_mmap=needs_mmap,
            ),
            reason=f"live fd satisfies access ({fd_backing.value}/{access.value})",
        )

    # (3) 라이브 fd 가 게스트 의도(seek/mmap)를 못 줌 → memfd 복제 강등.
    return ProxyFdResolution(
        ProxyAction.MEMFD_CLONE,
        injection=FdInjection(
            source="memfd",
            read_only=target.read_only,
            # rw 마운트 + 변형 의도면 close 에서 PFD 로 write-back.
            writeback_on_close=mutates and not target.read_only,
            requires_seek=needs_seek,
            requires_mmap=needs_mmap,
        ),
        degraded_from_live=True,
        reason=(
            f"PFD {fd_backing.value} cannot satisfy {access.value} "
            "(seek/mmap) — memfd clone"
        ),
    )

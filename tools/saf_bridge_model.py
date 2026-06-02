"""SAF(Storage Access Framework) 파일 브리지 — 순수 매핑/정책 모델.

비root Android에서는 게스트 rootfs를 사용자 저장소(사진/다운로드/문서)에
직접 mount/FUSE 할 수 없다. 대신 SAF tree URI(ACTION_OPEN_DOCUMENT_TREE로
사용자가 부여, persistable permission)를 게스트 경로공간의 한 지점
(예: /mnt/android/<label>)에 매핑한다.

이 모듈은 그 매핑/정책의 **순수 로직**만 담는다 — Android API(DocumentFile,
ContentResolver) 호출은 없다. 런타임(WS-1 path 중재)이 게스트 openat 경로를
이 모델로 분류·검증한 뒤, 매핑된 경우 SAF 호출로 프록시한다.

설계 문서: docs/design/file-bridge-saf.md
런타임 계약: 같은 문서 §"런타임 인터페이스 계약(§5-F)".

핵심 불변식:
  1. 게스트가 보는 경로는 항상 mount point(/mnt/android/<label>) 아래의
     POSIX 절대경로. tree 안의 상대 경로(`rel`)만 SAF DocumentFile로 내려간다.
  2. traversal(`..`)·심볼릭 이스케이프로 mount 밖(rootfs/Android FS)을
     벗어나는 경로는 거부한다(rootfs-escape 차단).
  3. 디렉토리 정책(READ_ONLY/READ_WRITE)을 쓰기 연산 전에 강제한다.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from enum import Enum
from typing import Optional


# 게스트 측 SAF mount 들이 모이는 부모 디렉토리. rootfs 안의 고정 위치.
SAF_MOUNT_ROOT = "/mnt/android"

# label: 사용자에게 보이는 마운트 이름(예: "downloads", "dcim", "docs").
# 경로 컴포넌트로 쓰이므로 안전한 문자만 허용.
_LABEL_RE = re.compile(r"^[A-Za-z0-9._-]+$")


class AccessMode(Enum):
    """디렉토리(또는 그 하위) 접근 정책."""

    READ_ONLY = "ro"
    READ_WRITE = "rw"


class BridgeMode(Enum):
    """마운트가 _희망_ 하는 브리지 메커니즘(`AccessMode` 와 직교).

    docs/design/saf-proxy.md §0/§5.
      COPY  = 게스트 open 을 rootfs 임시파일 copy-in/out 으로(폴백, 최대 호환).
      PROXY = 게스트 open 결과 fd 를 SAF 가 연 라이브 fd 로 치환(직통, 진짜
              마운트感). 실제 동작은 런타임 능력에 따라 copy 로 강등될 수 있다.

    `bridge_mode` 는 _희망_ 일 뿐, escape/ro 정책이 항상 이긴다(§5 불변식 3).
    """

    COPY = "copy"
    PROXY = "proxy"


class BridgeDecision(Enum):
    """supervisor(`runtime_report.cpp` trap 핸들러)가 게스트 openat 트랩에서
    내릴 결정. file-bridge-saf.md §5-F 의 C++ `SafDecision` 과 1:1.

      FALLTHROUGH   = SAF 무관: 기존 rootfs path rewrite 로 처리(현행 동작).
      REWRITE_PATH  = copy 모드: copy-in 한 host 경로로 in-place x1 rewrite.
      SUBSTITUTE_FD = proxy 모드: ALR 가 연 SAF fd 를 게스트 fd 로 치환(§3-C).
      DENY          = 정책 위반(ro 위반/escape): EACCES/EROFS 로 실패.
    """

    FALLTHROUGH = "fallthrough"
    REWRITE_PATH = "rewrite_path"
    SUBSTITUTE_FD = "substitute_fd"
    DENY = "deny"


class Operation(Enum):
    """게스트가 요청하는 파일 연산의 의도(쓰기 권한 강제에 사용)."""

    READ = "read"
    WRITE = "write"
    CREATE = "create"
    DELETE = "delete"
    LIST = "list"
    STAT = "stat"

    @property
    def mutates(self) -> bool:
        return self in (Operation.WRITE, Operation.CREATE, Operation.DELETE)


class BridgeErrorKind(Enum):
    NOT_MAPPED = "not_mapped"          # 이 게스트 경로는 SAF 마운트 밖
    ESCAPE = "escape"                  # traversal로 마운트 밖 탈출 시도
    READ_ONLY_VIOLATION = "read_only"  # ro 마운트에 쓰기 시도
    UNKNOWN_LABEL = "unknown_label"    # 그런 label 마운트 없음
    BAD_LABEL = "bad_label"            # label 형식 위반
    BAD_URI = "bad_uri"                # tree URI 형식 위반


class BridgeError(Exception):
    """브리지 정책/안전성 위반. kind로 호출자가 분기."""

    def __init__(self, kind: BridgeErrorKind, message: str):
        super().__init__(f"{kind.value}: {message}")
        self.kind = kind


@dataclass(frozen=True)
class SafMount:
    """SAF tree URI 하나를 게스트 경로 한 지점에 매핑한 마운트.

    tree_uri: ACTION_OPEN_DOCUMENT_TREE 결과 + persistable permission을
              가진 URI 문자열 (예:
              "content://com.android.externalstorage.documents/tree/primary%3ADownload").
    label:    게스트에서의 마운트 이름. guest_path = /mnt/android/<label>.
    mode:     READ_ONLY / READ_WRITE.
    """

    tree_uri: str
    label: str
    mode: AccessMode = AccessMode.READ_ONLY
    # 희망 브리지 메커니즘. 기본 COPY(가장 안전한 폴백). `mode`(ro/rw)와 직교.
    bridge_mode: BridgeMode = BridgeMode.COPY

    def __post_init__(self) -> None:
        if not _LABEL_RE.match(self.label):
            raise BridgeError(
                BridgeErrorKind.BAD_LABEL,
                f"label must match {_LABEL_RE.pattern!r}: {self.label!r}",
            )
        if not _is_tree_uri(self.tree_uri):
            raise BridgeError(
                BridgeErrorKind.BAD_URI,
                f"not a SAF tree uri: {self.tree_uri!r}",
            )

    @property
    def guest_mount_point(self) -> str:
        """이 마운트의 게스트 절대경로 루트(예 /mnt/android/downloads)."""
        return f"{SAF_MOUNT_ROOT}/{self.label}"


@dataclass(frozen=True)
class ResolvedPath:
    """게스트 경로를 SAF 마운트로 해석한 결과.

    런타임(§5-F)이 이 값을 받아 SAF DocumentFile 호출을 만든다:
      DocumentFile.fromTreeUri(tree_uri) 부터 rel 컴포넌트를 따라 내려가
      대상 문서를 연다.
    """

    tree_uri: str
    label: str
    mode: AccessMode
    # 마운트 루트 기준 상대 경로 컴포넌트(빈 리스트 = 마운트 루트 자체).
    rel_components: tuple[str, ...]
    # 이 마운트가 희망하는 브리지 메커니즘(런타임 decision 매핑 입력).
    bridge_mode: BridgeMode = BridgeMode.COPY

    @property
    def rel_path(self) -> str:
        return "/".join(self.rel_components)

    @property
    def is_mount_root(self) -> bool:
        return len(self.rel_components) == 0

    @property
    def mount_prefix(self) -> str:
        """이 해석의 게스트 마운트 루트(예 /mnt/android/downloads).

        proxy fd-주입 대상 판정에서 supervisor 가 "이 트랩 경로가 어느 SAF
        마운트 prefix 인가"를 모호함 없이 알게 하는 값.
        """
        return f"{SAF_MOUNT_ROOT}/{self.label}"


def _is_tree_uri(uri: str) -> bool:
    # content://<authority>/tree/<docid...> 형태만 SAF tree URI 로 인정.
    # 실제 권한 보유 여부는 런타임이 ContentResolver로 확인(모델 밖).
    return uri.startswith("content://") and "/tree/" in uri


def normalize_guest_path(path: str) -> list[str]:
    """게스트 POSIX 경로를 정규화해 컴포넌트 리스트로.

    `.`은 제거, `..`은 한 단계 상위로(pop). 절대경로 기준에서 루트 위로
    더 올라가려는 `..`는 **escape**로 간주해 BridgeError(ESCAPE).
    (런타임 alr_path 의 clamp-at-guest-root 와 같은 의미지만, 여기서는
    마운트 루트 밖으로의 탈출을 막기 위해 명시적으로 에러를 낸다.)

    상대경로(선행 `/` 없음)는 호출자가 mount point 기준으로 합칠 때만
    의미가 있으므로, 단독 호출 시에도 동일 규칙으로 정규화한다.
    """
    if not path:
        raise BridgeError(BridgeErrorKind.NOT_MAPPED, "empty path")
    out: list[str] = []
    for comp in path.split("/"):
        if comp == "" or comp == ".":
            continue
        if comp == "..":
            if not out:
                # 절대경로 루트(또는 상대 기준점) 위로 탈출
                raise BridgeError(
                    BridgeErrorKind.ESCAPE,
                    f"path escapes above root via '..': {path!r}",
                )
            out.pop()
            continue
        out.append(comp)
    return out


class SafBridge:
    """label -> SafMount 등록부 + 게스트 경로 해석/정책 강제.

    런타임은 게스트 openat(path) 마다 resolve(path, op) 를 호출한다.
    BridgeError(NOT_MAPPED) 면 SAF 경로가 아니므로 런타임은 기존 rootfs
    경로 중재로 폴백한다. 성공하면 ResolvedPath 로 SAF 프록시를 만든다.
    """

    def __init__(self) -> None:
        self._mounts: dict[str, SafMount] = {}

    # --- 등록/해제 --------------------------------------------------------
    def add_mount(self, mount: SafMount) -> None:
        self._mounts[mount.label] = mount

    def remove_mount(self, label: str) -> None:
        self._mounts.pop(label, None)

    def mounts(self) -> tuple[SafMount, ...]:
        return tuple(self._mounts.values())

    def mount_for_label(self, label: str) -> SafMount:
        mount = self._mounts.get(label)
        if mount is None:
            raise BridgeError(
                BridgeErrorKind.UNKNOWN_LABEL, f"no mount labeled {label!r}"
            )
        return mount

    # --- 경로 해석 --------------------------------------------------------
    def is_saf_path(self, guest_path: str) -> bool:
        """게스트 경로가 어떤 SAF 마운트 아래에 있나(정책/escape 무시, 분류용)."""
        try:
            self._label_and_rel(guest_path)
            return True
        except BridgeError:
            return False

    def _label_and_rel(self, guest_path: str) -> tuple[str, list[str]]:
        """게스트 절대경로를 (label, mount-루트-기준 상대 컴포넌트) 로.

        SAF_MOUNT_ROOT 아래가 아니면 NOT_MAPPED.
        """
        comps = normalize_guest_path(guest_path)
        root_comps = [c for c in SAF_MOUNT_ROOT.split("/") if c]
        n = len(root_comps)
        if comps[:n] != root_comps or len(comps) <= n:
            raise BridgeError(
                BridgeErrorKind.NOT_MAPPED,
                f"path is not under {SAF_MOUNT_ROOT}: {guest_path!r}",
            )
        label = comps[n]
        rel = comps[n + 1 :]
        return label, rel

    def resolve(self, guest_path: str, op: Operation = Operation.READ) -> ResolvedPath:
        """게스트 경로 + 연산 의도를 ResolvedPath 로. 정책/안전성 강제.

        - SAF 마운트 밖 -> BridgeError(NOT_MAPPED)  (런타임이 rootfs 폴백)
        - 알 수 없는 label -> UNKNOWN_LABEL
        - `..` 로 마운트 밖 탈출 -> ESCAPE (normalize에서 이미 차단되나,
          마운트 루트 자체로의 후퇴도 rel 음수가 되지 않게 보장)
        - ro 마운트 + 변형 연산 -> READ_ONLY_VIOLATION
        """
        label, rel = self._label_and_rel(guest_path)
        mount = self.mount_for_label(label)  # UNKNOWN_LABEL 가능

        # rel 컴포넌트 자체에 `..`/`.`가 남아 있지 않음(normalize_guest_path가
        # 이미 정리). 추가 안전벨트: rel 내 어떤 항목도 부모 참조가 아님.
        if any(c in ("..", ".", "") for c in rel):
            raise BridgeError(
                BridgeErrorKind.ESCAPE, f"residual traversal in {guest_path!r}"
            )

        if op.mutates and mount.mode is AccessMode.READ_ONLY:
            raise BridgeError(
                BridgeErrorKind.READ_ONLY_VIOLATION,
                f"{op.value} on read-only mount {label!r}",
            )
        return ResolvedPath(
            tree_uri=mount.tree_uri,
            label=mount.label,
            mode=mount.mode,
            rel_components=tuple(rel),
            bridge_mode=mount.bridge_mode,
        )


# --- §5-F decision 매핑 (host 절반: trap 결정 모델) ---------------------------
#
# file-bridge-saf.md §5-F / saf-proxy.md §5. 게스트 openat 트랩에서 supervisor 가
# 어떤 decision 을 내려야 하는지의 **순수 함수**. Android API/JNI 호출 0 — fd 를
# 실제로 열거나 복사하거나 주입하지 않는다. "어느 트랩에서 어떤 결정을, 어떤
# 페이로드로" 만 고정한다(실 fd/copy 는 런타임 책임).

# DENY 에 실리는 errno(런타임이 게스트에 반환). file-bridge-saf.md §5-F 매핑.
_EACCES = 13  # escape/정책 위반
_EROFS = 30   # read-only 마운트에 쓰기


@dataclass(frozen=True)
class ProxyTarget:
    """SUBSTITUTE_FD 결정의 페이로드 = proxy fd-주입 _대상_ 메타데이터.

    supervisor(saf-proxy.md §3-C)가 이 값으로 SAF URI 를 만들어 fd 를 열고
    게스트 fd 로 치환한다. `mount_prefix` 가 정확히 `/mnt/android/<label>` 이고
    `rel_path` 가 그 아래 상대경로임이 보장된다(prefix 밖이면 애초에
    FALLTHROUGH — fd-주입 비대상).
    """

    tree_uri: str
    label: str
    mount_prefix: str   # /mnt/android/<label> — fd-주입 대상 prefix 판정
    rel_path: str       # mount_prefix 아래 상대경로(마운트 루트면 "")
    read_only: bool     # ro 마운트면 openFileDescriptor(uri, "r")


@dataclass(frozen=True)
class SafResolution:
    """`decide()` 결과 = trap decision + 모드별 페이로드.

    런타임(`alr_saf_resolve`)이 이 값을 C++ `SafResolveResult` 로 옮긴다.
      - FALLTHROUGH : 둘 다 None — 기존 rootfs 중재로 폴백.
      - DENY        : deny_errno(13/30) — 게스트에 EACCES/EROFS.
      - REWRITE_PATH: resolved + copy_writeback(쓰기면 close 에서 write-back).
                      실제 copy-in host 경로는 런타임이 채운다(모델 밖).
      - SUBSTITUTE_FD: proxy_target — supervisor 가 fd 를 열어 치환.
    """

    decision: BridgeDecision
    resolved: Optional[ResolvedPath] = None
    deny_errno: Optional[int] = None
    proxy_target: Optional[ProxyTarget] = None
    copy_writeback: bool = False


def decide(
    bridge: "SafBridge",
    guest_path: str,
    *,
    write: bool = False,
    create: bool = False,
) -> SafResolution:
    """게스트 openat 경로 + 쓰기/생성 의도 → trap decision(순수).

    `write`/`create` 는 openat flags 에서 런타임이 뽑은 의도(O_WRONLY/O_RDWR =>
    write, O_CREAT => create). 둘 다 변형 의도로 묶여 정책에 쓰인다.

    분기(saf-proxy.md §5 표):
      NOT_MAPPED            -> FALLTHROUGH
      ro 마운트 + 변형 의도 -> DENY(EROFS)
      ESCAPE               -> DENY(EACCES)
      ok + PROXY           -> SUBSTITUTE_FD(proxy_target)
      ok + COPY            -> REWRITE_PATH(copy_writeback=변형 의도)

    escape/ro 정책은 모드 분기 _전에_ `resolve` 로 강제되므로, proxy 든 copy 든
    /mnt/android/<label> 밖을 못 만지고 ro 에 못 쓴다(불변식 1·3).
    """
    mutates = write or create
    op = (
        Operation.CREATE if create else Operation.WRITE if write else Operation.READ
    )
    try:
        resolved = bridge.resolve(guest_path, op)
    except BridgeError as e:
        if e.kind is BridgeErrorKind.NOT_MAPPED:
            # SAF 밖 — 런타임이 기존 rootfs path rewrite 로 처리.
            return SafResolution(BridgeDecision.FALLTHROUGH)
        if e.kind is BridgeErrorKind.UNKNOWN_LABEL:
            # /mnt/android/<없는라벨> — SAF 경로 모양이나 등록 안 됨. 게스트가
            # 임의 라벨로 rootfs 를 침범하지 못하게 DENY(폴백 아님: prefix 안).
            return SafResolution(BridgeDecision.DENY, deny_errno=_EACCES)
        if e.kind is BridgeErrorKind.READ_ONLY_VIOLATION:
            return SafResolution(BridgeDecision.DENY, deny_errno=_EROFS)
        # ESCAPE / 그 외 정책 위반 -> EACCES.
        return SafResolution(BridgeDecision.DENY, deny_errno=_EACCES)

    # 여기 도달 = escape/ro 통과한 SAF 경로. 모드로만 분기.
    if resolved.bridge_mode is BridgeMode.PROXY:
        target = ProxyTarget(
            tree_uri=resolved.tree_uri,
            label=resolved.label,
            mount_prefix=resolved.mount_prefix,
            rel_path=resolved.rel_path,
            read_only=resolved.mode is AccessMode.READ_ONLY,
        )
        return SafResolution(
            BridgeDecision.SUBSTITUTE_FD,
            resolved=resolved,
            proxy_target=target,
        )
    # COPY: copy-in 후 host 경로 rewrite. 변형 의도면 close 에서 write-back.
    return SafResolution(
        BridgeDecision.REWRITE_PATH,
        resolved=resolved,
        copy_writeback=mutates,
    )


# --- share intent (ACTION_SEND / ACTION_SEND_MULTIPLE) 라우팅 ------------------


@dataclass(frozen=True)
class IncomingShare:
    """안드로이드가 ACTION_SEND 로 넘긴 파일 한 건의 모델.

    런타임이 ContentResolver.openInputStream(content_uri) 로 바이트를 읽어
    아래 guest_dest 로 복사한다(in→ 단방향). content_uri 는 1회용
    (persistable 아님)이므로 in-bound copy 가 SAF 마운트보다 단순/안전.
    """

    content_uri: str
    display_name: str  # 안드로이드가 알려준 파일명(없으면 호출자가 생성)
    mime_type: Optional[str] = None


# share 로 받은 파일이 떨어지는 게스트 인박스(rootfs 안 고정 경로).
SHARE_INBOX = "/root/Android-Share"

_SAFE_NAME_RE = re.compile(r"^[A-Za-z0-9 ._()\[\]+-]+$")


def sanitize_share_name(name: str) -> str:
    """share display name 을 게스트 파일명으로 안전화.

    경로 구분자/상위참조 제거, 빈/위험 문자는 '_'로. traversal 불가.
    """
    base = name.replace("\\", "/").split("/")[-1].strip()
    if base in ("", ".", ".."):
        return "shared-file"
    cleaned = "".join(ch if _SAFE_NAME_RE.match(ch) else "_" for ch in base)
    return cleaned or "shared-file"


def route_incoming_share(share: IncomingShare) -> str:
    """share 파일이 복사될 게스트 절대경로를 계산(SHARE_INBOX 안, traversal 불가)."""
    safe = sanitize_share_name(share.display_name)
    # SHARE_INBOX 아래 한 단계로 고정 — 디렉토리 escape 불가.
    return f"{SHARE_INBOX}/{safe}"


# --- 게스트 출력 -> 안드로이드 저장소(MediaStore) export ------------------------


@dataclass(frozen=True)
class MediaStoreTarget:
    """게스트가 만든 결과물을 MediaStore 로 내보낼 때의 목적지 기술.

    런타임이 MediaStore.<collection>.EXTERNAL_CONTENT_URI 에 ContentValues
    (DISPLAY_NAME, MIME_TYPE, RELATIVE_PATH)로 insert 후, 게스트 출력 파일을
    그 URI 로 복사한다(out→ 단방향). SAF tree 권한이 없어도 동작.
    """

    collection: str       # "images" | "downloads" | "documents" | "audio" | "video"
    display_name: str
    relative_path: str     # 예: "Download/AndroLinux" (MediaStore RELATIVE_PATH)
    mime_type: Optional[str] = None


_ALLOWED_COLLECTIONS = frozenset(
    {"images", "downloads", "documents", "audio", "video"}
)


def export_to_mediastore(
    guest_output_path: str,
    collection: str,
    display_name: str,
    relative_path: str = "Download/AndroLinux",
    mime_type: Optional[str] = None,
) -> MediaStoreTarget:
    """게스트 출력 경로를 MediaStore export 기술로 변환.

    collection 검증 + display_name 안전화. guest_output_path 자체는 호출자
    (런타임)가 rootfs 안에서 읽는다 — 여기서는 목적지만 모델링.
    """
    if collection not in _ALLOWED_COLLECTIONS:
        raise BridgeError(
            BridgeErrorKind.NOT_MAPPED,
            f"unknown MediaStore collection {collection!r}",
        )
    return MediaStoreTarget(
        collection=collection,
        display_name=sanitize_share_name(display_name),
        relative_path=relative_path,
        mime_type=mime_type,
    )

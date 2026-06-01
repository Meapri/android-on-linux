# Rootfs Extraction Safety

Rootfs archives are untrusted until verified. Even if the first payload is bundled by us, the runtime must treat rootfs archives as hostile because later versions may be downloaded or replaced.

## Required gates before extraction

1. Manifest exists and validates schema.
2. Archive file size matches manifest `size_bytes`.
3. Archive SHA256 matches manifest `sha256`.
4. Tar members pass safety inspection.
5. Extraction target is app-private `files/rootfs/<name>`.
6. Version marker is written only after successful extraction.

## Tar member policy

Allowed:

- Regular files with relative paths.
- Directories with relative paths.
- Symlinks/hardlinks only when link target is relative and does not contain `..`.

Rejected:

- Absolute paths, e.g. `/system/bin/sh`.
- Parent traversal, e.g. `../escape`.
- Symlink or hardlink escape, e.g. `lib/x -> ../../outside`.
- Character devices.
- Block devices.
- FIFOs.
- Unknown tar member types.

## Android-specific notes

A rootfs archive must not be extracted into an executable trust boundary. The extracted files remain writable app data and must not be treated as direct `execve()` entrypoints on modern Android.

The initial executable remains the packaged native loader in `nativeLibraryDir`; rootfs files are data consumed by the runtime/backend.

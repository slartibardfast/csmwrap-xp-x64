# The pinned toolchain CSMWrap is built with.
#
# `.host-software` records this image by digest, so everything below is fixed at the
# moment the image is built. `podman build --timestamp 0` keeps the layers
# reproducible, so the same inputs give the same image digest.
#
# The package set is what CSMWrap's GNUmakefile and its SeaBIOS submodule actually
# invoke: base-devel for cc, ld, objcopy and make; nasm for the hand-written
# assembly; python for SeaBIOS's configtools and layout tools; xxd for turning the
# SeaBIOS blobs into headers; git because BUILD_VERSION is `git describe`.
FROM docker.io/library/archlinux:base

RUN pacman -Syu --noconfirm --needed \
        base-devel \
        git \
        nasm \
        python \
        xxd \
    && pacman -Scc --noconfirm \
    && rm -rf /var/cache/pacman/pkg/*

# The build container runs as root against a worktree the invoking user owns, so git
# refuses the repository as "dubious ownership" and `git describe` — which feeds
# BUILD_VERSION — falls back to the literal string "Unknown". That changes the
# embedded version and therefore the artifact bytes, so the refusal is lifted inside
# the image rather than worked around at every call site. Pinned per image, so it
# cannot drift with the host's git config.
RUN git config --system --add safe.directory '*'

WORKDIR /src

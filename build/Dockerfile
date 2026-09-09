# PSP build environment for MasterBoy (single-game launcher build).
#
# The stock pspdev image ships the SDK plus zlib/libpng/bzip2, but MasterBoy also
# links -losl (OSLib), which is not included. OSLib is available as an official
# pspdev package (built from dogo/oslib, the maintained OSLib MOD), so we pull it
# in with psp-pacman and fall back to a source build if the package is unavailable.
FROM pspdev/pspdev:latest

ENV PSPDEV=/usr/local/pspdev
ENV PSPSDK=$PSPDEV/psp/sdk
ENV PATH=$PSPDEV/bin:$PATH

RUN apk add --no-cache git make cmake build-base bash python3 2>/dev/null || \
    (apt-get update && apt-get install -y --no-install-recommends \
        git make cmake build-essential bash python3 && rm -rf /var/lib/apt/lists/*)

# --- OSLib (libosl.a + oslib/ headers) ---------------------------------------
RUN set -eux; \
    if command -v psp-pacman >/dev/null 2>&1; then \
        psp-pacman -Sy --noconfirm oslib || psp-pacman -S --noconfirm oslib; \
    else \
        git clone --depth 1 https://github.com/dogo/oslib /tmp/oslib; \
        cd /tmp/oslib; \
        make -j"$(nproc)"; \
        make install; \
        rm -rf /tmp/oslib; \
    fi; \
    test -f "$PSPDEV/psp/lib/libosl.a" || test -f "$PSPSDK/lib/libosl.a"

WORKDIR /project
CMD ["/bin/bash"]

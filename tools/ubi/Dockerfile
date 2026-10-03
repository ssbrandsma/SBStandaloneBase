FROM ubuntu:24.04
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential zlib1g-dev liblzo2-dev uuid-dev patch file \
    && rm -rf /var/lib/apt/lists/*
ARG MTD_COMMIT=bb56df1e1a84304ec4a14169e4cdc41116ed4256
RUN git clone https://git.kernel.org/pub/scm/linux/kernel/git/rw/mtd-utils.git /src/mtd-utils \
    && cd /src/mtd-utils && git checkout "$MTD_COMMIT"
COPY historical-build.patch /tmp/historical-build.patch
RUN cd /src/mtd-utils && patch -p1 </tmp/historical-build.patch \
    && mkdir -p /build/ubi-utils /build/mkfs \
    && make BUILDDIR=/build/ubi-utils -C ubi-utils /build/ubi-utils/libubi.a \
       CFLAGS='-O2 -std=gnu89' \
    && make BUILDDIR=/build/mkfs -C mkfs.ubifs /build/mkfs/mkfs.ubifs \
       CFLAGS='-O2 -std=gnu89' LDFLAGS='-L/build/ubi-utils'
RUN strip /build/mkfs/mkfs.ubifs && /build/mkfs/mkfs.ubifs -V
WORKDIR /work
ENTRYPOINT ["/build/mkfs/mkfs.ubifs"]

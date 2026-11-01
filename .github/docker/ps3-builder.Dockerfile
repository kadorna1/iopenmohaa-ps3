FROM ubuntu:24.04

LABEL org.opencontainers.image.source="https://github.com/kadorna1/iopenmohaa-ps3"

ENV DEBIAN_FRONTEND=noninteractive

COPY .github/ps3-host-deps.txt /tmp/ps3-host-deps.txt

RUN apt-get update \
    && sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' /tmp/ps3-host-deps.txt \
       | xargs -r apt-get install -y --no-install-recommends \
    && rm -rf /var/lib/apt/lists/* /tmp/ps3-host-deps.txt

WORKDIR /workspace

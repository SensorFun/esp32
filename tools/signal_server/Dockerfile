FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential autoconf automake libtool pkg-config \
    gengetopt libglib2.0-dev libjansson-dev libconfig-dev libssl-dev \
    libnice-dev libsrtp2-dev libwebsockets-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 --branch v1.4.3 https://github.com/meetecho/janus-gateway.git janus
WORKDIR /src/janus
RUN sh autogen.sh && ./configure --prefix=/opt/janus \
    --disable-all-plugins --disable-all-handlers --disable-all-loggers \
    --disable-data-channels --disable-turn-rest-api --disable-rest \
    --disable-rabbitmq --disable-mqtt --disable-unix-sockets --disable-nanomsg \
    --disable-docs && make -j2 && make install
RUN --mount=type=bind,source=plugin.c,target=/src/plugin.c,ro \
    mkdir -p /opt/janus/lib/janus/plugins && \
    cc -shared -fPIC -Wall -Wextra -Werror -I/src/janus/src \
    $(pkg-config --cflags glib-2.0 jansson) /src/plugin.c \
    -o /opt/janus/lib/janus/plugins/libjanus_signaling.so \
    $(pkg-config --libs glib-2.0 jansson)

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates python3 python3-aiohttp libglib2.0-0 libjansson4 libconfig9 libssl3 \
    libnice10 libsrtp2-1 libwebsockets17 libwebsockets-evlib-glib \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/janus /opt/janus
RUN useradd --system --uid 10001 --no-create-home signaling
USER 10001
ENTRYPOINT ["python3", "/opt/signaling/entrypoint.py"]

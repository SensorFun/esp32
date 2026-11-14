FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-aiohttp && rm -rf /var/lib/apt/lists/*
RUN useradd --system --uid 10001 --no-create-home signaling
USER 10001
ENTRYPOINT ["python3", "/opt/signaling/server.py"]

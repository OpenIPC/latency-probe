# Host side of latency-probe: builds the ARM probe and runs the analysis and browser tests.
FROM python:3.12-slim
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg openssh-client make && rm -rf /var/lib/apt/lists/*
ENV PLAYWRIGHT_BROWSERS_PATH=/ms-playwright
RUN pip install --no-cache-dir av numpy playwright ziglang && playwright install --with-deps chromium
WORKDIR /w

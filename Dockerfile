# syntax=docker/dockerfile:1
#
# Three stages: build the React UI, build the C++ binaries, then assemble a
# runtime image that carries only ffmpeg, the two binaries and the static UI.
#
#   docker build -t mp4web .
#   docker run --rm -p 8787:8787 -v "$PWD/data:/app/.mp4web" mp4web

# ---------------------------------------------------------------- UI build
FROM node:24-alpine AS web
WORKDIR /app/web
# copy manifests first so the dependency layer caches independently of source
COPY web/package.json web/package-lock.json ./
RUN npm ci
COPY web/ ./
RUN npm run build

# --------------------------------------------------------------- C++ build
FROM alpine:3.20 AS build
RUN apk add --no-cache g++ musl-dev
WORKDIR /src
COPY mp4probe.hpp mp4check.cpp mp4web.cpp ./
RUN g++ -std=c++17 -O2 -Wall -o mp4check mp4check.cpp \
 && g++ -std=c++17 -O2 -Wall -pthread -o mp4web mp4web.cpp

# ----------------------------------------------------------------- runtime
FROM alpine:3.20
# ffmpeg carries libx264/libvpx; libstdc++ is needed by the C++ binaries
RUN apk add --no-cache ffmpeg libstdc++ \
 && addgroup -S app && adduser -S -G app app

WORKDIR /app
COPY --from=build /src/mp4check /src/mp4web /usr/local/bin/
COPY --from=web   /app/web/dist ./web/dist

# the server writes uploads and encoded output under ./.mp4web
RUN mkdir -p /app/.mp4web && chown -R app:app /app
VOLUME ["/app/.mp4web"]

USER app
EXPOSE 8787
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s \
  CMD wget -qO- http://127.0.0.1:8787/api/jobs >/dev/null || exit 1

ENTRYPOINT ["mp4web"]
CMD ["8787", "--dist", "/app/web/dist"]

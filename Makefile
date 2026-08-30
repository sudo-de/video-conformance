CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall
PORT     ?= 8787

all: mp4check mp4web compile_commands.json

mp4check: mp4check.cpp mp4probe.hpp
	$(CXX) $(CXXFLAGS) -o $@ mp4check.cpp

mp4web: mp4web.cpp mp4probe.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ mp4web.cpp

web: web/dist/index.html

web/dist/index.html: $(wildcard web/src/*) web/package.json
	cd web && npm install --silent && npm run build

## production: build everything, serve UI + API from the C++ server
serve: all web
	./mp4web $(PORT) --dist web/dist

## development: API on 8787, Vite dev server with HMR on 5173
dev: mp4web
	./mp4web $(PORT) --dist web/dist & cd web && npm run dev

## regenerate the compilation database so IDE IntelliSense uses -std=c++17
compile_commands.json: Makefile
	@SDK=$$(xcrun --show-sdk-path 2>/dev/null); printf '[\n' > $@; \
	 printf '  {"directory":"%s","file":"%s/mp4check.cpp","command":"clang++ -std=c++17 -O2 -Wall -isysroot %s -c mp4check.cpp"},\n' "$$PWD" "$$PWD" "$$SDK" >> $@; \
	 printf '  {"directory":"%s","file":"%s/mp4web.cpp","command":"clang++ -std=c++17 -O2 -Wall -pthread -isysroot %s -c mp4web.cpp"}\n' "$$PWD" "$$PWD" "$$SDK" >> $@; \
	 printf ']\n' >> $@

## run the conformance test suite (requires ffmpeg)
test: mp4check
	./tests/smoke.sh

## build the container image (does not start Docker for you)
image:
	docker build -t mp4web:local .

clean:
	rm -f mp4check mp4web
	rm -rf web/dist .mp4web

.PHONY: all web serve dev test image clean

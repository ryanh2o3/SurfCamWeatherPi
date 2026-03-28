# SurfCamWeatherPi — convenience targets around CMake, ctest, and Docker.
# Requires a native toolchain with libcamera + GStreamer unless you use docker-* targets.

BUILD_DIR      ?= build
CMAKE_FLAGS    ?= -DCMAKE_BUILD_TYPE=Release
# Parallel jobs (macOS has no nproc)
JOBS           ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
DOCKER_IMAGE   ?= surfcam-build
DOCKERFILE     ?= Dockerfile.build

.PHONY: help all surfcam configure configure-test build build-test \
        test check unit-test http-test integration docker docker-tsan \
        tsan-local clean distclean

help:
	@echo "SurfCamWeatherPi Makefile"
	@echo ""
	@echo "  make / make surfcam   Configure (no test targets) and build surfcam only"
	@echo "  make build-test       Configure with BUILD_TESTING=ON and build surfcam + tests"
	@echo "  make test / check  Unit tests (ctest) + HTTP integration (mock + surfcam_test_http)"
	@echo "  make unit-test     ctest only"
	@echo "  make http-test     Mock API + surfcam_test_http only"
	@echo "  make docker        docker build -f $(DOCKERFILE) (release + tests, like CI)"
	@echo "  make docker-tsan   Same image with ENABLE_TSAN=ON (compile-only)"
	@echo "  make tsan-local    Native RelWithDebInfo + ENABLE_TSAN (Linux + deps required)"
	@echo "  make clean         cmake --build $(BUILD_DIR) --target clean"
	@echo "  make distclean     rm -rf $(BUILD_DIR)"
	@echo ""
	@echo "Variables: BUILD_DIR=$(BUILD_DIR) CMAKE_FLAGS='$(CMAKE_FLAGS)' JOBS=$(JOBS)"

all: surfcam

surfcam: configure
	cmake --build $(BUILD_DIR) -j$(JOBS) --target surfcam

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS) -DBUILD_TESTING=OFF

configure-test:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS) -DBUILD_TESTING=ON

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

build-test: configure-test
	cmake --build $(BUILD_DIR) -j$(JOBS)

test check: build-test
	ctest --test-dir $(BUILD_DIR) --output-on-failure
	SURFCAM_BUILD_DIR=$(BUILD_DIR) bash scripts/ci-run-integration-http-tests.sh

unit-test: build-test
	ctest --test-dir $(BUILD_DIR) --output-on-failure

http-test integration: build-test
	SURFCAM_BUILD_DIR=$(BUILD_DIR) bash scripts/ci-run-integration-http-tests.sh

docker:
	docker build -f $(DOCKERFILE) --build-arg ENABLE_TSAN=OFF -t $(DOCKER_IMAGE) .

docker-tsan:
	docker build -f $(DOCKERFILE) --build-arg ENABLE_TSAN=ON -t $(DOCKER_IMAGE)-tsan .

tsan-local:
	cmake -S . -B $(BUILD_DIR)-tsan $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DENABLE_TSAN=ON -DBUILD_TESTING=OFF
	cmake --build $(BUILD_DIR)-tsan -j$(JOBS)

clean:
	@cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-tsan

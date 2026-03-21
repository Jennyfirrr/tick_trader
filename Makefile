BUILD_DIR = build

all: $(BUILD_DIR)
	cmake --build $(BUILD_DIR) -j$$(nproc)

$(BUILD_DIR):
	cmake -B $(BUILD_DIR)

# build variants
profile:
	cmake -B $(BUILD_DIR) -DLATENCY_PROFILING=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

profile-lite:
	cmake -B $(BUILD_DIR) -DLATENCY_LITE=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

bench:
	cmake -B $(BUILD_DIR) -DLATENCY_BENCH=ON && cmake --build $(BUILD_DIR) -j$$(nproc)

# tests
test: all
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	rm -rf $(BUILD_DIR)

# reconfigure (clears cmake cache)
reconfigure:
	rm -rf $(BUILD_DIR) && cmake -B $(BUILD_DIR)

.PHONY: all profile profile-lite bench test clean reconfigure

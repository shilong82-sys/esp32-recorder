#!/bin/bash
set -e

export IDF_PATH=/Users/long/Projects/esp32-recorder/esp-idf
export IDF_PYTHON_ENV_PATH=/Users/long/.espressif/idf-python-venv
# Skip Python env checks
export IDF_SKIP_CHECK_SUBMODULES=1
export PATH="/Users/long/.local/bin:/Users/long/.espressif/tools/xtensa-esp-elf/esp-13.2.0_20230928/xtensa-esp-elf/bin:/Users/long/.espressif/tools/riscv32-esp-elf/esp-13.2.0_20230928/riscv32-esp-elf/bin:/Users/long/.espressif/tools/xtensa-esp-elf-gdb/14.2_20240403/xtensa-esp-elf-gdb/bin:/Users/long/.espressif/tools/openocd-esp32/bin:$PATH"
source /Users/long/.espressif/idf-python-venv/bin/activate

# Download constraints file
curl -fsSL -o /Users/long/.espressif/espidf.constraints.v5.2.txt https://dl.espressif.com/dl/esp-idf/espidf.constraints.v5.2.txt 2>/dev/null || echo "Constraints download failed, continuing anyway"

cd /Users/long/Projects/esp32-recorder/hello_world

echo "=== Setting target to ESP32-S3 ==="
python3 $IDF_PATH/tools/idf.py --no-ccache set-target esp32s3 2>&1 || true

echo ""
echo "=== Building hello_world ==="
python3 $IDF_PATH/tools/idf.py build 2>&1

echo "=== BUILD COMPLETE ==="

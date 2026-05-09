#!/bin/bash
set -e

export IDF_PATH=/Users/long/Projects/esp32-recorder/esp-idf
source /Users/long/.espressif/idf-python-venv/bin/activate

# Install with correct version constraints for ESP-IDF v5.2
pip install -r $IDF_PATH/tools/requirements/requirements.core.txt \
  -c /Users/long/.espressif/espidf.constraints.v5.2.txt 2>&1 | tail -20

echo "=== Done installing constrained deps ==="

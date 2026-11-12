#pragma once

// Pure signal processing, kept free of Arduino dependencies so it can be tested on a PC.
// Mirrors is_ping() and find_first_echo() in TUSS4470_shield_002/esp32_interface.py.

#include <stdint.h>
#include <stddef.h>
#include <algorithm>

struct EchoResult {
  bool found;
  int index;    // sample index of the first echo peak
  float level;  // threshold level in ADC counts
};

// Real pings start with the transmit ring-down; extra triggers without a ping do not.
inline bool isPing(const uint16_t *values, size_t count, size_t ringdownSamples, int threshold) {
  if (threshold <= 0) {
    return true;
  }
  size_t n = std::min(ringdownSamples, count);
  if (n == 0) {
    return false;
  }
  float sum = 0;
  for (size_t i = 0; i < n; i++) {
    sum += values[i];
  }
  return sum / n >= threshold;
}

// First peak after `start` that reaches `thresholdPercent` of the way from the baseline
// (median) to the strongest reading. `scratch` must hold at least `count` values.
inline EchoResult findFirstEcho(const uint16_t *values, size_t count, size_t start,
                                float thresholdPercent, uint16_t *scratch) {
  EchoResult result = {false, -1, 0};
  if (start >= count) {
    return result;
  }
  const uint16_t *segment = values + start;
  size_t n = count - start;

  std::copy(segment, segment + n, scratch);
  std::sort(scratch, scratch + n);
  float baseline = (n % 2) ? scratch[n / 2] : (scratch[n / 2 - 1] + scratch[n / 2]) / 2.0f;
  float peak = scratch[n - 1];
  if (peak <= baseline) {
    return result;
  }

  float level = baseline + (peak - baseline) * thresholdPercent / 100.0f;
  size_t i = 0;
  while (i < n && segment[i] < level) {
    i++;
  }
  while (i + 1 < n && segment[i + 1] >= segment[i]) {
    i++;
  }
  result.found = true;
  result.index = (int)(start + i);
  result.level = level;
  return result;
}

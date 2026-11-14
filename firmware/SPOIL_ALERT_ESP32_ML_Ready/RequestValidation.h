#pragma once

// Parse a non-negative decimal item ID without accepting numeric prefixes,
// signs, whitespace, or values outside the available item range.
inline bool parseFruitId(const char* text, int itemCount, int* result) {
  if (text == nullptr || result == nullptr || itemCount <= 0 || *text == '\0') return false;

  int value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    const int digit = *cursor - '0';
    if (digit > itemCount - 1 || value > (itemCount - 1 - digit) / 10) return false;
    value = value * 10 + digit;
  }

  *result = value;
  return true;
}

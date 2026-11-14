#include <cassert>
#include "../SPOIL_ALERT_ESP32_ML_Ready/RequestValidation.h"

int main() {
  int id = -1;
  assert(parseFruitId("0", 50, &id) && id == 0);
  assert(parseFruitId("1", 50, &id) && id == 1);
  assert(parseFruitId("9", 10, &id) && id == 9);
  assert(!parseFruitId("10", 10, &id));
  assert(parseFruitId("49", 50, &id) && id == 49); // generic parser remains reusable

  assert(!parseFruitId("", 50, &id));
  assert(!parseFruitId("1x", 50, &id));
  assert(!parseFruitId("x1", 50, &id));
  assert(!parseFruitId("-1", 50, &id));
  assert(!parseFruitId("+1", 50, &id));
  assert(!parseFruitId(" 1", 50, &id));
  assert(!parseFruitId("1 ", 50, &id));
  assert(!parseFruitId("50", 50, &id));
  assert(!parseFruitId("999999999999999999999", 50, &id));
  return 0;
}

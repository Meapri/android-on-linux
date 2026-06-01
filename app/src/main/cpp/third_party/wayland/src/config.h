/* Shim so wayland sources that #include "config.h" (in src/) and those that
 * #include "../config.h" both resolve to the same hand-written config. */
#include "../config.h"

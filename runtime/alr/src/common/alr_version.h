/* Single source of truth for the release version.
 *
 * Consumed by `alr version`, scripts/make-release.sh and the termux-packages
 * recipe's TERMUX_PKG_VERSION.  Bump here and nowhere else -- three copies
 * drifting apart is how a bug report stops being actionable. */
#ifndef ALR_VERSION_H
#define ALR_VERSION_H

#define ALR_VERSION "1.0.0"

#endif /* ALR_VERSION_H */

#ifndef WF_VERSION_H
#define WF_VERSION_H
/* major.minor (user 2026-08-29): build.sh bumps MINOR on every build,
 * `./build.sh --major` bumps MAJOR (minor back to 0) when we decide to.
 * WF_VERSION stays a comparable number = major * 1000 + minor. */
#define WF_VERSION_MAJOR 1
#define WF_VERSION_MINOR 113
#define WF_VERSION (WF_VERSION_MAJOR * 1000 + WF_VERSION_MINOR)
#define WF_VERSION_STRING "1.113"
#endif

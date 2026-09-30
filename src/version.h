/* version.h - the single source of truth for the wnip version.
 *
 * Included by both C sources (through wnip.h) and the resource script, so the
 * numeric resource, the strings and the about box can never disagree.
 */
#ifndef WNIP_VERSION_H
#define WNIP_VERSION_H

#define WNIP_VERSION_MAJOR 1
#define WNIP_VERSION_MINOR 0
#define WNIP_VERSION_PATCH 0

/* Stringify / widen helpers usable from C and from windres. */
#define WNIP_VER_STR2(x) #x
#define WNIP_VER_STR(x)  WNIP_VER_STR2(x)
#define WNIP_VER_WIDE2(x) L##x
#define WNIP_VER_WIDE(x)  WNIP_VER_WIDE2(x)

/* Narrow form for the version resource ("1.0.0" and "1.0.0.0"). */
#define WNIP_VERSION_A  WNIP_VER_STR(WNIP_VERSION_MAJOR) "." \
                        WNIP_VER_STR(WNIP_VERSION_MINOR) "." \
                        WNIP_VER_STR(WNIP_VERSION_PATCH)
#define WNIP_VERSION_A4 WNIP_VERSION_A ".0"

/* Wide form for the UI. */
#define WNIP_VERSION_W WNIP_VER_WIDE(WNIP_VERSION_A)

#endif /* WNIP_VERSION_H */

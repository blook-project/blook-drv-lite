/*
 * blook-drv-lite - small kernel helpers (image base, names, safe reads).
 */
#ifndef BLOOK_DRVLITE_DRIVER_UTIL_H
#define BLOOK_DRVLITE_DRIVER_UTIL_H

#include "nt.h"

namespace drvlite {

/* Computes the load base of the image containing `address` by walking back
 * page by page and validating the PE headers against the image entry point. */
u64 image_base_from_pointer(const void* address);

/* SizeOfImage of a mapped PE image (0 when the headers are invalid). */
u32 image_size_at(u64 base);

/* Case insensitive comparison of a Windows UNICODE_STRING with an ASCII
 * literal. */
bool unicode_equals_ascii(const UNICODE_STRING& value, const char* ascii);

/* Copies a UNICODE_STRING (full path or base name) into a narrow buffer,
 * lower-cased, always NUL terminated. */
void narrow_copy(const UNICODE_STRING& value, char* output, usize capacity);

/* ASCII lower case helper. */
char ascii_lower(char value);

/* Reads a kernel pointer defensively: returns false instead of faulting when
 * the address is not mapped. */
bool safe_read_kernel(u64 address, void* buffer, usize size);

/* Writes to kernel memory through the physical-write path (read-only pages
 * included). Defined by mem/kva.cpp; declared here for patch users. */
bool write_kernel(u64 address, const void* buffer, usize size);
bool read_kernel(u64 address, void* buffer, usize size);

}  // namespace drvlite

#endif /* BLOOK_DRVLITE_DRIVER_UTIL_H */

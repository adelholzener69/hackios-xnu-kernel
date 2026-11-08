/*
 * What the NXP TFA98xx code (Apache-2.0, from Xiaomi's kernel for this phone) expects of a Linux kernel,
 * in terms of what this kernel has. The same header serves the host test (tools/tfa-host-test).
 */
#ifndef TFA_KSHIM_H
#define TFA_KSHIM_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#ifndef __KERNEL__
#define __KERNEL__ 1
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;

#define KERN_INFO ""
#define KERN_ERR ""
#define KERN_DEBUG ""
#define GFP_KERNEL 0
#define unlikely(x) (x)
#define likely(x) (x)
#define BIT(n) (1UL << (n))
#define cpu_to_be16(x) ((uint16_t)__builtin_bswap16((uint16_t)(x)))

int printf(const char *, ...);
#define printk printf
#define pr_info(...) printf(__VA_ARGS__)
#define pr_err(...) printf(__VA_ARGS__)
#define pr_warn(...) printf(__VA_ARGS__)
#define pr_debug(...) do { } while (0)

struct kmem_cache;
void *kmem_cache_alloc(struct kmem_cache *cache, int flags);
void kmem_cache_free(struct kmem_cache *cache, void *object);
void msleep_interruptible(unsigned ms);
#define msleep(ms) msleep_interruptible(ms)
uint32_t crc32_le(uint32_t crc, const unsigned char *data, size_t length);
void *tfa_kmalloc(size_t size);
void tfa_kfree(void *pointer);
#define kmalloc(n, f) tfa_kmalloc(n)
#define kzalloc(n, f) tfa_kmalloc(n)
#define kfree(p) tfa_kfree(p)
#define isalnum(c) (((c) >= '0' && (c) <= '9') || ((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define sscanf(...) 0   /* only used to print the API version */
char *tfa_strstr(const char *haystack, const char *needle);
#define strstr(h, n) tfa_strstr(h, n)
#endif

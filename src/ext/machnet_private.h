/**
 * @file nsaas_private.h
 *
 * Helper functions to integreate NSaaS with shared memory channels.
 * This is an internal API, not to be used from applications.
 */
#ifndef SRC_EXT_NSAAS_PRIVATE_H_
#define SRC_EXT_NSAAS_PRIVATE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "nsaas_common.h"

#define NSAAS_CHANNEL_CTRL_SQ_SLOT_NR 2
#define NSAAS_CHANNEL_CTRL_CQ_SLOT_NR (NSAAS_CHANNEL_CTRL_SQ_SLOT_NR)

// Count the number of array elements.
// clang-format off
#define COUNT_OF(x) \
  ((sizeof(x) / sizeof(0[x])) / ((size_t)(!(sizeof(x) % sizeof(0[x])))))
// clang-format on

#define IS_POW2(x) (((x) & ((x)-1)) == 0)
// Macro to round up to next power of 2.
#define ROUNDUP_U64_POW2(x) (1ULL << (64 - __builtin_clzll(((uint64_t)x) - 1)))

/**
 * Calculate the memory size needed for an NSaaS Dataplane channel.
 *
 * An NSaaS Dataplane channel contains two rings for message passing in each
 * direction (NSaaS -> Application, Application -> NSaas), and one ring that
 * holds free buffers (used for allocations).
 *
 * This function returns the number of bytes needed for the channel area, given
 * the number of elements in each of the rings of the channel and the desired
 * buffer size.
 *
 * @param nsaas_ring_slot_nr The number of NSaaS->App messaging ring slots (must
 *                           be power of 2).
 * @param app_ring_slot_nr   The number of App->NSaaS messaging ring slots (must
 *                           be power of 2).
 * @param buf_ring_slot_nr   The number of buffers + 1 in the pool (must be
 *                           power of 2).
 * @param buffer_size        The usable size of each buffer.
 * @param is_posix_shm       Whether the channel will be a POSIX shared memory.
 * @return
 *   - The memory size in bytes needed for the NSaaS channel on success.
 *   - (size_t)-1 - Some parameter is not a power of 2, or the buffer size is
 *                  bad (too big).
 */
static inline size_t __nsaas_channel_dataplane_calculate_size(
    size_t nsaas_ring_slot_nr, size_t app_ring_slot_nr, size_t buf_ring_slot_nr,
    size_t buffer_size, int is_posix_shm) {
  // Check that all parameters are power of 2.
  if (!IS_POW2(nsaas_ring_slot_nr) || !IS_POW2(app_ring_slot_nr) ||
      !IS_POW2(buf_ring_slot_nr))
    return -1;

  const size_t total_buffer_size = ROUNDUP_U64_POW2(
      buffer_size + NSAAS_MSGBUF_SPACE_RESERVED + NSAAS_MSGBUF_HEADROOM_MAX);

  const size_t kPageSize = (is_posix_shm ? getpagesize() : HUGE_PAGE_2M_SIZE);
  if (buffer_size > kPageSize) return -1;

  // Add the size of the channel's header.
  size_t total_size = sizeof(NSaaSChannelCtx_t);

  // Size of statistics structure.
  total_size += sizeof(NSaaSChannelStats_t);

  // Add the size of the rings (NSaaS, Application, BufferRing).
  size_t ring_sizes[] = {nsaas_ring_slot_nr, app_ring_slot_nr,
                         buf_ring_slot_nr};
  for (size_t i = 0; i < COUNT_OF(ring_sizes); i++) {
    size_t acc =
        jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t), ring_sizes[i]);
    if (acc == (size_t)-1) return -1;
    total_size += acc;
  }

  // Align to page boundary.
  total_size = ALIGN_TO_PAGE_SIZE(total_size, kPageSize);

  // Add the size of the buffers.
  total_size += buf_ring_slot_nr * total_buffer_size;

  // Align to page boundary.
  total_size = ALIGN_TO_PAGE_SIZE(total_size, kPageSize);

  return total_size;
}

/**
 * Initialiaze an NSaaS Dataplane channel.
 *
 * This function initializes the memory of an NSaaS Dataplane channel. It
 * initializes the context, the rings and buffers required to facilitate
 * bidirectional communication.
 *
 * @param shm                Pointer to the shared memory area.
 * @param shm_size           Size of the shared memory area.
 * @param is_posix_shm       Whether the channel is based on POSIX shared
 *                           memory(1, or 0 otherwise).
 * @param name               The name of the channel.
 * @param nsaas_ring_slot_nr The number of NSaaS->App messaging ring slots.
 * @param app_ring_slot_nr   The number of App->NSaaS messaging ring slots.
 * @param buf_ring_slot_nr   The number of buffers + 1 to be used in this
 *                           channel (must sum up to a power of 2).
 * @param buffer_size        The size of each buffer.
 * @param is_multithread     1 if NSaaS is using multiple threads per channel, 0
 *                           otherwise.
 * @return                   '0' on success, '-1' on failure.
 */
static inline int __nsaas_channel_dataplane_init(
    uchar_t *shm, size_t shm_size, int is_posix_shm, const char *name,
    size_t nsaas_ring_slot_nr, size_t app_ring_slot_nr, size_t buf_ring_slot_nr,
    size_t buffer_size, int is_multithread) {
  size_t total_size = __nsaas_channel_dataplane_calculate_size(
      nsaas_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      is_posix_shm);
  // Guard against mismatches.
  if (total_size > shm_size || total_size == (size_t)-1) return -1;

  // TODO(ilias): Check that we can always accomodate an NSAAS_MSG_MAX_LEN sized
  // mesage with the number of buffers and buffer_size provided here.

  // Check the memory layout in "nsaas_common.h".
  // Initialize the channel context.
  NSaaSChannelCtx_t *ctx = (NSaaSChannelCtx_t *)shm;
  ctx->version = NSAAS_CHANNEL_VERSION;
  ctx->size = total_size;
  strncpy(ctx->name, name, sizeof(ctx->name));
  ctx->name[sizeof(ctx->name) - 1] = '\0';

  // Initiliaze the ctrl context.
  ctx->ctrl_ctx.req_id = 0;

  // Clear out statatistics.
  ctx->data_ctx.stats_ofs = sizeof(*ctx);
  NSaaSChannelStats_t *stats = (NSaaSChannelStats_t *)__nsaas_channel_mem_ofs(
      ctx, ctx->data_ctx.stats_ofs);
  memset(stats, 0, sizeof(*stats));

  const int kMultiThread = 1;  // Assume the application always multithreaded.

  // Ring0 (ctrl - SQ) follows immediately after the statistics.
  ctx->data_ctx.ctrl_sq_ring_ofs = ctx->data_ctx.stats_ofs + sizeof(*stats);
  int ret = jring_init(__nsaas_channel_ctrl_sq_ring(ctx),
                       NSAAS_CHANNEL_CTRL_SQ_SLOT_NR,
                       sizeof(NSaaSCtrlQueueEntry_t), is_multithread, 0);
  if (ret != 0) return ret;

  // Ring1 (ctrl - CQ) follows immediately after the first ring.
  ctx->data_ctx.ctrl_cq_ring_ofs =
      ctx->data_ctx.ctrl_sq_ring_ofs +
      jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t),
                              NSAAS_CHANNEL_CTRL_SQ_SLOT_NR);
  ret = jring_init(__nsaas_channel_ctrl_cq_ring(ctx),
                   NSAAS_CHANNEL_CTRL_CQ_SLOT_NR, sizeof(NSaaSCtrlQueueEntry_t),
                   0, is_multithread);
  if (ret != 0) return ret;

  // Initialize the NSaaS->Application ring.
  ctx->data_ctx.nsaas_ring_ofs =
      ctx->data_ctx.ctrl_cq_ring_ofs +
      jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t),
                              NSAAS_CHANNEL_CTRL_CQ_SLOT_NR);

  jring_t *nsaas_ring = __nsaas_channel_nsaas_ring(ctx);
  ret = jring_init(nsaas_ring, nsaas_ring_slot_nr, sizeof(NSaaSRingSlot_t),
                   is_multithread, kMultiThread);
  if (ret != 0) return ret;

  // App->NSaaS ring follows immediately after the NSaaS->App ring.
  ctx->data_ctx.app_ring_ofs =
      ctx->data_ctx.nsaas_ring_ofs +
      jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t), nsaas_ring_slot_nr);
  jring_t *app_ring = __nsaas_channel_app_ring(ctx);
  ret = jring_init(app_ring, app_ring_slot_nr, sizeof(NSaaSRingSlot_t),
                   kMultiThread, is_multithread);
  if (ret != 0) return ret;

  // jring_get_buf_ring_size() cannot fail here.
  ctx->data_ctx.buf_ring_ofs =
      ctx->data_ctx.app_ring_ofs +
      jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t), app_ring_slot_nr);

  // Initialize the buffer ring.
  jring_t *buf_ring = __nsaas_channel_buf_ring(ctx);
  ret = jring_init(buf_ring, buf_ring_slot_nr, sizeof(NSaaSRingSlot_t),
                   kMultiThread, kMultiThread);
  if (ret != 0) return ret;

  // Offset in memory channel where the final ring ends (buf_ring).
  size_t buf_ring_end_ofs =
      ctx->data_ctx.buf_ring_ofs +
      jring_get_buf_ring_size(sizeof(NSaaSRingSlot_t), buf_ring_slot_nr);

  // Calculate the actual buffer size (incl. metadata).
  const size_t kTotalBufSize = ROUNDUP_U64_POW2(
      buffer_size + NSAAS_MSGBUF_SPACE_RESERVED + NSAAS_MSGBUF_HEADROOM_MAX);
  // Initialize the buffers. Note that the buffer pool start is aligned to the
  // page_size boundary.
  const size_t kPageSize = is_posix_shm ? getpagesize() : HUGE_PAGE_2M_SIZE;
  ctx->data_ctx.buf_pool_ofs = ALIGN_TO_PAGE_SIZE(buf_ring_end_ofs, kPageSize);
  ctx->data_ctx.buf_pool_mask = buf_ring->capacity;
  ctx->data_ctx.buf_size = kTotalBufSize;
  ctx->data_ctx.buf_mss = buffer_size;

  // Initialize the message header of each buffer.
  for (uint32_t i = 0; i < buf_ring->capacity; i++) {
    NSaaSMsgBuf_t *buf = __nsaas_channel_buf(ctx, i);
    __nsaas_channel_buf_init(buf);
    // The following fields should only be initialized once here.
    *__DECONST(uint32_t *, &buf->magic) = NSAAS_MSGBUF_MAGIC;
    *__DECONST(uint32_t *, &buf->index) = i;
    *__DECONST(uint32_t *, &buf->size) =
        buffer_size + NSAAS_MSGBUF_HEADROOM_MAX;
  }

  // Initialize the buffer index table, and make all these buffers available.
  NSaaSRingSlot_t *buf_index_table =
      (NSaaSRingSlot_t *)malloc(buf_ring->capacity * sizeof(NSaaSRingSlot_t));
  if (buf_index_table == NULL) return -1;

  for (size_t i = 0; i < buf_ring->capacity; i++) buf_index_table[i] = i;

  unsigned int free_space;
  int enqueued = jring_enqueue_bulk(buf_ring, buf_index_table,
                                    buf_ring->capacity, &free_space);
  free(buf_index_table);
  if (((size_t)enqueued != buf_ring->capacity) || (free_space != 0))
    return -1;  // Enqueue has failed.

  // Set the header magic at the end.
  __sync_synchronize();
  ctx->magic = NSAAS_CHANNEL_CTX_MAGIC;
  __sync_synchronize();

  return 0;
}

/**
 * This function creates a POSIX shared memory region to be used as an NSaaS
 * channel. The shared memory region is created with the given name and size and
 * does not support huge pages.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in] channel_size           Size of the usable memory area of the
 * channel in bytes.
 * @param[out] shm_fd             Sets the file descriptor accordingly (-1 on
 *                           failure, >0 on success).
 * @return                   Pointer to channel's memory area on success, NULL
 *                           otherwise.
 */
static inline NSaaSChannelCtx_t *__nsaas_channel_posix_create(
    const char *channel_name, size_t channel_size, int *shm_fd) {
  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  NSaaSChannelCtx_t *channel = NULL;
  int shm_flags, prot_flags;

  // Create the shared memory segment.
  *shm_fd = shm_open(channel_name, O_CREAT | O_EXCL | O_RDWR, 0666);
  if (*shm_fd < 0) {
    perror("shm_open()");
    return NULL;
  }

  // Set the size of the shared memory segment.
  if (ftruncate(*shm_fd, channel_size) == -1) {
    perror("ftruncate()");
    goto fail;
  }

  // Map the shared memory segment into the address space of the process.
  prot_flags = PROT_READ | PROT_WRITE;
  shm_flags = MAP_SHARED | MAP_POPULATE;
  channel = (NSaaSChannelCtx_t *)mmap(NULL, channel_size, prot_flags, shm_flags,
                                      *shm_fd, 0);
  if (channel == MAP_FAILED) {
    perror("mmap()");
    goto fail;
  }

  // Lock the memory segment in RAM.
  if (mlock((void *)channel, channel_size) != 0) {
    perror("mlock()");
    goto fail;
  }

  return channel;

fail:
  if (channel != NULL && channel != MAP_FAILED) munmap(channel, channel_size);

  if (*shm_fd != -1) {
    close(*shm_fd);
    shm_unlink(channel_name);
    *shm_fd = -1;
  }
  return NULL;
}

/**
 * This function creates a POSIX shared memory region to be used as an NSaaS
 * channel. The shared memory region is created with the given name and size and
 * does not support huge pages.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in]  channel_size          Size of the usable memory area of the
 * channel (in bytes, huge pages aligned)
 * @param[out] shm_fd                Sets the file descriptor accordingly (-1 on
 * failure, >0 on success).
 * @return                   Pointer to channel's memory area on success, NULL
 *                           otherwise.
 */
static inline NSaaSChannelCtx_t *__nsaas_channel_hugetlbfs_create(
    const char *channel_name, size_t channel_size, int *shm_fd) {
  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  NSaaSChannelCtx_t *channel = NULL;
  int shm_flags;

  // Check if channel size is huge page aligned.
  if ((channel_size & (HUGE_PAGE_2M_SIZE - 1)) != 0) {
    fprintf(stderr, "Channel size is not huge page aligned.\n");
    return NULL;
  }

  // Create the shared memory segment.
  *shm_fd = memfd_create(channel_name, MFD_HUGETLB);
  if (*shm_fd < 0) {
    return NULL;
  }

  // Set the size of the shared memory segment.
  if (ftruncate(*shm_fd, channel_size) == -1) {
    perror("ftruncate()");
    goto fail;
  }

  // Map the shared memory segment into the address space of the process.
  shm_flags = MAP_SHARED | MAP_POPULATE | MAP_HUGETLB;
  channel = (NSaaSChannelCtx_t *)mmap(
      NULL, channel_size, PROT_READ | PROT_WRITE, shm_flags, *shm_fd, 0);
  if (channel == MAP_FAILED) {
    perror("mmap()");
    goto fail;
  }

  // Lock the memory segment in RAM.
  if (mlock((void *)channel, channel_size) != 0) {
    perror("mlock()");
    goto fail;
  }

  return channel;

fail:
  if (channel != NULL && channel != MAP_FAILED) munmap(channel, channel_size);

  if (*shm_fd != -1) {
    close(*shm_fd);
  }
  *shm_fd = -1;
  return NULL;
}

/**
 * This function unmaps, and destroys an NSaaS channel, releasing the shared
 * memory segment.
 *
 * @param[in] mapped_mem             Pointer to the mapped memory area of the
 * channel.
 * @param[in] mapped_mem_size        Size of the mapped memory area.
 * @param[in] shm_fd                 Opened file descriptor for the shared
 * memory segment.
 * @param[in] is_posix_shm           0 if this is not POSIX shmem segment.
 * @param[in] channel_name           The name of the shared memory segment (if
 * POSIX shmem). Can be NULL if this is not POSIX shmem.
 */
static inline void __nsaas_channel_destroy(void *mapped_mem,
                                           size_t mapped_mem_size, int *shm_fd,
                                           int is_posix_shm,
                                           const char *channel_name) {
  assert(mapped_mem != NULL);
  assert(mapped_mem_size > 0);

  // Unmap the shared memory segment.
  munmap(mapped_mem, mapped_mem_size);
  if (shm_fd != NULL && *shm_fd >= 0) {
    close(*shm_fd);
    *shm_fd = -1;
  }

  if (is_posix_shm) {
    assert(channel_name != NULL);
    shm_unlink(channel_name);
  }
}

/**
 * This function creates a shared memory region to be used as an NSaaS channel.
 *
 * @param[in] channel_name           The name of the shared memory segment.
 * @param[in] nsaas_ring_slot_nr     Number of slots in the NSaaS ring.
 * @param[in] app_ring_slot_nr       Number of slots in the application ring.
 * @param[in] buf_ring_slot_nr       Number of slots in the buffer ring.
 * @param[out] channel_mem_size      (ptr) The real size of the underlying
 * shared memory segment. Can differ from `channel_size` because of alignment
 * reasons (e.g, 4K or 2MB).
 * @param[out] is_posix_shm          (ptr) Set to 1 if this is a POSIX shared
 * memory segment (not backed by hugetlbfs)
 * @param[out] shm_fd                Sets the file descriptor accordingly (-1 on
 *                                   failure, >0 on success).
 * @return                           Pointer to channel's memory area on
 * success, NULL otherwise.
 */
static inline NSaaSChannelCtx_t *__nsaas_channel_create(
    const char *channel_name, size_t nsaas_ring_slot_nr,
    size_t app_ring_slot_nr, size_t buf_ring_slot_nr, size_t buffer_size,
    size_t *channel_mem_size, int *is_posix_shm, int *shm_fd) {
  assert(channel_name != NULL);
  assert(shm_fd != NULL);
  assert(channel_mem_size != NULL);
  assert(is_posix_shm != NULL);
  assert(shm_fd != NULL);
  NSaaSChannelCtx_t *channel;

  *is_posix_shm = 0;
  *channel_mem_size = __nsaas_channel_dataplane_calculate_size(
      nsaas_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      *is_posix_shm);
  // Try creating and mapping a hugetlbfs backed shared memory segment.
  channel =
      __nsaas_channel_hugetlbfs_create(channel_name, *channel_mem_size, shm_fd);
  if (channel != NULL) goto out;

  fprintf(stderr,
          "Failed to create hugetlbfs backed shared memory segment; falling "
          "back to POSIX shm.\n");

  // Hugetlbfs backed shared memory segment creation failed. Fallback to a
  // regular POSIX shm segment.
  *is_posix_shm = 1;
  *channel_mem_size = __nsaas_channel_dataplane_calculate_size(
      nsaas_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size,
      *is_posix_shm);
  channel =
      __nsaas_channel_posix_create(channel_name, *channel_mem_size, shm_fd);
  if (channel != NULL) goto out;

  // Failed to create shared memory segment.
  return NULL;

out:
  // The shared memory segment is created and mapped. Initialize it.
  int ret = __nsaas_channel_dataplane_init(
      (uchar_t *)channel, *channel_mem_size, *is_posix_shm, channel_name,
      nsaas_ring_slot_nr, app_ring_slot_nr, buf_ring_slot_nr, buffer_size, 0);
  if (ret != 0) {
    __nsaas_channel_destroy((void *)channel, *channel_mem_size, shm_fd,
                            *is_posix_shm, channel_name);
    *channel_mem_size = 0;
    // shm_fd is set to -1 by __nsaas_channel_destroy().
    return NULL;
  }

  return channel;
}

static inline __attribute__((always_inline)) uint32_t __nsaas_channel_enqueue(
    const NSaaSChannelCtx_t *ctx, unsigned int n, const NSaaSRingSlot_t *bufs) {
  assert(ctx != NULL);
  jring_t *nsaas_ring = __nsaas_channel_nsaas_ring(ctx);

  return jring_enqueue_bulk(nsaas_ring, bufs, n, NULL);
}

#ifdef __cplusplus
}
#endif
#endif  // SRC_EXT_NSAAS_PRIVATE_H_
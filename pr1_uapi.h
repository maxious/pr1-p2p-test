#ifndef PR1_UAPI_H
#define PR1_UAPI_H

#include <linux/types.h>
#include <drm/drm.h>

#define PR1_DRM_XE_P2P_ALLOC 0x10

struct pr1_drm_xe_p2p_alloc {
	__u64 extensions;
	__u64 size;
	__u32 flags;
	__u32 pad;
	__u64 reserved[2];
};

#define PR1_DRM_IOCTL_XE_P2P_ALLOC \
	DRM_IOW(DRM_COMMAND_BASE + PR1_DRM_XE_P2P_ALLOC, struct pr1_drm_xe_p2p_alloc)

enum pr1_io_uring_regbuf_type {
	PR1_IO_REGBUF_TYPE_EMPTY,
	PR1_IO_REGBUF_TYPE_UADDR,
	PR1_IO_REGBUF_TYPE_DMABUF,
	PR1_IO_REGBUF_TYPE_MAX,
};

struct pr1_io_uring_regbuf_desc {
	__u32 type;
	__u32 flags;
	__u64 size;
	__u64 uaddr;

	__s32 dmabuf_fd;
	__s32 target_fd;
	__u64 __resv[6];
};

#define PR1_IORING_RSRC_UPDATE_EXTENDED (1U << 1)

#define PR1_DRM_XE_GEM_CREATE 0x01
#define PR1_DRM_XE_GEM_MMAP_OFFSET 0x02
#define PR1_DRM_XE_MEM_REGION_CLASS_SYSMEM 0
#define PR1_DRM_XE_MEM_REGION_CLASS_VRAM 1
#define PR1_DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM (1 << 2)
#define PR1_DRM_XE_GEM_CPU_CACHING_WC 2

struct pr1_drm_xe_gem_create {
	__u64 extensions;
	__u64 size;
	__u32 placement;
	__u32 flags;
	__u32 vm_id;
	__u32 handle;
	__u16 cpu_caching;
	__u16 pad[3];
	__u64 reserved[2];
};

#define PR1_DRM_IOCTL_XE_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + PR1_DRM_XE_GEM_CREATE, struct pr1_drm_xe_gem_create)

struct pr1_drm_xe_gem_mmap_offset {
	__u64 extensions;
	__u32 handle;
	__u32 flags;
	__u64 offset;
	__u64 reserved[2];
};

#define PR1_DRM_IOCTL_XE_GEM_MMAP_OFFSET \
	DRM_IOWR(DRM_COMMAND_BASE + PR1_DRM_XE_GEM_MMAP_OFFSET, struct pr1_drm_xe_gem_mmap_offset)

struct pr1_io_uring_rsrc_register {
	__u32 nr;
	__u32 flags;
	__u64 resv2;
	__aligned_u64 data;
	__aligned_u64 tags;
};

struct pr1_io_uring_rsrc_update2 {
	__u32 offset;
	union {
		__u32 resv;
		__u32 flags;
	};
	__aligned_u64 data;
	__aligned_u64 tags;
	__u32 nr;
	__u32 resv2;
};

#endif

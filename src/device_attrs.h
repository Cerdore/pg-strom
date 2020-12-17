/*
 * all the valid device attributes are introduced using
 *
 * DEV_ATTR(label, class, is_minor, desc)
 *
 * caller has to define the DEV_ATTR according to the context
 *
 * class is one of INT, BYTES, KB, KHZ, COMPUTEMODE, BOOL
 */
DEV_ATTR(MAX_THREADS_PER_BLOCK, INT, 0, "Maximum number of threads per block")
DEV_ATTR(MAX_BLOCK_DIM_X, INT, 0, "Maximum block dimension X")
DEV_ATTR(MAX_BLOCK_DIM_Y, INT, 0, "Maximum block dimension Y")
DEV_ATTR(MAX_BLOCK_DIM_Z, INT, 0, "Maximum block dimension Z")
DEV_ATTR(MAX_GRID_DIM_X, INT, 0, "Maximum grid dimension X")
DEV_ATTR(MAX_GRID_DIM_Y, INT, 0, "Maximum grid dimension Y")
DEV_ATTR(MAX_GRID_DIM_Z, INT, 0, "Maximum grid dimension Z")
DEV_ATTR(MAX_SHARED_MEMORY_PER_BLOCK, BYTES, 0, "Maximum shared memory available per block in bytes")
DEV_ATTR(TOTAL_CONSTANT_MEMORY, BYTES, 0, "Memory available on device for __constant__ variables in a CUDA C kernel in bytes")
DEV_ATTR(WARP_SIZE, INT, 0, "Warp size in threads")
DEV_ATTR(MAX_PITCH, BYTES, 0, "Maximum pitch in bytes allowed by memory copies")
DEV_ATTR(MAX_REGISTERS_PER_BLOCK, INT, 0, "Maximum number of 32-bit registers available per block")
DEV_ATTR(CLOCK_RATE, KHZ, 0, "Typical clock frequency in kilohertz")
DEV_ATTR(TEXTURE_ALIGNMENT, INT, 1, "Alignment requirement for textures")
DEV_ATTR(MULTIPROCESSOR_COUNT, INT, 0, "Number of multiprocessors on device")
DEV_ATTR(KERNEL_EXEC_TIMEOUT, INT, 0, "Specifies whether there is a run time limit on kernels")
DEV_ATTR(INTEGRATED, INT, 0, "Device is integrated with host memory")
DEV_ATTR(CAN_MAP_HOST_MEMORY, INT, 0, "Device can map host memory into CUDA address space")
DEV_ATTR(COMPUTE_MODE, COMPUTEMODE, 0, "Compute mode (See ::CUcomputemode for details)")
DEV_ATTR(MAXIMUM_TEXTURE1D_WIDTH, INT, 1, "Maximum 1D texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_WIDTH, INT, 1, "Maximum 2D texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_HEIGHT, INT, 1, "Maximum 2D texture height")
DEV_ATTR(MAXIMUM_TEXTURE3D_WIDTH, INT, 1, "Maximum 3D texture width")
DEV_ATTR(MAXIMUM_TEXTURE3D_HEIGHT, INT, 1, "Maximum 3D texture height")
DEV_ATTR(MAXIMUM_TEXTURE3D_DEPTH, INT, 1, "Maximum 3D texture depth")
DEV_ATTR(MAXIMUM_TEXTURE2D_LAYERED_WIDTH, INT, 1, "Maximum 2D layered texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_LAYERED_HEIGHT, INT, 1, "Maximum 2D layered texture height")
DEV_ATTR(MAXIMUM_TEXTURE2D_LAYERED_LAYERS, INT, 1, "Maximum layers in a 2D layered texture")
DEV_ATTR(SURFACE_ALIGNMENT, INT, 1, "Alignment requirement for surfaces")
DEV_ATTR(CONCURRENT_KERNELS, BOOL, 0, "Device can possibly execute multiple kernels concurrently")
DEV_ATTR(ECC_ENABLED, BOOL, 0, "Device has ECC support enabled")
DEV_ATTR(PCI_BUS_ID, INT, 0, "PCI bus ID of the device")
DEV_ATTR(PCI_DEVICE_ID, INT, 0, "PCI device ID of the device")
DEV_ATTR(TCC_DRIVER, BOOL, 0, "Device is using TCC driver model")
DEV_ATTR(MEMORY_CLOCK_RATE, KHZ, 0, "Peak memory clock frequency in kilohertz")
DEV_ATTR(GLOBAL_MEMORY_BUS_WIDTH, BITS, 0, "Global memory bus width in bits")
DEV_ATTR(L2_CACHE_SIZE, BYTES, 0, "Size of L2 cache in bytes")
DEV_ATTR(MAX_THREADS_PER_MULTIPROCESSOR, INT, 0, "Maximum resident threads per multiprocessor")
DEV_ATTR(ASYNC_ENGINE_COUNT, INT, 0, "Number of asynchronous engines")
DEV_ATTR(UNIFIED_ADDRESSING, BOOL, 0, "Device shares a unified address space with the host")
DEV_ATTR(MAXIMUM_TEXTURE1D_LAYERED_WIDTH, INT, 1, "Maximum 1D layered texture width")
DEV_ATTR(MAXIMUM_TEXTURE1D_LAYERED_LAYERS, INT, 1, "Maximum layers in a 1D layered texture")
DEV_ATTR(MAXIMUM_TEXTURE2D_GATHER_WIDTH, INT, 1, "Maximum 2D texture width if CUDA_ARRAY3D_TEXTURE_GATHER is set")
DEV_ATTR(MAXIMUM_TEXTURE2D_GATHER_HEIGHT, INT, 1, "Maximum 2D texture height if CUDA_ARRAY3D_TEXTURE_GATHER is set")
DEV_ATTR(MAXIMUM_TEXTURE3D_WIDTH_ALTERNATE, INT, 1, "Alternate maximum 3D texture width")
DEV_ATTR(MAXIMUM_TEXTURE3D_HEIGHT_ALTERNATE, INT, 1, "Alternate maximum 3D texture height")
DEV_ATTR(MAXIMUM_TEXTURE3D_DEPTH_ALTERNATE, INT, 1, "Alternate maximum 3D texture depth")
DEV_ATTR(PCI_DOMAIN_ID, INT, 0, "PCI domain ID of the device")
DEV_ATTR(TEXTURE_PITCH_ALIGNMENT, INT, 1, "Pitch alignment requirement for textures")
DEV_ATTR(MAXIMUM_TEXTURECUBEMAP_WIDTH, INT, 1, "Maximum cubemap texture width/height")
DEV_ATTR(MAXIMUM_TEXTURECUBEMAP_LAYERED_WIDTH, INT, 1, "Maximum cubemap layered texture width/height")
DEV_ATTR(MAXIMUM_TEXTURECUBEMAP_LAYERED_LAYERS, INT, 1, "Maximum layers in a cubemap layered texture")
DEV_ATTR(MAXIMUM_SURFACE1D_WIDTH, INT, 1, "Maximum 1D surface width")
DEV_ATTR(MAXIMUM_SURFACE2D_WIDTH, INT, 1, "Maximum 2D surface width")
DEV_ATTR(MAXIMUM_SURFACE2D_HEIGHT, INT, 1, "Maximum 2D surface height")
DEV_ATTR(MAXIMUM_SURFACE3D_WIDTH, INT, 1, "Maximum 3D surface width")
DEV_ATTR(MAXIMUM_SURFACE3D_HEIGHT, INT, 1, "Maximum 3D surface height")
DEV_ATTR(MAXIMUM_SURFACE3D_DEPTH, INT, 1, "Maximum 3D surface depth")
DEV_ATTR(MAXIMUM_SURFACE1D_LAYERED_WIDTH, INT, 1, "Maximum 1D layered surface width")
DEV_ATTR(MAXIMUM_SURFACE1D_LAYERED_LAYERS, INT, 1, "Maximum layers in a 1D layered surface")
DEV_ATTR(MAXIMUM_SURFACE2D_LAYERED_WIDTH, INT, 1, "Maximum 2D layered surface width")
DEV_ATTR(MAXIMUM_SURFACE2D_LAYERED_HEIGHT, INT, 1, "Maximum 2D layered surface height")
DEV_ATTR(MAXIMUM_SURFACE2D_LAYERED_LAYERS, INT, 1, "Maximum layers in a 2D layered surface")
DEV_ATTR(MAXIMUM_SURFACECUBEMAP_WIDTH, INT, 1, "Maximum cubemap surface width")
DEV_ATTR(MAXIMUM_SURFACECUBEMAP_LAYERED_WIDTH, INT, 1, "Maximum cubemap layered surface width")
DEV_ATTR(MAXIMUM_SURFACECUBEMAP_LAYERED_LAYERS, INT, 1, "Maximum layers in a cubemap layered surface")
DEV_ATTR(MAXIMUM_TEXTURE1D_LINEAR_WIDTH, INT, 1, "Maximum 1D linear texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_LINEAR_WIDTH, INT, 1, "Maximum 2D linear texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_LINEAR_HEIGHT, INT, 1, "Maximum 2D linear texture height")
DEV_ATTR(MAXIMUM_TEXTURE2D_LINEAR_PITCH, BYTES, 1, "Maximum 2D linear texture pitch in bytes")
DEV_ATTR(MAXIMUM_TEXTURE2D_MIPMAPPED_WIDTH, INT, 1, "Maximum mipmapped 2D texture width")
DEV_ATTR(MAXIMUM_TEXTURE2D_MIPMAPPED_HEIGHT, INT, 1, "Maximum mipmapped 2D texture height")
DEV_ATTR(COMPUTE_CAPABILITY_MAJOR, INT, 0, "Major compute capability version number")
DEV_ATTR(COMPUTE_CAPABILITY_MINOR, INT, 0, "Minor compute capability version number")
DEV_ATTR(MAXIMUM_TEXTURE1D_MIPMAPPED_WIDTH, INT, 1, "Maximum mipmapped 1D texture width")
DEV_ATTR(STREAM_PRIORITIES_SUPPORTED, BOOL, 0, "Device supports stream priorities")
DEV_ATTR(GLOBAL_L1_CACHE_SUPPORTED, BOOL, 0, "Device supports caching globals in L1")
DEV_ATTR(LOCAL_L1_CACHE_SUPPORTED, BOOL, 0, "Device supports caching locals in L1")
DEV_ATTR(MAX_SHARED_MEMORY_PER_MULTIPROCESSOR, BYTES, 0, "Maximum shared memory available per multiprocessor in bytes")
DEV_ATTR(MAX_REGISTERS_PER_MULTIPROCESSOR, INT, 0, "Maximum number of 32-bit registers available per multiprocessor")
DEV_ATTR(MANAGED_MEMORY, BOOL, 0, "Device can allocate managed memory on this system")
DEV_ATTR(MULTI_GPU_BOARD, BOOL, 0, "Device is on a multi-GPU board")
DEV_ATTR(MULTI_GPU_BOARD_GROUP_ID, INT, 0, "Unique id for a group of devices on the same multi-GPU board")
#if CUDA_VERSION >= 8000
DEV_ATTR(HOST_NATIVE_ATOMIC_SUPPORTED, BOOL, 0, "Link between the device and the host supports native atomic operations")
DEV_ATTR(SINGLE_TO_DOUBLE_PRECISION_PERF_RATIO, INT, 0, "Ratio of single precision performance (in floating-point operations per second) to double precision performance")
DEV_ATTR(PAGEABLE_MEMORY_ACCESS, BOOL, 0, "Device supports coherently accessing pageable memory without calling cudaHostRegister on it")
DEV_ATTR(CONCURRENT_MANAGED_ACCESS, BOOL, 0, "Device can coherently access managed memory concurrently with the CPU")
DEV_ATTR(COMPUTE_PREEMPTION_SUPPORTED, BOOL, 0, "Device supports compute preemption")
DEV_ATTR(CAN_USE_HOST_POINTER_FOR_REGISTERED_MEM, BOOL, 0, "Device can access host registered memory at the same virtual address as the CPU")
#if CUDA_VERSION >= 9000
DEV_ATTR(CAN_USE_STREAM_MEM_OPS, BOOL, 0, "cuStreamBatchMemOp and related APIs are supported")
DEV_ATTR(CAN_USE_64_BIT_STREAM_MEM_OPS, BOOL, 1, "64-bit operations are supported in ::cuStreamBatchMemOp and related APIs")
DEV_ATTR(CAN_USE_STREAM_WAIT_VALUE_NOR, BOOL, 1, "CU_STREAM_WAIT_VALUE_NOR is supported")
DEV_ATTR(COOPERATIVE_LAUNCH, BOOL, 1, "Device supports launching cooperative kernels via cuLaunchCooperativeKernel")
DEV_ATTR(COOPERATIVE_MULTI_DEVICE_LAUNCH, BOOL, 1, "Device can participate in cooperative kernels launched via cuLaunchCooperativeKernelMultiDevice")
DEV_ATTR(MAX_SHARED_MEMORY_PER_BLOCK_OPTIN, INT, 1, "Maximum optin shared memory per block")
#if CUDA_VERSION >= 9020
DEV_ATTR(HOST_REGISTER_SUPPORTED, BOOL, 1, "Device supports host memory registration")
DEV_ATTR(PAGEABLE_MEMORY_ACCESS_USES_HOST_PAGE_TABLES, BOOL, 1, "Device accesses pageable memory via the host's page tables")
DEV_ATTR(DIRECT_MANAGED_MEM_ACCESS_FROM_HOST, BOOL, 0, "The host can directly access managed memory on the device without migration")
#if CUDA_VERSION >= 10020
DEV_ATTR(VIRTUAL_ADDRESS_MANAGEMENT_SUPPORTED, BOOL, 0, "Device supports virtual address management APIs")
DEV_ATTR(HANDLE_TYPE_POSIX_FILE_DESCRIPTOR_SUPPORTED, BOOL, 0, "Device supports exporting memory to a posix file descriptor")
DEV_ATTR(HANDLE_TYPE_WIN32_HANDLE_SUPPORTED, BOOL, 1, "Device supports exporting memory to a Win32 NT handle")
DEV_ATTR(HANDLE_TYPE_WIN32_KMT_HANDLE_SUPPORTED, BOOL, 1, "Device supports exporting memory to a Win32 KMT handle")
#if CUDA_VERSION >= 11000
DEV_ATTR(MAX_BLOCKS_PER_MULTIPROCESSOR, INT, 0, "Maximum number of blocks per multiprocessor")
DEV_ATTR(GENERIC_COMPRESSION_SUPPORTED, BOOL, 1, "Device supports compression of memory")
DEV_ATTR(MAX_PERSISTING_L2_CACHE_SIZE, INT, 0, "Device's maximum L2 persisting lines capacity setting in bytes")
DEV_ATTR(MAX_ACCESS_POLICY_WINDOW_SIZE, INT, 1, "The maximum value of CUaccessPolicyWindow::num_bytes")
DEV_ATTR(GPU_DIRECT_RDMA_WITH_CUDA_VMM_SUPPORTED, BOOL, 0, "Device supports specifying the GPUDirect RDMA flag with ::cuMemCreate")
DEV_ATTR(RESERVED_SHARED_MEMORY_PER_BLOCK, INT, 0, "Shared memory reserved by CUDA driver per block in bytes")
#if CUDA_VERSION >= 11010
DEV_ATTR(SPARSE_CUDA_ARRAY_SUPPORTED, BOOL, 1, "Device supports sparse CUDA arrays and sparse CUDA mipmapped arrays")
DEV_ATTR(READ_ONLY_HOST_REGISTER_SUPPORTED, BOOL, 1, "Device supports using the ::cuMemHostRegister flag CU_MEMHOSTERGISTER_READ_ONLY to register memory that must be mapped as read-only to the GPU")
#if CUDA_VERSION >= 11020
DEV_ATTR(TIMELINE_SEMAPHORE_INTEROP_SUPPORTED, BOOL, 1, "External timeline semaphore interop is supported on the device")
DEV_ATTR(MEMORY_POOLS_SUPPORTED, BOOL, 0, "Device supports using the ::cuMemAllocAsync and ::cuMemPool family of APIs")
#endif	/* CUDA 11.2 */
#endif	/* CUDA 11.1 */
#endif	/* CUDA 11.0 */
#endif	/* CUDA 10.2 */
#endif	/* CUDA 9.2 */
#endif	/* CUDA 9.0 */
#endif	/* CUDA 8.0 */

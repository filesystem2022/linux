/*
 * xsave/xrstor support.
 *
 * Author: Suresh Siddha <suresh.b.siddha@intel.com>
 */
#include <linux/compat.h>
#include <linux/cpu.h>
#include <linux/mman.h>
#include <linux/pkeys.h>

#include <asm/fpu/api.h>
#include <asm/fpu/internal.h>
#include <asm/fpu/signal.h>
#include <asm/fpu/regset.h>
#include <asm/fpu/xstate.h>

#include <asm/tlbflush.h>
#include <asm/cpufeature.h>

/*
 * Although we spell it out in here, the Processor Trace
 * xfeature is completely unused.  We use other mechanisms
 * to save/restore PT state in Linux.
 */
static const char *xfeature_names[] =
{
	"x87 floating point registers"	,
	"SSE registers"			,
	"AVX registers"			,
	"MPX bounds registers"		,
	"MPX CSR"			,
	"AVX-512 opmask"		,
	"AVX-512 Hi256"			,
	"AVX-512 ZMM_Hi256"		,
	"Processor Trace (unused)"	,
	"Protection Keys User registers",
	"PASID state",
	"unknown xstate feature"	,
};

static short xsave_cpuid_features[] __initdata = {
	X86_FEATURE_FPU,
	X86_FEATURE_XMM,
	X86_FEATURE_AVX,
	X86_FEATURE_MPX,
	X86_FEATURE_MPX,
	X86_FEATURE_AVX512F,
	X86_FEATURE_AVX512F,
	X86_FEATURE_AVX512F,
	X86_FEATURE_INTEL_PT,
	X86_FEATURE_PKU,
	X86_FEATURE_ENQCMD,
};

/*
 * This represents the full set of bits that should ever be set in a kernel
 * XSAVE buffer, both supervisor and user xstates.
 */
u64 xfeatures_mask_all __read_mostly;

static unsigned int xstate_offsets[XFEATURE_MAX] = { [ 0 ... XFEATURE_MAX - 1] = -1};
static unsigned int xstate_sizes[XFEATURE_MAX]   = { [ 0 ... XFEATURE_MAX - 1] = -1};
static unsigned int xstate_comp_offsets[XFEATURE_MAX] = { [ 0 ... XFEATURE_MAX - 1] = -1};
static unsigned int xstate_supervisor_only_offsets[XFEATURE_MAX] = { [ 0 ... XFEATURE_MAX - 1] = -1};

/*
 * The XSAVE area of kernel can be in standard or compacted format;
 * it is always in standard format for user mode. This is the user
 * mode standard format size used for signal and ptrace frames.
 */
unsigned int fpu_user_xstate_size;

/*
 * Clear all of the X86_FEATURE_* bits that are unavailable
 * when the CPU has no XSAVE support.
 */
void fpu__xstate_clear_all_cpu_caps(void)
{
	setup_clear_cpu_cap(X86_FEATURE_XSAVE);
}

/*
 * Return whether the system supports a given xfeature.
 *
 * Also return the name of the (most advanced) feature that the caller requested:
 */
int cpu_has_xfeatures(u64 xfeatures_needed, const char **feature_name)
{
	u64 xfeatures_missing = xfeatures_needed & ~xfeatures_mask_all;

	if (unlikely(feature_name)) {
		long xfeature_idx, max_idx;
		u64 xfeatures_print;
		/*
		 * So we use FLS here to be able to print the most advanced
		 * feature that was requested but is missing. So if a driver
		 * asks about "XFEATURE_MASK_SSE | XFEATURE_MASK_YMM" we'll print the
		 * missing AVX feature - this is the most informative message
		 * to users:
		 */
		if (xfeatures_missing)
			xfeatures_print = xfeatures_missing;
		else
			xfeatures_print = xfeatures_needed;

		xfeature_idx = fls64(xfeatures_print)-1;
		max_idx = ARRAY_SIZE(xfeature_names)-1;
		xfeature_idx = min(xfeature_idx, max_idx);

		*feature_name = xfeature_names[xfeature_idx];
	}

	if (xfeatures_missing)
		return 0;

	return 1;
}
EXPORT_SYMBOL_GPL(cpu_has_xfeatures);

static bool xfeature_is_supervisor(int xfeature_nr)
{
	/*
	 * Extended State Enumeration Sub-leaves (EAX = 0DH, ECX = n, n > 1)
	 * returns ECX[0] set to (1) for a supervisor state, and cleared (0)
	 * for a user state.
	 */
	u32 eax, ebx, ecx, edx;

	cpuid_count(XSTATE_CPUID, xfeature_nr, &eax, &ebx, &ecx, &edx);
	return ecx & 1;
}

/*
 * When executing XSAVEOPT (or other optimized XSAVE instructions), if
 * a processor implementation detects that an FPU state component is still
 * (or is again) in its initialized state, it may clear the corresponding
 * bit in the header.xfeatures field, and can skip the writeout of registers
 * to the corresponding memory layout.
 *
 * This means that when the bit is zero, the state component might still contain
 * some previous - non-initialized register state.
 *
 * Before writing xstate information to user-space we sanitize those components,
 * to always ensure that the memory layout of a feature will be in the init state
 * if the corresponding header bit is zero. This is to ensure that user-space doesn't
 * see some stale state in the memory layout during signal handling, debugging etc.
 */
void fpstate_sanitize_xstate(struct fpu *fpu)
{
	struct fxregs_state *fx = &fpu->state.fxsave;
	int feature_bit;
	u64 xfeatures;

	if (!use_xsaveopt())
		return;

	xfeatures = fpu->state.xsave.header.xfeatures;

	/*
	 * None of the feature bits are in init state. So nothing else
	 * to do for us, as the memory layout is up to date.
	 */
	if ((xfeatures & xfeatures_mask_all) == xfeatures_mask_all)
		return;

	/*
	 * FP is in init state
	 */
	if (!(xfeatures & XFEATURE_MASK_FP)) {
		fx->cwd = 0x37f;
		fx->swd = 0;
		fx->twd = 0;
		fx->fop = 0;
		fx->rip = 0;
		fx->rdp = 0;
		memset(&fx->st_space[0], 0, 128);
	}

	/*
	 * SSE is in init state
	 */
	if (!(xfeatures & XFEATURE_MASK_SSE))
		memset(&fx->xmm_space[0], 0, 256);

	/*
	 * First two features are FPU and SSE, which above we handled
	 * in a special way already:
	 */
	feature_bit = 0x2;
	xfeatures = (xfeatures_mask_user() & ~xfeatures) >> 2;

	/*
	 * Update all the remaining memory layouts according to their
	 * standard xstate layout, if their header bit is in the init
	 * state:
	 */
	while (xfeatures) {
		if (xfeatures & 0x1) {
			int offset = xstate_comp_offsets[feature_bit];
			int size = xstate_sizes[feature_bit];

			memcpy((void *)fx + offset,
			       (void *)&init_fpstate.xsave + offset,
			       size);
		}

		xfeatures >>= 1;
		feature_bit++;
	}
}

/*
 * Enable the extended processor state save/restore feature.
 * Called once per CPU onlining.
 */
void fpu__init_cpu_xstate(void)
{
	u64 unsup_bits;

	if (!boot_cpu_has(X86_FEATURE_XSAVE) || !xfeatures_mask_all)
		return;
	/*
	 * Unsupported supervisor xstates should not be found in
	 * the xfeatures mask.
	 */
	unsup_bits = xfeatures_mask_all & XFEATURE_MASK_SUPERVISOR_UNSUPPORTED;
	WARN_ONCE(unsup_bits, "x86/fpu: Found unsupported supervisor xstates: 0x%llx\n",
		  unsup_bits);

	xfeatures_mask_all &= ~XFEATURE_MASK_SUPERVISOR_UNSUPPORTED;

	cr4_set_bits(X86_CR4_OSXSAVE);

	/*
	 * XCR_XFEATURE_ENABLED_MASK (aka. XCR0) sets user features
	 * managed by XSAVE{C, OPT, S} and XRSTOR{S}.  Only XSAVE user
	 * states can be set here.
	 */
	xsetbv(XCR_XFEATURE_ENABLED_MASK, xfeatures_mask_user());

	/*
	 * MSR_IA32_XSS sets supervisor states managed by XSAVES.
	 */
	if (boot_cpu_has(X86_FEATURE_XSAVES)) {
		wrmsrl(MSR_IA32_XSS, xfeatures_mask_supervisor() |
				     xfeatures_mask_dynamic());
	}
}

static bool xfeature_enabled(enum xfeature xfeature)
{
	return xfeatures_mask_all & BIT_ULL(xfeature);
}

/*
 * Record the offsets and sizes of various xstates contained
 * in the XSAVE state memory layout.
 */
static void __init setup_xstate_features(void)
{
	u32 eax, ebx, ecx, edx, i;
	/* start at the beginnning of the "extended state" */
	unsigned int last_good_offset = offsetof(struct xregs_state,
						 extended_state_area);
	/*
	 * The FP xstates and SSE xstates are legacy states. They are always
	 * in the fixed offsets in the xsave area in either compacted form
	 * or standard form.
	 */
	xstate_offsets[XFEATURE_FP]	= 0;
	xstate_sizes[XFEATURE_FP]	= offsetof(struct fxregs_state,
						   xmm_space);

	xstate_offsets[XFEATURE_SSE]	= xstate_sizes[XFEATURE_FP];
	xstate_sizes[XFEATURE_SSE]	= FIELD_SIZEOF(struct fxregs_state,
						       xmm_space);

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		if (!xfeature_enabled(i))
			continue;

		cpuid_count(XSTATE_CPUID, i, &eax, &ebx, &ecx, &edx);

		xstate_sizes[i] = eax;

		/*
		 * If an xfeature is supervisor state, the offset in EBX is
		 * invalid, leave it to -1.
		 */
		if (xfeature_is_supervisor(i))
			continue;

		xstate_offsets[i] = ebx;

		/*
		 * In our xstate size checks, we assume that the highest-numbered
		 * xstate feature has the highest offset in the buffer.  Ensure
		 * it does.
		 */
		WARN_ONCE(last_good_offset > xstate_offsets[i],
			  "x86/fpu: misordered xstate at %d\n", last_good_offset);

		last_good_offset = xstate_offsets[i];
	}
}

static void __init print_xstate_feature(u64 xstate_mask)
{
	const char *feature_name;

	if (cpu_has_xfeatures(xstate_mask, &feature_name))
		pr_info("x86/fpu: Supporting XSAVE feature 0x%03Lx: '%s'\n", xstate_mask, feature_name);
}

/*
 * Print out all the supported xstate features:
 */
static void __init print_xstate_features(void)
{
	print_xstate_feature(XFEATURE_MASK_FP);
	print_xstate_feature(XFEATURE_MASK_SSE);
	print_xstate_feature(XFEATURE_MASK_YMM);
	print_xstate_feature(XFEATURE_MASK_BNDREGS);
	print_xstate_feature(XFEATURE_MASK_BNDCSR);
	print_xstate_feature(XFEATURE_MASK_OPMASK);
	print_xstate_feature(XFEATURE_MASK_ZMM_Hi256);
	print_xstate_feature(XFEATURE_MASK_Hi16_ZMM);
	print_xstate_feature(XFEATURE_MASK_PKRU);
	print_xstate_feature(XFEATURE_MASK_PASID);
}

/*
 * This check is important because it is easy to get XSTATE_*
 * confused with XSTATE_BIT_*.
 */
#define CHECK_XFEATURE(nr) do {		\
	WARN_ON(nr < FIRST_EXTENDED_XFEATURE);	\
	WARN_ON(nr >= XFEATURE_MAX);	\
} while (0)

/*
 * We could cache this like xstate_size[], but we only use
 * it here, so it would be a waste of space.
 */
static int xfeature_is_aligned(int xfeature_nr)
{
	u32 eax, ebx, ecx, edx;

	CHECK_XFEATURE(xfeature_nr);

	if (!xfeature_enabled(xfeature_nr)) {
		WARN_ONCE(1, "Checking alignment of disabled xfeature %d\n",
			  xfeature_nr);
		return 0;
	}

	cpuid_count(XSTATE_CPUID, xfeature_nr, &eax, &ebx, &ecx, &edx);
	/*
	 * The value returned by ECX[1] indicates the alignment
	 * of state component 'i' when the compacted format
	 * of the extended region of an XSAVE area is used:
	 */
	return !!(ecx & 2);
}

/*
 * This function sets up offsets and sizes of all extended states in
 * xsave area. This supports both standard format and compacted format
 * of the xsave area.
 */
static void __init setup_xstate_comp_offsets(void)
{
	unsigned int next_offset;
	int i;

	/*
	 * The FP xstates and SSE xstates are legacy states. They are always
	 * in the fixed offsets in the xsave area in either compacted form
	 * or standard form.
	 */
	xstate_comp_offsets[XFEATURE_FP] = 0;
	xstate_comp_offsets[XFEATURE_SSE] = offsetof(struct fxregs_state,
						     xmm_space);

	if (!boot_cpu_has(X86_FEATURE_XSAVES)) {
		for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
			if (xfeature_enabled(i))
				xstate_comp_offsets[i] = xstate_offsets[i];
		}
		return;
	}

	next_offset = FXSAVE_SIZE + XSAVE_HDR_SIZE;

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		if (!xfeature_enabled(i))
			continue;

		if (xfeature_is_aligned(i))
			next_offset = ALIGN(next_offset, 64);

		xstate_comp_offsets[i] = next_offset;
		next_offset += xstate_sizes[i];
	}
}

/*
 * Setup offsets of a supervisor-state-only XSAVES buffer:
 *
 * The offsets stored in xstate_comp_offsets[] only work for one specific
 * value of the Requested Feature BitMap (RFBM).  In cases where a different
 * RFBM value is used, a different set of offsets is required.  This set of
 * offsets is for when RFBM=xfeatures_mask_supervisor().
 */
static void __init setup_supervisor_only_offsets(void)
{
	unsigned int next_offset;
	int i;

	next_offset = FXSAVE_SIZE + XSAVE_HDR_SIZE;

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		if (!xfeature_enabled(i) || !xfeature_is_supervisor(i))
			continue;

		if (xfeature_is_aligned(i))
			next_offset = ALIGN(next_offset, 64);

		xstate_supervisor_only_offsets[i] = next_offset;
		next_offset += xstate_sizes[i];
	}
}

/*
 * Print out xstate component offsets and sizes
 */
static void __init print_xstate_offset_size(void)
{
	int i;

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		if (!xfeature_enabled(i))
			continue;
		pr_info("x86/fpu: xstate_offset[%d]: %4d, xstate_sizes[%d]: %4d\n",
			 i, xstate_comp_offsets[i], i, xstate_sizes[i]);
	}
}

/*
 * setup the xstate image representing the init state
 */
static void __init setup_init_fpu_buf(void)
{
	static int on_boot_cpu __initdata = 1;

	WARN_ON_FPU(!on_boot_cpu);
	on_boot_cpu = 0;

	if (!boot_cpu_has(X86_FEATURE_XSAVE))
		return;

	setup_xstate_features();
	print_xstate_features();

	if (boot_cpu_has(X86_FEATURE_XSAVES))
		init_fpstate.xsave.header.xcomp_bv = XCOMP_BV_COMPACTED_FORMAT |
						     xfeatures_mask_all;

	/*
	 * Init all the features state with header.xfeatures being 0x0
	 */
	copy_kernel_to_xregs_booting(&init_fpstate.xsave);

	/*
	 * Dump the init state again. This is to identify the init state
	 * of any feature which is not represented by all zero's.
	 */
	copy_xregs_to_kernel_booting(&init_fpstate.xsave);
}

static int xfeature_uncompacted_offset(int xfeature_nr)
{
	u32 eax, ebx, ecx, edx;

	/*
	 * Only XSAVES supports supervisor states and it uses compacted
	 * format. Checking a supervisor state's uncompacted offset is
	 * an error.
	 */
	if (XFEATURE_MASK_SUPERVISOR_ALL & BIT_ULL(xfeature_nr)) {
		WARN_ONCE(1, "No fixed offset for xstate %d\n", xfeature_nr);
		return -1;
	}

	CHECK_XFEATURE(xfeature_nr);
	cpuid_count(XSTATE_CPUID, xfeature_nr, &eax, &ebx, &ecx, &edx);
	return ebx;
}

int xfeature_size(int xfeature_nr)
{
	u32 eax, ebx, ecx, edx;

	CHECK_XFEATURE(xfeature_nr);
	cpuid_count(XSTATE_CPUID, xfeature_nr, &eax, &ebx, &ecx, &edx);
	return eax;
}

/*
 * 'XSAVES' implies two different things:
 * 1. saving of supervisor/system state
 * 2. using the compacted format
 *
 * Use this function when dealing with the compacted format so
 * that it is obvious which aspect of 'XSAVES' is being handled
 * by the calling code.
 */
int using_compacted_format(void)
{
	return boot_cpu_has(X86_FEATURE_XSAVES);
}

/* Validate an xstate header supplied by userspace (ptrace or sigreturn) */
int validate_user_xstate_header(const struct xstate_header *hdr)
{
	/* No unknown or supervisor features may be set */
	if (hdr->xfeatures & ~xfeatures_mask_user())
		return -EINVAL;

	/* Userspace must use the uncompacted format */
	if (hdr->xcomp_bv)
		return -EINVAL;

	/*
	 * If 'reserved' is shrunken to add a new field, make sure to validate
	 * that new field here!
	 */
	BUILD_BUG_ON(sizeof(hdr->reserved) != 48);

	/* No reserved bits may be set */
	if (memchr_inv(hdr->reserved, 0, sizeof(hdr->reserved)))
		return -EINVAL;

	return 0;
}

static void __xstate_dump_leaves(void)
{
	int i;
	u32 eax, ebx, ecx, edx;
	static int should_dump = 1;

	if (!should_dump)
		return;
	should_dump = 0;
	/*
	 * Dump out a few leaves past the ones that we support
	 * just in case there are some goodies up there
	 */
	for (i = 0; i < XFEATURE_MAX + 10; i++) {
		cpuid_count(XSTATE_CPUID, i, &eax, &ebx, &ecx, &edx);
		pr_warn("CPUID[%02x, %02x]: eax=%08x ebx=%08x ecx=%08x edx=%08x\n",
			XSTATE_CPUID, i, eax, ebx, ecx, edx);
	}
}

#define XSTATE_WARN_ON(x) do {							\
	if (WARN_ONCE(x, "XSAVE consistency problem, dumping leaves")) {	\
		__xstate_dump_leaves();						\
	}									\
} while (0)

#define XCHECK_SZ(sz, nr, nr_macro, __struct) do {			\
	if ((nr == nr_macro) &&						\
	    WARN_ONCE(sz != sizeof(__struct),				\
		"%s: struct is %zu bytes, cpu state %d bytes\n",	\
		__stringify(nr_macro), sizeof(__struct), sz)) {		\
		__xstate_dump_leaves();					\
	}								\
} while (0)

/*
 * We have a C struct for each 'xstate'.  We need to ensure
 * that our software representation matches what the CPU
 * tells us about the state's size.
 */
static void check_xstate_against_struct(int nr)
{
	/*
	 * Ask the CPU for the size of the state.
	 */
	int sz = xfeature_size(nr);
	/*
	 * Match each CPU state with the corresponding software
	 * structure.
	 */
	XCHECK_SZ(sz, nr, XFEATURE_YMM,       struct ymmh_struct);
	XCHECK_SZ(sz, nr, XFEATURE_BNDREGS,   struct mpx_bndreg_state);
	XCHECK_SZ(sz, nr, XFEATURE_BNDCSR,    struct mpx_bndcsr_state);
	XCHECK_SZ(sz, nr, XFEATURE_OPMASK,    struct avx_512_opmask_state);
	XCHECK_SZ(sz, nr, XFEATURE_ZMM_Hi256, struct avx_512_zmm_uppers_state);
	XCHECK_SZ(sz, nr, XFEATURE_Hi16_ZMM,  struct avx_512_hi16_state);
	XCHECK_SZ(sz, nr, XFEATURE_PKRU,      struct pkru_state);
	XCHECK_SZ(sz, nr, XFEATURE_PASID,     struct ia32_pasid_state);

	/*
	 * Make *SURE* to add any feature numbers in below if
	 * there are "holes" in the xsave state component
	 * numbers.
	 */
	if ((nr < XFEATURE_YMM) ||
	    (nr >= XFEATURE_MAX) ||
	    (nr == XFEATURE_PT_UNIMPLEMENTED_SO_FAR) ||
	    ((nr >= XFEATURE_RSRVD_COMP_11) && (nr <= XFEATURE_LBR))) {
		WARN_ONCE(1, "no structure for xstate: %d\n", nr);
		XSTATE_WARN_ON(1);
	}
}

/*
 * This essentially double-checks what the cpu told us about
 * how large the XSAVE buffer needs to be.  We are recalculating
 * it to be safe.
 *
 * Dynamic XSAVE features allocate their own buffers and are not
 * covered by these checks. Only the size of the buffer for task->fpu
 * is checked here.
 */
static void do_extra_xstate_size_checks(void)
{
	int paranoid_xstate_size = FXSAVE_SIZE + XSAVE_HDR_SIZE;
	int i;

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		if (!xfeature_enabled(i))
			continue;

		check_xstate_against_struct(i);
		/*
		 * Supervisor state components can be managed only by
		 * XSAVES, which is compacted-format only.
		 */
		if (!using_compacted_format())
			XSTATE_WARN_ON(xfeature_is_supervisor(i));

		/* Align from the end of the previous feature */
		if (xfeature_is_aligned(i))
			paranoid_xstate_size = ALIGN(paranoid_xstate_size, 64);
		/*
		 * The offset of a given state in the non-compacted
		 * format is given to us in a CPUID leaf.  We check
		 * them for being ordered (increasing offsets) in
		 * setup_xstate_features().
		 */
		if (!using_compacted_format())
			paranoid_xstate_size = xfeature_uncompacted_offset(i);
		/*
		 * The compacted-format offset always depends on where
		 * the previous state ended.
		 */
		paranoid_xstate_size += xfeature_size(i);
	}
	XSTATE_WARN_ON(paranoid_xstate_size != fpu_kernel_xstate_size);
}


/*
 * Get total size of enabled xstates in XCR0 | IA32_XSS.
 *
 * Note the SDM's wording here.  "sub-function 0" only enumerates
 * the size of the *user* states.  If we use it to size a buffer
 * that we use 'XSAVES' on, we could potentially overflow the
 * buffer because 'XSAVES' saves system states too.
 */
static unsigned int __init get_xsaves_size(void)
{
	unsigned int eax, ebx, ecx, edx;
	/*
	 * - CPUID function 0DH, sub-function 1:
	 *    EBX enumerates the size (in bytes) required by
	 *    the XSAVES instruction for an XSAVE area
	 *    containing all the state components
	 *    corresponding to bits currently set in
	 *    XCR0 | IA32_XSS.
	 */
	cpuid_count(XSTATE_CPUID, 1, &eax, &ebx, &ecx, &edx);
	return ebx;
}

/*
 * Get the total size of the enabled xstates without the dynamic supervisor
 * features.
 */
static unsigned int __init get_xsaves_size_no_dynamic(void)
{
	u64 mask = xfeatures_mask_dynamic();
	unsigned int size;

	if (!mask)
		return get_xsaves_size();

	/* Disable dynamic features. */
	wrmsrl(MSR_IA32_XSS, xfeatures_mask_supervisor());

	/*
	 * Ask the hardware what size is required of the buffer.
	 * This is the size required for the task->fpu buffer.
	 */
	size = get_xsaves_size();

	/* Re-enable dynamic features so XSAVES will work on them again. */
	wrmsrl(MSR_IA32_XSS, xfeatures_mask_supervisor() | mask);

	return size;
}

static unsigned int __init get_xsave_size(void)
{
	unsigned int eax, ebx, ecx, edx;
	/*
	 * - CPUID function 0DH, sub-function 0:
	 *    EBX enumerates the size (in bytes) required by
	 *    the XSAVE instruction for an XSAVE area
	 *    containing all the *user* state components
	 *    corresponding to bits currently set in XCR0.
	 */
	cpuid_count(XSTATE_CPUID, 0, &eax, &ebx, &ecx, &edx);
	return ebx;
}

/*
 * Will the runtime-enumerated 'xstate_size' fit in the init
 * task's statically-allocated buffer?
 */
static bool is_supported_xstate_size(unsigned int test_xstate_size)
{
	if (test_xstate_size <= sizeof(union fpregs_state))
		return true;

	pr_warn("x86/fpu: xstate buffer too small (%zu < %d), disabling xsave\n",
			sizeof(union fpregs_state), test_xstate_size);
	return false;
}

static int init_xstate_size(void)
{
	/* Recompute the context size for enabled features: */
	unsigned int possible_xstate_size;
	unsigned int xsave_size;

	xsave_size = get_xsave_size();

	if (boot_cpu_has(X86_FEATURE_XSAVES))
		possible_xstate_size = get_xsaves_size_no_dynamic();
	else
		possible_xstate_size = xsave_size;

	/* Ensure we have the space to store all enabled: */
	if (!is_supported_xstate_size(possible_xstate_size))
		return -EINVAL;

	/*
	 * The size is OK, we are definitely going to use xsave,
	 * make it known to the world that we need more space.
	 */
	fpu_kernel_xstate_size = possible_xstate_size;
	do_extra_xstate_size_checks();

	/*
	 * User space is always in standard format.
	 */
	fpu_user_xstate_size = xsave_size;
	return 0;
}

/*
 * We enabled the XSAVE hardware, but something went wrong and
 * we can not use it.  Disable it.
 */
static void fpu__init_disable_system_xstate(void)
{
	xfeatures_mask_all = 0;
	cr4_clear_bits(X86_CR4_OSXSAVE);
	fpu__xstate_clear_all_cpu_caps();
}

/*
 * Enable and initialize the xsave feature.
 * Called once per system bootup.
 */
void __init fpu__init_system_xstate(void)
{
	unsigned int eax, ebx, ecx, edx;
	static int on_boot_cpu __initdata = 1;
	int err;
	int i;

	WARN_ON_FPU(!on_boot_cpu);
	on_boot_cpu = 0;

	if (!boot_cpu_has(X86_FEATURE_FPU)) {
		pr_info("x86/fpu: No FPU detected\n");
		return;
	}

	if (!boot_cpu_has(X86_FEATURE_XSAVE)) {
		pr_info("x86/fpu: x87 FPU will use %s\n",
			boot_cpu_has(X86_FEATURE_FXSR) ? "FXSAVE" : "FSAVE");
		return;
	}

	if (boot_cpu_data.cpuid_level < XSTATE_CPUID) {
		WARN_ON_FPU(1);
		return;
	}

	/*
	 * Find user xstates supported by the processor.
	 */
	cpuid_count(XSTATE_CPUID, 0, &eax, &ebx, &ecx, &edx);
	xfeatures_mask_all = eax + ((u64)edx << 32);

	/*
	 * Find supervisor xstates supported by the processor.
	 */
	cpuid_count(XSTATE_CPUID, 1, &eax, &ebx, &ecx, &edx);
	xfeatures_mask_all |= ecx + ((u64)edx << 32);

	if ((xfeatures_mask_user() & XFEATURE_MASK_FPSSE) != XFEATURE_MASK_FPSSE) {
		/*
		 * This indicates that something really unexpected happened
		 * with the enumeration.  Disable XSAVE and try to continue
		 * booting without it.  This is too early to BUG().
		 */
		pr_err("x86/fpu: FP/SSE not present amongst the CPU's xstate features: 0x%llx.\n",
		       xfeatures_mask_all);
		goto out_disable;
	}

	/*
	 * Clear XSAVE features that are disabled in the normal CPUID.
	 */
	for (i = 0; i < ARRAY_SIZE(xsave_cpuid_features); i++) {
		if (!boot_cpu_has(xsave_cpuid_features[i]))
			xfeatures_mask_all &= ~BIT_ULL(i);
	}

	xfeatures_mask_all &= fpu__get_supported_xfeatures_mask();

	/* Enable xstate instructions to be able to continue with initialization: */
	fpu__init_cpu_xstate();
	err = init_xstate_size();
	if (err)
		goto out_disable;

	/*
	 * Update info used for ptrace frames; use standard-format size and no
	 * supervisor xstates:
	 */
	update_regset_xstate_info(fpu_user_xstate_size, xfeatures_mask_user());

	fpu__init_prepare_fx_sw_frame();
	setup_init_fpu_buf();
	setup_xstate_comp_offsets();
	setup_supervisor_only_offsets();
	print_xstate_offset_size();

	pr_info("x86/fpu: Enabled xstate features 0x%llx, context size is %d bytes, using '%s' format.\n",
		xfeatures_mask_all,
		fpu_kernel_xstate_size,
		boot_cpu_has(X86_FEATURE_XSAVES) ? "compacted" : "standard");
	return;

out_disable:
	/* something went wrong, try to boot without any XSAVE support */
	fpu__init_disable_system_xstate();
}

/*
 * Restore minimal FPU state after suspend:
 */
void fpu__resume_cpu(void)
{
	/*
	 * Restore XCR0 on xsave capable CPUs:
	 */
	if (boot_cpu_has(X86_FEATURE_XSAVE))
		xsetbv(XCR_XFEATURE_ENABLED_MASK, xfeatures_mask_user());

	/*
	 * Restore IA32_XSS. The same CPUID bit enumerates support
	 * of XSAVES and MSR_IA32_XSS.
	 */
	if (boot_cpu_has(X86_FEATURE_XSAVES)) {
		wrmsrl(MSR_IA32_XSS, xfeatures_mask_supervisor()  |
				     xfeatures_mask_dynamic());
	}
}

/*
 * Given an xstate feature nr, calculate where in the xsave
 * buffer the state is.  Callers should ensure that the buffer
 * is valid.
 *
 * Note: does not work for compacted buffers.
 */
static void *__raw_xsave_addr(struct xregs_state *xsave, int xfeature_nr)
{
	if (!xfeature_enabled(xfeature_nr)) {
		WARN_ON_FPU(1);
		return NULL;
	}

	return (void *)xsave + xstate_comp_offsets[xfeature_nr];
}
/*
 * Given the xsave area and a state inside, this function returns the
 * address of the state.
 *
 * This is the API that is called to get xstate address in either
 * standard format or compacted format of xsave area.
 *
 * Note that if there is no data for the field in the xsave buffer
 * this will return NULL.
 *
 * Inputs:
 *	xstate: the thread's storage area for all FPU data
 *	xfeature_nr: state which is defined in xsave.h (e.g. XFEATURE_FP,
 *	XFEATURE_SSE, etc...)
 * Output:
 *	address of the state in the xsave area, or NULL if the
 *	field is not present in the xsave buffer.
 */
void *get_xsave_addr(struct xregs_state *xsave, int xfeature_nr)
{
	/*
	 * Do we even *have* xsave state?
	 */
	if (!boot_cpu_has(X86_FEATURE_XSAVE))
		return NULL;

	/*
	 * We should not ever be requesting features that we
	 * have not enabled.
	 */
	WARN_ONCE(!(xfeatures_mask_all & BIT_ULL(xfeature_nr)),
		  "get of unsupported state");
	/*
	 * This assumes the last 'xsave*' instruction to
	 * have requested that 'xfeature_nr' be saved.
	 * If it did not, we might be seeing and old value
	 * of the field in the buffer.
	 *
	 * This can happen because the last 'xsave' did not
	 * request that this feature be saved (unlikely)
	 * or because the "init optimization" caused it
	 * to not be saved.
	 */
	if (!(xsave->header.xfeatures & BIT_ULL(xfeature_nr)))
		return NULL;

	return __raw_xsave_addr(xsave, xfeature_nr);
}
EXPORT_SYMBOL_GPL(get_xsave_addr);

/*
 * This wraps up the common operations that need to occur when retrieving
 * data from xsave state.  It first ensures that the current task was
 * using the FPU and retrieves the data in to a buffer.  It then calculates
 * the offset of the requested field in the buffer.
 *
 * This function is safe to call whether the FPU is in use or not.
 *
 * Note that this only works on the current task.
 *
 * Inputs:
 *	@xfeature_nr: state which is defined in xsave.h (e.g. XFEATURE_FP,
 *	XFEATURE_SSE, etc...)
 * Output:
 *	address of the state in the xsave area or NULL if the state
 *	is not present or is in its 'init state'.
 */
const void *get_xsave_field_ptr(int xfeature_nr)
{
	struct fpu *fpu = &current->thread.fpu;

	/*
	 * fpu__save() takes the CPU's xstate registers
	 * and saves them off to the 'fpu memory buffer.
	 */
	fpu__save(fpu);

	return get_xsave_addr(&fpu->state.xsave, xfeature_nr);
}

#ifdef CONFIG_ARCH_HAS_PKEYS

#define NR_VALID_PKRU_BITS (CONFIG_NR_PROTECTION_KEYS * 2)
#define PKRU_VALID_MASK (NR_VALID_PKRU_BITS - 1)
/*
 * This will go out and modify PKRU register to set the access
 * rights for @pkey to @init_val.
 */
int arch_set_user_pkey_access(struct task_struct *tsk, int pkey,
		unsigned long init_val)
{
	u32 old_pkru;
	int pkey_shift = (pkey * PKRU_BITS_PER_PKEY);
	u32 new_pkru_bits = 0;

	/*
	 * This check implies XSAVE support.  OSPKE only gets
	 * set if we enable XSAVE and we enable PKU in XCR0.
	 */
	if (!boot_cpu_has(X86_FEATURE_OSPKE))
		return -EINVAL;

	/* Set the bits we need in PKRU:  */
	if (init_val & PKEY_DISABLE_ACCESS)
		new_pkru_bits |= PKRU_AD_BIT;
	if (init_val & PKEY_DISABLE_WRITE)
		new_pkru_bits |= PKRU_WD_BIT;

	/* Shift the bits in to the correct place in PKRU for pkey: */
	new_pkru_bits <<= pkey_shift;

	/* Get old PKRU and mask off any old bits in place: */
	old_pkru = read_pkru();
	old_pkru &= ~((PKRU_AD_BIT|PKRU_WD_BIT) << pkey_shift);

	/* Write old part along with new part: */
	write_pkru(old_pkru | new_pkru_bits);

	return 0;
}
#endif /* ! CONFIG_ARCH_HAS_PKEYS */

/*
 * Weird legacy quirk: SSE and YMM states store information in the
 * MXCSR and MXCSR_FLAGS fields of the FP area. That means if the FP
 * area is marked as unused in the xfeatures header, we need to copy
 * MXCSR and MXCSR_FLAGS if either SSE or YMM are in use.
 */
static inline bool xfeatures_mxcsr_quirk(u64 xfeatures)
{
	if (!(xfeatures & (XFEATURE_MASK_SSE|XFEATURE_MASK_YMM)))
		return false;

	if (xfeatures & XFEATURE_MASK_FP)
		return false;

	return true;
}

static void fill_gap(unsigned to, void **kbuf, unsigned *pos, unsigned *count)
{
	if (*pos < to) {
		unsigned size = to - *pos;

		if (size > *count)
			size = *count;
		memcpy(*kbuf, (void *)&init_fpstate.xsave + *pos, size);
		*kbuf += size;
		*pos += size;
		*count -= size;
	}
}

static void copy_part(unsigned offset, unsigned size, void *from,
			void **kbuf, unsigned *pos, unsigned *count)
{
	fill_gap(offset, kbuf, pos, count);
	if (size > *count)
		size = *count;
	if (size) {
		memcpy(*kbuf, from, size);
		*kbuf += size;
		*pos += size;
		*count -= size;
	}
}

/*
 * Convert from kernel XSAVES compacted format to standard format and copy
 * to a kernel-space ptrace buffer.
 *
 * It supports partial copy but pos always starts from zero. This is called
 * from xstateregs_get() and there we check the CPU has XSAVES.
 */
int copy_xstate_to_kernel(void *kbuf, struct xregs_state *xsave, unsigned int offset_start, unsigned int size_total)
{
	struct xstate_header header;
	const unsigned off_mxcsr = offsetof(struct fxregs_state, mxcsr);
	unsigned count = size_total;
	int i;

	/*
	 * Currently copy_regset_to_user() starts from pos 0:
	 */
	if (unlikely(offset_start != 0))
		return -EFAULT;

	/*
	 * The destination is a ptrace buffer; we put in only user xstates:
	 */
	memset(&header, 0, sizeof(header));
	header.xfeatures = xsave->header.xfeatures;
	header.xfeatures &= xfeatures_mask_user();

	if (header.xfeatures & XFEATURE_MASK_FP)
		copy_part(0, off_mxcsr,
			  &xsave->i387, &kbuf, &offset_start, &count);
	if (header.xfeatures & (XFEATURE_MASK_SSE | XFEATURE_MASK_YMM))
		copy_part(off_mxcsr, MXCSR_AND_FLAGS_SIZE,
			  &xsave->i387.mxcsr, &kbuf, &offset_start, &count);
	if (header.xfeatures & XFEATURE_MASK_FP)
		copy_part(offsetof(struct fxregs_state, st_space), 128,
			  &xsave->i387.st_space, &kbuf, &offset_start, &count);
	if (header.xfeatures & XFEATURE_MASK_SSE)
		copy_part(xstate_offsets[XFEATURE_SSE], 256,
			  &xsave->i387.xmm_space, &kbuf, &offset_start, &count);
	/*
	 * Fill xsave->i387.sw_reserved value for ptrace frame:
	 */
	copy_part(offsetof(struct fxregs_state, sw_reserved), 48,
		  xstate_fx_sw_bytes, &kbuf, &offset_start, &count);
	/*
	 * Copy xregs_state->header:
	 */
	copy_part(offsetof(struct xregs_state, header), sizeof(header),
		  &header, &kbuf, &offset_start, &count);

	for (i = FIRST_EXTENDED_XFEATURE; i < XFEATURE_MAX; i++) {
		/*
		 * Copy only in-use xstates:
		 */
		if ((header.xfeatures >> i) & 1) {
			void *src = __raw_xsave_addr(xsave, i);

			copy_part(xstate_offsets[i], xstate_sizes[i],
				  src, &kbuf, &offset_start, &count);
		}

	}
	fill_gap(size_total, &kbuf, &offset_start, &count);

	return 0;
}

static inline int
__copy_xstate_to_user(void __user *ubuf, const void *data, unsigned int offset, unsigned int size, unsigned int size_total)
{
	if (!size)
		return 0;

	if (offset < size_total) {
		unsigned int copy = min(size, size_total - offset);

		if (__copy_to_user(ubuf + offset, data, copy))
			return -EFAULT;
	}
	return 0;
}

/*
 * Convert from kernel XSAVES compacted format to standard format and copy
 * to a user-space buffer. It supports partial copy but pos always starts from
 * zero. This is called from xstateregs_get() and there we check the CPU
 * has XSAVES.
 */
int copy_xstate_to_user(void __user *ubuf, struct xregs_state *xsave, unsigned int offset_start, unsigned int size_total)
{
	unsigned int offset, size;
	int ret, i;
	struct xstate_header header;

	/*
	 * Currently copy_regset_to_user() starts from pos 0:
	 */
	if (unlikely(offset_start != 0))
		return -EFAULT;

	/*
	 * The destination is a ptrace buffer; we put in only user xstates:
	 */
	memset(&header, 0, sizeof(header));
	header.xfeatures = xsave->header.xfeatures;
	header.xfeatures &= xfeatures_mask_user();

	/*
	 * Copy xregs_state->header:
	 */
	offset = offsetof(struct xregs_state, header);
	size = sizeof(header);

	ret = __copy_xstate_to_user(ubuf, &header, offset, size, size_total);
	if (ret)
		return ret;

	for (i = 0; i < XFEATURE_MAX; i++) {
		/*
		 * Copy only in-use xstates:
		 */
		if ((header.xfeatures >> i) & 1) {
			void *src = __raw_xsave_addr(xsave, i);

			offset = xstate_offsets[i];
			size = xstate_sizes[i];

			/* The next component has to fit fully into the output buffer: */
			if (offset + size > size_total)
				break;

			ret = __copy_xstate_to_user(ubuf, src, offset, size, size_total);
			if (ret)
				return ret;
		}

	}

	if (xfeatures_mxcsr_quirk(header.xfeatures)) {
		offset = offsetof(struct fxregs_state, mxcsr);
		size = MXCSR_AND_FLAGS_SIZE;
		__copy_xstate_to_user(ubuf, &xsave->i387.mxcsr, offset, size, size_total);
	}

	/*
	 * Fill xsave->i387.sw_reserved value for ptrace frame:
	 */
	offset = offsetof(struct fxregs_state, sw_reserved);
	size = sizeof(xstate_fx_sw_bytes);

	ret = __copy_xstate_to_user(ubuf, xstate_fx_sw_bytes, offset, size, size_total);
	if (ret)
		return ret;

	return 0;
}

/*
 * Convert from a ptrace standard-format kernel buffer to kernel XSAVES format
 * and copy to the target thread. This is called from xstateregs_set().
 */
int copy_kernel_to_xstate(struct xregs_state *xsave, const void *kbuf)
{
	unsigned int offset, size;
	int i;
	struct xstate_header hdr;

	offset = offsetof(struct xregs_state, header);
	size = sizeof(hdr);

	memcpy(&hdr, kbuf + offset, size);

	if (validate_user_xstate_header(&hdr))
		return -EINVAL;

	for (i = 0; i < XFEATURE_MAX; i++) {
		u64 mask = ((u64)1 << i);

		if (hdr.xfeatures & mask) {
			void *dst = __raw_xsave_addr(xsave, i);

			offset = xstate_offsets[i];
			size = xstate_sizes[i];

			memcpy(dst, kbuf + offset, size);
		}
	}

	if (xfeatures_mxcsr_quirk(hdr.xfeatures)) {
		offset = offsetof(struct fxregs_state, mxcsr);
		size = MXCSR_AND_FLAGS_SIZE;
		memcpy(&xsave->i387.mxcsr, kbuf + offset, size);
	}

	/*
	 * The state that came in from userspace was user-state only.
	 * Mask all the user states out of 'xfeatures':
	 */
	xsave->header.xfeatures &= XFEATURE_MASK_SUPERVISOR_ALL;

	/*
	 * Add back in the features that came in from userspace:
	 */
	xsave->header.xfeatures |= hdr.xfeatures;

	return 0;
}

/*
 * Convert from a ptrace or sigreturn standard-format user-space buffer to
 * kernel XSAVES format and copy to the target thread. This is called from
 * xstateregs_set(), as well as potentially from the sigreturn() and
 * rt_sigreturn() system calls.
 */
int copy_user_to_xstate(struct xregs_state *xsave, const void __user *ubuf)
{
	unsigned int offset, size;
	int i;
	struct xstate_header hdr;

	offset = offsetof(struct xregs_state, header);
	size = sizeof(hdr);

	if (copy_from_user(&hdr, ubuf + offset, size))
		return -EFAULT;

	if (validate_user_xstate_header(&hdr))
		return -EINVAL;

	for (i = 0; i < XFEATURE_MAX; i++) {
		u64 mask = ((u64)1 << i);

		if (hdr.xfeatures & mask) {
			void *dst = __raw_xsave_addr(xsave, i);

			offset = xstate_offsets[i];
			size = xstate_sizes[i];

			if (copy_from_user(dst, ubuf + offset, size))
				return -EFAULT;
		}
	}

	if (xfeatures_mxcsr_quirk(hdr.xfeatures)) {
		offset = offsetof(struct fxregs_state, mxcsr);
		size = MXCSR_AND_FLAGS_SIZE;
		if (copy_from_user(&xsave->i387.mxcsr, ubuf + offset, size))
			return -EFAULT;
	}

	/*
	 * The state that came in from userspace was user-state only.
	 * Mask all the user states out of 'xfeatures':
	 */
	xsave->header.xfeatures &= XFEATURE_MASK_SUPERVISOR_ALL;

	/*
	 * Add back in the features that came in from userspace:
	 */
	xsave->header.xfeatures |= hdr.xfeatures;

	return 0;
}

/*
 * Save only supervisor states to the kernel buffer.  This blows away all
 * old states, and is intended to be used only in __fpu__restore_sig(), where
 * user states are restored from the user buffer.
 */
void copy_supervisor_to_kernel(struct xregs_state *xstate)
{
	struct xstate_header *header;
	u64 max_bit, min_bit;
	u32 lmask, hmask;
	int err, i;

	if (WARN_ON(!boot_cpu_has(X86_FEATURE_XSAVES)))
		return;

	if (!xfeatures_mask_supervisor())
		return;

	max_bit = __fls(xfeatures_mask_supervisor());
	min_bit = __ffs(xfeatures_mask_supervisor());

	lmask = xfeatures_mask_supervisor();
	hmask = xfeatures_mask_supervisor() >> 32;
	XSTATE_OP(XSAVES, xstate, lmask, hmask, err);

	/* We should never fault when copying to a kernel buffer: */
	if (WARN_ON_FPU(err))
		return;

	/*
	 * At this point, the buffer has only supervisor states and must be
	 * converted back to normal kernel format.
	 */
	header = &xstate->header;
	header->xcomp_bv |= xfeatures_mask_all;

	/*
	 * This only moves states up in the buffer.  Start with
	 * the last state and move backwards so that states are
	 * not overwritten until after they are moved.  Note:
	 * memmove() allows overlapping src/dst buffers.
	 */
	for (i = max_bit; i >= min_bit; i--) {
		u8 *xbuf = (u8 *)xstate;

		if (!((header->xfeatures >> i) & 1))
			continue;

		/* Move xfeature 'i' into its normal location */
		memmove(xbuf + xstate_comp_offsets[i],
			xbuf + xstate_supervisor_only_offsets[i],
			xstate_sizes[i]);
	}
}

/**
 * copy_dynamic_supervisor_to_kernel() - Save dynamic supervisor states to
 *                                       an xsave area
 * @xstate: A pointer to an xsave area
 * @mask: Represent the dynamic supervisor features saved into the xsave area
 *
 * Only the dynamic supervisor states sets in the mask are saved into the xsave
 * area (See the comment in XFEATURE_MASK_DYNAMIC for the details of dynamic
 * supervisor feature). Besides the dynamic supervisor states, the legacy
 * region and XSAVE header are also saved into the xsave area. The supervisor
 * features in the XFEATURE_MASK_SUPERVISOR_SUPPORTED and
 * XFEATURE_MASK_SUPERVISOR_UNSUPPORTED are not saved.
 *
 * The xsave area must be 64-bytes aligned.
 */
void copy_dynamic_supervisor_to_kernel(struct xregs_state *xstate, u64 mask)
{
	u64 dynamic_mask = xfeatures_mask_dynamic() & mask;
	u32 lmask, hmask;
	int err;

	if (WARN_ON_FPU(!boot_cpu_has(X86_FEATURE_XSAVES)))
		return;

	if (WARN_ON_FPU(!dynamic_mask))
		return;

	lmask = dynamic_mask;
	hmask = dynamic_mask >> 32;

	XSTATE_OP(XSAVES, xstate, lmask, hmask, err);

	/* Should never fault when copying to a kernel buffer */
	WARN_ON_FPU(err);
}

/**
 * copy_kernel_to_dynamic_supervisor() - Restore dynamic supervisor states from
 *                                       an xsave area
 * @xstate: A pointer to an xsave area
 * @mask: Represent the dynamic supervisor features restored from the xsave area
 *
 * Only the dynamic supervisor states sets in the mask are restored from the
 * xsave area (See the comment in XFEATURE_MASK_DYNAMIC for the details of
 * dynamic supervisor feature). Besides the dynamic supervisor states, the
 * legacy region and XSAVE header are also restored from the xsave area. The
 * supervisor features in the XFEATURE_MASK_SUPERVISOR_SUPPORTED and
 * XFEATURE_MASK_SUPERVISOR_UNSUPPORTED are not restored.
 *
 * The xsave area must be 64-bytes aligned.
 */
void copy_kernel_to_dynamic_supervisor(struct xregs_state *xstate, u64 mask)
{
	u64 dynamic_mask = xfeatures_mask_dynamic() & mask;
	u32 lmask, hmask;
	int err;

	if (WARN_ON_FPU(!boot_cpu_has(X86_FEATURE_XSAVES)))
		return;

	if (WARN_ON_FPU(!dynamic_mask))
		return;

	lmask = dynamic_mask;
	hmask = dynamic_mask >> 32;

	XSTATE_OP(XRSTORS, xstate, lmask, hmask, err);

	/* Should never fault when copying from a kernel buffer */
	WARN_ON_FPU(err);
}

#ifdef CONFIG_IOMMU_SUPPORT
void update_pasid(void)
{
	u64 pasid_state;
	u32 pasid;

	if (!cpu_feature_enabled(X86_FEATURE_ENQCMD))
		return;

	if (!current->mm)
		return;

	pasid = READ_ONCE(current->mm->pasid);
	/* Set the valid bit in the PASID MSR/state only for valid pasid. */
	pasid_state = pasid == PASID_DISABLED ?
		      pasid : pasid | MSR_IA32_PASID_VALID;

	/*
	 * No need to hold fregs_lock() since the task's fpstate won't
	 * be changed by others (e.g. ptrace) while the task is being
	 * switched to or is in IPI.
	 */
	if (!test_thread_flag(TIF_NEED_FPU_LOAD)) {
		/* The MSR is active and can be directly updated. */
		wrmsrl(MSR_IA32_PASID, pasid_state);
	} else {
		struct fpu *fpu = &current->thread.fpu;
		struct ia32_pasid_state *ppasid_state;
		struct xregs_state *xsave;

		/*
		 * The CPU's xstate registers are not currently active. Just
		 * update the PASID state in the memory buffer here. The
		 * PASID MSR will be loaded when returning to user mode.
		 */
		xsave = &fpu->state.xsave;
		xsave->header.xfeatures |= XFEATURE_MASK_PASID;
		ppasid_state = get_xsave_addr(xsave, XFEATURE_PASID);
		/*
		 * Since XFEATURE_MASK_PASID is set in xfeatures, ppasid_state
		 * won't be NULL and no need to check its value.
		 *
		 * Only update the task's PASID state when it's different
		 * from the mm's pasid.
		 */
		if (ppasid_state->pasid != pasid_state) {
			/*
			 * Invalid fpregs so that state restoring will pick up
			 * the PASID state.
			 */
			__fpu_invalidate_fpregs_state(fpu);
			ppasid_state->pasid = pasid_state;
		}
	}
}
#endif /* CONFIG_IOMMU_SUPPORT */
// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <debug.h>
#include <inttypes.h>

#include <bits.h>
#include <new.h>
#include <string.h>

#include <arch/defines.h>
#include <arch/hypervisor.h>
#include <arch/x86/descriptor.h>
#include <arch/x86/feature.h>
#include <arch/x86/hypervisor.h>
#include <arch/x86/hypervisor_state.h>
#include <arch/x86/idt.h>
#include <arch/x86/registers.h>
#include <hypervisor/guest_physical_address_space.h>
#include <kernel/mp.h>
#include <kernel/thread.h>
#include <magenta/errors.h>

#if WITH_LIB_MAGENTA
#include <magenta/fifo_dispatcher.h>
#endif // WITH_LIB_MAGENTA

#include "hypervisor_priv.h"

#define VMX_ERR_CHECK(var) \
    "setna %[" #var "];"     // Check CF and ZF for error.

extern uint8_t _gdt[];

static const int kUartIoPort = 0x3f8;

static status_t vmxon(paddr_t pa) {
    uint8_t err;

    __asm__ volatile (
        "vmxon %[pa];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        : [pa] "m"(pa)
        : "cc", "memory");

    return err ? ERR_INTERNAL : NO_ERROR;
}

static status_t vmxoff() {
    uint8_t err;

    __asm__ volatile (
        "vmxoff;"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        :
        : "cc");

    return err ? ERR_INTERNAL : NO_ERROR;
}

static status_t vmptrld(paddr_t pa) {
    uint8_t err;

    __asm__ volatile (
        "vmptrld %[pa];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        : [pa] "m"(pa)
        : "cc", "memory");

    return err ? ERR_INTERNAL : NO_ERROR;
}

static status_t vmclear(paddr_t pa) {
    uint8_t err;

    __asm__ volatile (
        "vmclear %[pa];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        : [pa] "m"(pa)
        : "cc", "memory");

    return err ? ERR_INTERNAL : NO_ERROR;
}

static uint64_t vmread(uint64_t field) {
    uint8_t err;
    uint64_t val;

    __asm__ volatile(
        "vmread %[field], %[val];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err), [val] "=m"(val)
        : [field] "r"(field)
        : "cc");

    DEBUG_ASSERT(err == NO_ERROR);
    return val;
}

static uint16_t vmcs_read(VmcsField16 field) {
    return static_cast<uint16_t>(vmread(static_cast<uint64_t>(field)));
}

static uint32_t vmcs_read(VmcsField32 field) {
    return static_cast<uint32_t>(vmread(static_cast<uint64_t>(field)));
}

static uint64_t vmcs_read(VmcsField64 field) {
    return vmread(static_cast<uint64_t>(field));
}

static uint64_t vmcs_read(VmcsFieldXX field) {
    return vmread(static_cast<uint64_t>(field));
}

static void vmwrite(uint64_t field, uint64_t val) {
    uint8_t err;

    __asm__ volatile (
        "vmwrite %[val], %[field];"
        VMX_ERR_CHECK(err)
        : [err] "=r"(err)
        : [val] "r"(val), [field] "r"(field)
        : "cc");

    DEBUG_ASSERT(err == NO_ERROR);
}

static void vmcs_write(VmcsField16 field, uint16_t val) {
    vmwrite(static_cast<uint64_t>(field), val);
}

static void vmcs_write(VmcsField32 field, uint32_t val) {
    vmwrite(static_cast<uint64_t>(field), val);
}

static void vmcs_write(VmcsField64 field, uint64_t val) {
    vmwrite(static_cast<uint64_t>(field), val);
}

static void vmcs_write(VmcsFieldXX field, uint64_t val) {
    vmwrite(static_cast<uint64_t>(field), val);
}

// TODO(abdulla): Update this to execute on every CPU. For development, it is
// convenient to only consider a single CPU for now.
static status_t percpu_exec(thread_start_routine entry, void* arg) {
    thread_t *t = thread_create("vmx", entry, arg, HIGH_PRIORITY, DEFAULT_STACK_SIZE);
    if (!t)
        return ERR_NO_MEMORY;

    thread_set_pinned_cpu(t, 0);
    status_t status = thread_resume(t);
    if (status != NO_ERROR)
        return status;

    int retcode;
    status = thread_join(t, &retcode, INFINITE_TIME);
    return status != NO_ERROR ? status : retcode;
}

VmxInfo::VmxInfo() {
    // From Volume 3, Appendix A.1.
    uint64_t basic_info = read_msr(X86_MSR_IA32_VMX_BASIC);
    revision_id = static_cast<uint32_t>(BITS(basic_info, 30, 0));
    region_size = static_cast<uint16_t>(BITS_SHIFT(basic_info, 44, 32));
    write_back = BITS_SHIFT(basic_info, 53, 50) == VMX_MEMORY_TYPE_WRITE_BACK;
    io_exit_info = BIT_SHIFT(basic_info, 54);
    vmx_controls = BIT_SHIFT(basic_info, 55);
}

MiscInfo::MiscInfo() {
    // From Volume 3, Appendix A.6.
    uint64_t misc_info = read_msr(X86_MSR_IA32_VMX_MISC);
    wait_for_sipi = BIT_SHIFT(misc_info, 8);
    msr_list_limit = static_cast<uint32_t>(BITS_SHIFT(misc_info, 27, 25) + 1) * 512;
}

EptInfo::EptInfo() {
    // From Volume 3, Appendix A.10.
    uint64_t ept_info = read_msr(X86_MSR_IA32_VMX_EPT_VPID_CAP);
    page_walk_4 = BIT_SHIFT(ept_info, 6);
    write_back = BIT_SHIFT(ept_info, 14);
    pde_2mb_page = BIT_SHIFT(ept_info, 16);
    pdpe_1gb_page = BIT_SHIFT(ept_info, 17);
    ept_flags = BIT_SHIFT(ept_info, 21);
    exit_info = BIT_SHIFT(ept_info, 22);
    invept =
        // INVEPT instruction is supported.
        BIT_SHIFT(ept_info, 20) &&
        // Single-context INVEPT type is supported.
        BIT_SHIFT(ept_info, 25) &&
        // All-context INVEPT type is supported.
        BIT_SHIFT(ept_info, 26);
}

ExitInfo::ExitInfo() {
        exit_reason = static_cast<ExitReason>(vmcs_read(VmcsField32::EXIT_REASON));
        exit_qualification = vmcs_read(VmcsFieldXX::EXIT_QUALIFICATION);
        interruption_information = vmcs_read(VmcsField32::INTERRUPTION_INFORMATION);
        interruption_error_code = vmcs_read(VmcsField32::INTERRUPTION_ERROR_CODE);
        instruction_length = vmcs_read(VmcsField32::INSTRUCTION_LENGTH);
        instruction_information = vmcs_read(VmcsField32::INSTRUCTION_INFORMATION);
        guest_physical_address = vmcs_read(VmcsField64::GUEST_PHYSICAL_ADDRESS);
        guest_linear_address = vmcs_read(VmcsFieldXX::GUEST_LINEAR_ADDRESS);
        guest_interruptibility_state = vmcs_read(VmcsField32::GUEST_INTERRUPTIBILITY_STATE);
        guest_rip = vmcs_read(VmcsFieldXX::GUEST_RIP);

        dprintf(SPEW, "exit reason: %#" PRIx32 "\n", static_cast<uint32_t>(exit_reason));
        dprintf(SPEW, "exit qualification: %#" PRIx64 "\n", exit_qualification);
        dprintf(SPEW, "interruption information: %#" PRIx32 "\n", interruption_information);
        dprintf(SPEW, "interruption error code: %#" PRIx32 "\n", interruption_error_code);
        dprintf(SPEW, "instruction length: %#" PRIx32 "\n", instruction_length);
        dprintf(SPEW, "instruction information: %#" PRIx32 "\n", instruction_information);
        dprintf(SPEW, "guest physical address: %#" PRIx64 "\n", guest_physical_address);
        dprintf(SPEW, "guest linear address: %#" PRIx64 "\n", guest_linear_address);
        dprintf(SPEW, "guest interruptibility state: %#" PRIx32 "\n", guest_interruptibility_state);
        dprintf(SPEW, "guest rip: %#" PRIx64 "\n", guest_rip);
}

IoInfo::IoInfo(uint64_t qualification) {
    bytes = static_cast<uint8_t>(BITS(qualification, 2, 0) + 1);
    input = BIT_SHIFT(qualification, 3);
    string = BIT_SHIFT(qualification, 4);
    repeat = BIT_SHIFT(qualification, 5);
    port = static_cast<uint16_t>(BITS_SHIFT(qualification, 31, 16));
}

VmxPage::~VmxPage() {
    vm_page_t* page = paddr_to_vm_page(pa_);
    if (page != nullptr)
        pmm_free_page(page);
}

status_t VmxPage::Alloc(const VmxInfo& vmx_info, uint8_t fill) {
    // From Volume 3, Appendix A.1: Bits 44:32 report the number of bytes that
    // software should allocate for the VMXON region and any VMCS region. It is
    // a value greater than 0 and at most 4096 (bit 44 is set if and only if
    // bits 43:32 are clear).
    if (vmx_info.region_size > PAGE_SIZE)
        return ERR_NOT_SUPPORTED;

    // Check use write-back memory for VMX regions is supported.
    if (!vmx_info.write_back)
        return ERR_NOT_SUPPORTED;

    // The maximum size for a VMXON or VMCS region is 4096, therefore
    // unconditionally allocating a page is adequate.
    if (pmm_alloc_page(0, &pa_) == nullptr)
        return ERR_NO_MEMORY;

    DEBUG_ASSERT(IS_PAGE_ALIGNED(pa_));
    memset(VirtualAddress(), fill, PAGE_SIZE);
    return NO_ERROR;
}

paddr_t VmxPage::PhysicalAddress() {
    DEBUG_ASSERT(pa_ != 0);
    return pa_;
}

void* VmxPage::VirtualAddress() {
    DEBUG_ASSERT(pa_ != 0);
    return paddr_to_kvaddr(pa_);
}

static bool cr_is_invalid(uint64_t cr_value, uint32_t fixed0_msr, uint32_t fixed1_msr) {
    uint64_t fixed0 = read_msr(fixed0_msr);
    uint64_t fixed1 = read_msr(fixed1_msr);
    return ~(cr_value | ~fixed0) != 0 || ~(~cr_value | fixed1) != 0;
}

static int vmx_enable(void* arg) {
    VmxonContext* context = static_cast<VmxonContext*>(arg);
    VmxonPerCpu* per_cpu = context->PerCpu();

    // Check that we have instruction information when we VM exit on IO.
    VmxInfo vmx_info;
    if (!vmx_info.io_exit_info)
        return ERR_NOT_SUPPORTED;

    // Check that full VMX controls are supported.
    if (!vmx_info.vmx_controls)
        return ERR_NOT_SUPPORTED;

    // Check that a page-walk length of 4 is supported.
    EptInfo ept_info;
    if (!ept_info.page_walk_4)
        return ERR_NOT_SUPPORTED;

    // Check use write-back memory for EPT is supported.
    if (!ept_info.write_back)
        return ERR_NOT_SUPPORTED;

    // Check that accessed and dirty flags for EPT are supported.
    if (!ept_info.ept_flags)
        return ERR_NOT_SUPPORTED;

    // Check that the INVEPT instruction is supported.
    if (!ept_info.invept)
        return ERR_NOT_SUPPORTED;

    // Check that wait for startup IPI is a supported activity state.
    MiscInfo misc_info;
    if (!misc_info.wait_for_sipi)
        return ERR_NOT_SUPPORTED;

    // Enable VMXON, if required.
    uint64_t feature_control = read_msr(X86_MSR_IA32_FEATURE_CONTROL);
    if (!(feature_control & X86_MSR_IA32_FEATURE_CONTROL_LOCK) ||
        !(feature_control & X86_MSR_IA32_FEATURE_CONTROL_VMXON)) {
        if ((feature_control & X86_MSR_IA32_FEATURE_CONTROL_LOCK) &&
            !(feature_control & X86_MSR_IA32_FEATURE_CONTROL_VMXON)) {
            return ERR_NOT_SUPPORTED;
        }
        feature_control |= X86_MSR_IA32_FEATURE_CONTROL_LOCK;
        feature_control |= X86_MSR_IA32_FEATURE_CONTROL_VMXON;
        write_msr(X86_MSR_IA32_FEATURE_CONTROL, feature_control);
    }


    // Check control registers are in a VMX-friendly state.
    uint64_t cr0 = x86_get_cr0();
    if (cr_is_invalid(cr0, X86_MSR_IA32_VMX_CR0_FIXED0, X86_MSR_IA32_VMX_CR0_FIXED1)) {
        return ERR_BAD_STATE;
    }
    uint64_t cr4 = x86_get_cr4() | X86_CR4_VMXE;
    if (cr_is_invalid(cr4, X86_MSR_IA32_VMX_CR4_FIXED0, X86_MSR_IA32_VMX_CR4_FIXED1)) {
        return ERR_BAD_STATE;
    }

    // Enable VMX using the VMXE bit.
    x86_set_cr4(cr4);

    // Execute VMXON.
    return per_cpu->VmxOn();
}

status_t PerCpu::Init(const VmxInfo& info) {
    status_t status = page_.Alloc(info, 0);
    if (status != NO_ERROR)
        return status;

    VmxRegion* region = page_.VirtualAddress<VmxRegion>();
    region->revision_id = info.revision_id;
    return NO_ERROR;
}

status_t VmxonPerCpu::VmxOn() {
    status_t status = vmxon(page_.PhysicalAddress());
    is_on_ = status == NO_ERROR;
    return status;
}

status_t VmxonPerCpu::VmxOff() {
    return is_on_ ? vmxoff() : NO_ERROR;
}

// static
status_t VmxonContext::Create(mxtl::unique_ptr<VmxonContext>* context) {
    uint num_cpus = arch_max_num_cpus();

    AllocChecker ac;
    VmxonPerCpu* ctxs = new (&ac) VmxonPerCpu[num_cpus];
    if (!ac.check())
        return ERR_NO_MEMORY;

    mxtl::Array<VmxonPerCpu> cpu_ctxs(ctxs, num_cpus);
    mxtl::unique_ptr<VmxonContext> ctx(new (&ac) VmxonContext(mxtl::move(cpu_ctxs)));
    if (!ac.check())
        return ERR_NO_MEMORY;

    VmxInfo vmx_info;
    status_t status = InitPerCpus(vmx_info, &ctx->per_cpus_);
    if (status != NO_ERROR)
        return status;

    status = percpu_exec(vmx_enable, ctx.get());
    if (status != NO_ERROR)
        return status;

    *context = mxtl::move(ctx);
    return NO_ERROR;
}

VmxonContext::VmxonContext(mxtl::Array<VmxonPerCpu> per_cpus)
    : per_cpus_(mxtl::move(per_cpus)) {}

static int vmx_disable(void* arg) {
    VmxonContext* context = static_cast<VmxonContext*>(arg);
    VmxonPerCpu* per_cpu = context->PerCpu();

    // Execute VMXOFF.
    status_t status = per_cpu->VmxOff();
    if (status != NO_ERROR)
        return status;

    // Disable VMX.
    x86_set_cr4(x86_get_cr4() & ~X86_CR4_VMXE);
    return NO_ERROR;
}

VmxonContext::~VmxonContext() {
    __UNUSED status_t status = percpu_exec(vmx_disable, this);
    DEBUG_ASSERT(status == NO_ERROR);
}

VmxonPerCpu* VmxonContext::PerCpu() {
    return &per_cpus_[arch_curr_cpu_num()];
}

AutoVmcsLoad::AutoVmcsLoad(VmxPage* page) {
    DEBUG_ASSERT(!arch_ints_disabled());
    arch_disable_ints();
    __UNUSED status_t status = vmptrld(page->PhysicalAddress());
    DEBUG_ASSERT(status == NO_ERROR);
}

AutoVmcsLoad::~AutoVmcsLoad() {
    DEBUG_ASSERT(arch_ints_disabled());
    arch_enable_ints();
}

status_t VmcsPerCpu::Init(const VmxInfo& vmx_info) {
    status_t status = PerCpu::Init(vmx_info);
    if (status != NO_ERROR)
        return status;

    status = msr_bitmaps_page_.Alloc(vmx_info, 0xff);
    if (status != NO_ERROR)
        return status;

    status = host_msr_page_.Alloc(vmx_info, 0);
    if (status != NO_ERROR)
        return status;

    status = guest_msr_page_.Alloc(vmx_info, 0);
    if (status != NO_ERROR)
        return status;

    memset(&vmx_state_, 0, sizeof(vmx_state_));
    return NO_ERROR;
}

status_t VmcsPerCpu::Clear() {
    return page_.IsAllocated() ? vmclear(page_.PhysicalAddress()) : NO_ERROR;
}

static status_t set_vmcs_control(VmcsField32 controls, uint64_t true_msr, uint64_t old_msr,
                                 uint32_t set, uint32_t clear) {
    uint32_t allowed_0 = static_cast<uint32_t>(BITS(true_msr, 31, 0));
    uint32_t allowed_1 = static_cast<uint32_t>(BITS_SHIFT(true_msr, 63, 32));
    if ((allowed_1 & set) != set) {
        dprintf(SPEW, "can not set vmcs controls %#x\n", static_cast<uint>(controls));
        return ERR_NOT_SUPPORTED;
    }
    if ((~allowed_0 & clear) != clear) {
        dprintf(SPEW, "can not clear vmcs controls %#x\n", static_cast<uint>(controls));
        return ERR_NOT_SUPPORTED;
    }
    if ((set & clear) != 0) {
        dprintf(SPEW, "can not set and clear the same vmcs controls %#x\n",
                static_cast<uint>(controls));
        return ERR_INVALID_ARGS;
    }

    // Reference Volume 3, Section 31.5.1, Algorithm 3, Part C. If the control
    // can be either 0 or 1 (flexible), and the control is unknown, then refer
    // to the old MSR to find the default value.
    uint32_t flexible = allowed_0 ^ allowed_1;
    uint32_t unknown = flexible & ~(set | clear);
    uint32_t defaults = unknown & BITS(old_msr, 31, 0);
    vmcs_write(controls, allowed_0 | defaults | set);
    return NO_ERROR;
}

static uint64_t ept_pointer(paddr_t pml4_address) {
    DEBUG_ASSERT(IS_PAGE_ALIGNED(pml4_address));
    return
        // Physical address of the PML4 page, page aligned.
        pml4_address |
        // Use write back memory.
        VMX_MEMORY_TYPE_WRITE_BACK << 0 |
        // Page walk length of 4 (defined as N minus 1).
        3u << 3 |
        // Accessed and dirty flags are enabled.
        1u << 6;
}

static void ignore_msr(VmxPage* msr_bitmaps_page, uint32_t msr) {
    // From Volume 3, Section 24.6.9.
    uint8_t* msr_bitmaps = msr_bitmaps_page->VirtualAddress<uint8_t>();
    if (msr >= 0xc0000000)
        msr_bitmaps += 1 << 10;

    uint16_t msr_low = msr & 0x1fff;
    uint16_t msr_byte = msr_low / 8;
    uint8_t msr_bit = msr_low % 8;

    // Ignore reads to the MSR.
    msr_bitmaps[msr_byte] &= (uint8_t)~(1 << msr_bit);

    // Ignore writes to the MSR.
    msr_bitmaps += 2 << 10;
    msr_bitmaps[msr_byte] &= (uint8_t)~(1 << msr_bit);
}

struct MsrListEntry {
    uint32_t msr;
    uint32_t reserved;
    uint64_t value;
} __PACKED;

static void edit_msr_list(VmxPage* msr_list_page, uint index, uint32_t msr, uint64_t value) {
    // From Volume 3, Section 24.7.2.

    // From Volume 3, Appendix A.6: Specifically, if the value bits 27:25 of
    // IA32_VMX_MISC is N, then 512 * (N + 1) is the recommended maximum number
    // of MSRs to be included in each list.
    //
    // From Volume 3, Section 24.7.2: This field specifies the number of MSRs to
    // be stored on VM exit. It is recommended that this count not exceed 512
    // bytes.
    //
    // Since these two statements conflict, we are taking the conservative
    // minimum and asserting that: index < (512 bytes / size of MsrListEntry).
    ASSERT(index < (512 / sizeof(MsrListEntry)));

    MsrListEntry* entry = msr_list_page->VirtualAddress<MsrListEntry>() + index;
    entry->msr = msr;
    entry->value = value;
}

status_t VmcsPerCpu::Setup(paddr_t pml4_address) {
    status_t status = Clear();
    if (status != NO_ERROR)
        return status;

    AutoVmcsLoad vmcs_load(&page_);

    // Setup secondary processor-based VMCS controls.
    status = set_vmcs_control(VmcsField32::PROCBASED_CTLS2,
                              read_msr(X86_MSR_IA32_VMX_PROCBASED_CTLS2),
                              0,
                              // Enable use of extended page tables.
                              PROCBASED_CTLS2_EPT |
                              // Enable use of RDTSCP instruction.
                              PROCBASED_CTLS2_RDTSCP |
                              // Associate cached translations of linear
                              // addresses with a virtual processor ID.
                              PROCBASED_CTLS2_VPID |
                              // Enable use of XSAVES and XRSTORS instructions.
                              PROCBASED_CTLS2_XSAVES_XRSTORS,
                              0);
    if (status != NO_ERROR)
        return status;

    // Setup pin-based VMCS controls.
    status = set_vmcs_control(VmcsField32::PINBASED_CTLS,
                              read_msr(X86_MSR_IA32_VMX_TRUE_PINBASED_CTLS),
                              read_msr(X86_MSR_IA32_VMX_PINBASED_CTLS),
                              // External interrupts cause a VM exit.
                              PINBASED_CTLS_EXTINT_EXITING |
                              // Non-maskable interrupts cause a VM exit.
                              PINBASED_CTLS_NMI_EXITING,
                              0);
    if (status != NO_ERROR)
        return status;

    // Setup primary processor-based VMCS controls.
    status = set_vmcs_control(VmcsField32::PROCBASED_CTLS,
                              read_msr(X86_MSR_IA32_VMX_TRUE_PROCBASED_CTLS),
                              read_msr(X86_MSR_IA32_VMX_PROCBASED_CTLS),
                              // Enable VM exit on IO instructions.
                              PROCBASED_CTLS_IO_EXITING |
                              // Enable use of MSR bitmaps.
                              PROCBASED_CTLS_MSR_BITMAPS |
                              // Enable secondary processor-based controls.
                              PROCBASED_CTLS_PROCBASED_CTLS2,
                              // Disable VM exit on CR3 load.
                              PROCBASED_CTLS_CR3_LOAD_EXITING |
                              // Disable VM exit on CR3 store.
                              PROCBASED_CTLS_CR3_STORE_EXITING);
    if (status != NO_ERROR)
        return status;

    // Setup VM-exit VMCS controls.
    status = set_vmcs_control(VmcsField32::EXIT_CTLS,
                              read_msr(X86_MSR_IA32_VMX_TRUE_EXIT_CTLS),
                              read_msr(X86_MSR_IA32_VMX_EXIT_CTLS),
                              // Logical processor is in 64-bit mode after VM
                              // exit. On VM exit CS.L, IA32_EFER.LME, and
                              // IA32_EFER.LMA is set to true.
                              EXIT_CTLS_64BIT_MODE |
                              // Save the guest IA32_PAT MSR on exit.
                              EXIT_CTLS_SAVE_IA32_PAT |
                              // Load the host IA32_PAT MSR on exit.
                              EXIT_CTLS_LOAD_IA32_PAT |
                              // Save the guest IA32_EFER MSR on exit.
                              EXIT_CTLS_SAVE_IA32_EFER |
                              // Load the host IA32_EFER MSR on exit.
                              EXIT_CTLS_LOAD_IA32_EFER,
                              0);
    if (status != NO_ERROR)
        return status;

    // Setup VM-entry VMCS controls.
    status = set_vmcs_control(VmcsField32::ENTRY_CTLS,
                              read_msr(X86_MSR_IA32_VMX_TRUE_ENTRY_CTLS),
                              read_msr(X86_MSR_IA32_VMX_ENTRY_CTLS),
                              // After VM entry, logical processor is in IA-32e
                              // mode and IA32_EFER.LMA is set to true.
                              ENTRY_CTLS_IA32E_MODE |
                              // Load the guest IA32_PAT MSR on entry.
                              ENTRY_CTLS_LOAD_IA32_PAT |
                              // Load the guest IA32_EFER MSR on entry.
                              ENTRY_CTLS_LOAD_IA32_EFER,
                              0);
    if (status != NO_ERROR)
        return status;

    // From Volume 3, Section 24.6.3: The exception bitmap is a 32-bit field
    // that contains one bit for each exception. When an exception occurs,
    // its vector is used to select a bit in this field. If the bit is 1,
    // the exception causes a VM exit. If the bit is 0, the exception is
    // delivered normally through the IDT, using the descriptor
    // corresponding to the exception’s vector.
    //
    // From Volume 3, Section 25.2: If software desires VM exits on all page
    // faults, it can set bit 14 in the exception bitmap to 1 and set the
    // page-fault error-code mask and match fields each to 00000000H.
    vmcs_write(VmcsField32::EXCEPTION_BITMAP, EXCEPTION_BITMAP_ALL_EXCEPTIONS);
    vmcs_write(VmcsField32::PAGEFAULT_ERRORCODE_MASK, 0);
    vmcs_write(VmcsField32::PAGEFAULT_ERRORCODE_MATCH, 0);

    // From Volume 3, Section 28.1: Virtual-processor identifiers (VPIDs)
    // introduce to VMX operation a facility by which a logical processor may
    // cache information for multiple linear-address spaces. When VPIDs are
    // used, VMX transitions may retain cached information and the logical
    // processor switches to a different linear-address space.
    //
    // From Volume 3, Section 26.2.1.1: If the “enable VPID” VM-execution
    // control is 1, the value of the VPID VM-execution control field must not
    // be 0000H.
    //
    // From Volume 3, Section 28.3.3.3: If EPT is in use, the logical processor
    // associates all mappings it creates with the value of bits 51:12 of
    // current EPTP. If a VMM uses different EPTP values for different guests,
    // it may use the same VPID for those guests.
    x86_percpu* percpu = x86_get_percpu();
    vmcs_write(VmcsField16::VPID, static_cast<uint16_t>(percpu->cpu_num + 1));

    // From Volume 3, Section 28.2: The extended page-table mechanism (EPT) is a
    // feature that can be used to support the virtualization of physical
    // memory. When EPT is in use, certain addresses that would normally be
    // treated as physical addresses (and used to access memory) are instead
    // treated as guest-physical addresses. Guest-physical addresses are
    // translated by traversing a set of EPT paging structures to produce
    // physical addresses that are used to access memory.
    vmcs_write(VmcsField64::EPT_POINTER, ept_pointer(pml4_address));

    // Setup MSR handling.
    ignore_msr(&msr_bitmaps_page_, X86_MSR_IA32_GS_BASE);
    ignore_msr(&msr_bitmaps_page_, X86_MSR_IA32_KERNEL_GS_BASE);
    vmcs_write(VmcsField64::MSR_BITMAPS_ADDRESS, msr_bitmaps_page_.PhysicalAddress());

    edit_msr_list(&host_msr_page_, 0, X86_MSR_IA32_STAR, read_msr(X86_MSR_IA32_STAR));
    edit_msr_list(&host_msr_page_, 1, X86_MSR_IA32_LSTAR, read_msr(X86_MSR_IA32_LSTAR));
    edit_msr_list(&host_msr_page_, 2, X86_MSR_IA32_FMASK, read_msr(X86_MSR_IA32_FMASK));
    // NOTE(abdulla): Index 3, X86_MSR_IA32_KERNEL_GS_BASE, is handled below.
    vmcs_write(VmcsField64::EXIT_MSR_LOAD_ADDRESS, host_msr_page_.PhysicalAddress());
    vmcs_write(VmcsField32::EXIT_MSR_LOAD_COUNT, 4);

    edit_msr_list(&guest_msr_page_, 0, X86_MSR_IA32_KERNEL_GS_BASE, 0);
    vmcs_write(VmcsField64::EXIT_MSR_STORE_ADDRESS, guest_msr_page_.PhysicalAddress());
    vmcs_write(VmcsField32::EXIT_MSR_STORE_COUNT, 1);
    vmcs_write(VmcsField64::ENTRY_MSR_LOAD_ADDRESS, guest_msr_page_.PhysicalAddress());
    vmcs_write(VmcsField32::ENTRY_MSR_LOAD_COUNT, 1);

    // Setup VMCS host state.
    //
    // NOTE: We are pinned to a thread when executing this function, therefore
    // it is acceptable to use per-CPU state.
    vmcs_write(VmcsField64::HOST_IA32_PAT, read_msr(X86_MSR_IA32_PAT));
    vmcs_write(VmcsField64::HOST_IA32_EFER, read_msr(X86_MSR_IA32_EFER));
    vmcs_write(VmcsFieldXX::HOST_CR0, x86_get_cr0());
    vmcs_write(VmcsFieldXX::HOST_CR4, x86_get_cr4());
    vmcs_write(VmcsField16::HOST_ES_SELECTOR, 0);
    vmcs_write(VmcsField16::HOST_CS_SELECTOR, CODE_64_SELECTOR);
    vmcs_write(VmcsField16::HOST_SS_SELECTOR, DATA_SELECTOR);
    vmcs_write(VmcsField16::HOST_DS_SELECTOR, 0);
    vmcs_write(VmcsField16::HOST_FS_SELECTOR, 0);
    vmcs_write(VmcsField16::HOST_GS_SELECTOR, 0);
    vmcs_write(VmcsField16::HOST_TR_SELECTOR, TSS_SELECTOR(percpu->cpu_num));
    vmcs_write(VmcsFieldXX::HOST_FS_BASE, read_msr(X86_MSR_IA32_FS_BASE));
    vmcs_write(VmcsFieldXX::HOST_GS_BASE, read_msr(X86_MSR_IA32_GS_BASE));
    vmcs_write(VmcsFieldXX::HOST_TR_BASE, reinterpret_cast<uint64_t>(&percpu->default_tss));
    vmcs_write(VmcsFieldXX::HOST_GDTR_BASE, reinterpret_cast<uint64_t>(_gdt));
    vmcs_write(VmcsFieldXX::HOST_IDTR_BASE, reinterpret_cast<uint64_t>(idt_get_readonly()));
    vmcs_write(VmcsFieldXX::HOST_IA32_SYSENTER_ESP, 0);
    vmcs_write(VmcsFieldXX::HOST_IA32_SYSENTER_EIP, 0);
    vmcs_write(VmcsField32::HOST_IA32_SYSENTER_CS, 0);
    vmcs_write(VmcsFieldXX::HOST_RSP, reinterpret_cast<uint64_t>(&vmx_state_));
    vmcs_write(VmcsFieldXX::HOST_RIP, reinterpret_cast<uint64_t>(vmx_exit_entry));

    // Setup VMCS guest state.

    uint64_t cr0 = X86_CR0_PE | // Enable protected mode
                   X86_CR0_PG | // Enable paging
                   X86_CR0_NE;  // Enable internal x87 exception handling
    if (cr_is_invalid(cr0, X86_MSR_IA32_VMX_CR0_FIXED0, X86_MSR_IA32_VMX_CR0_FIXED1)) {
        return ERR_BAD_STATE;
    }
    vmcs_write(VmcsFieldXX::GUEST_CR0, cr0);

    uint64_t cr4 = X86_CR4_PAE |  // Enable PAE paging
                   X86_CR4_VMXE;  // Enable VMX
    if (cr_is_invalid(cr4, X86_MSR_IA32_VMX_CR4_FIXED0, X86_MSR_IA32_VMX_CR4_FIXED1)) {
        return ERR_BAD_STATE;
    }
    vmcs_write(VmcsFieldXX::GUEST_CR4, cr4);

    vmcs_write(VmcsField64::GUEST_IA32_PAT, read_msr(X86_MSR_IA32_PAT));
    vmcs_write(VmcsField64::GUEST_IA32_EFER, read_msr(X86_MSR_IA32_EFER));

    vmcs_write(VmcsField32::GUEST_CS_ACCESS_RIGHTS,
               GUEST_XX_ACCESS_RIGHTS_TYPE_A |
               GUEST_XX_ACCESS_RIGHTS_TYPE_W |
               GUEST_XX_ACCESS_RIGHTS_TYPE_E |
               GUEST_XX_ACCESS_RIGHTS_TYPE_CODE |
               GUEST_XX_ACCESS_RIGHTS_S |
               GUEST_XX_ACCESS_RIGHTS_P |
               GUEST_XX_ACCESS_RIGHTS_L);

    vmcs_write(VmcsField32::GUEST_TR_ACCESS_RIGHTS,
               GUEST_TR_ACCESS_RIGHTS_TSS_BUSY |
               GUEST_XX_ACCESS_RIGHTS_P);

    // Disable all other segment selectors until we have a guest that uses them.
    vmcs_write(VmcsField32::GUEST_SS_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);
    vmcs_write(VmcsField32::GUEST_DS_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);
    vmcs_write(VmcsField32::GUEST_ES_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);
    vmcs_write(VmcsField32::GUEST_FS_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);
    vmcs_write(VmcsField32::GUEST_GS_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);
    vmcs_write(VmcsField32::GUEST_LDTR_ACCESS_RIGHTS, GUEST_XX_ACCESS_RIGHTS_UNUSABLE);

    vmcs_write(VmcsFieldXX::GUEST_GDTR_BASE, 0);
    vmcs_write(VmcsField32::GUEST_GDTR_LIMIT, 0);
    vmcs_write(VmcsFieldXX::GUEST_IDTR_BASE, 0);
    vmcs_write(VmcsField32::GUEST_IDTR_LIMIT, 0);

    // Set all reserved RFLAGS bits to their correct values
    vmcs_write(VmcsFieldXX::GUEST_RFLAGS, X86_FLAGS_RESERVED_ONES);

    vmcs_write(VmcsField32::GUEST_ACTIVITY_STATE, 0);
    vmcs_write(VmcsField32::GUEST_INTERRUPTIBILITY_STATE, 0);
    vmcs_write(VmcsFieldXX::GUEST_PENDING_DEBUG_EXCEPTIONS, 0);

    // From Volume 3, Section 26.3.1.1: The IA32_SYSENTER_ESP field and the
    // IA32_SYSENTER_EIP field must each contain a canonical address.
    vmcs_write(VmcsFieldXX::GUEST_IA32_SYSENTER_ESP, 0);
    vmcs_write(VmcsFieldXX::GUEST_IA32_SYSENTER_EIP, 0);

    vmcs_write(VmcsField32::GUEST_IA32_SYSENTER_CS, 0);
    vmcs_write(VmcsFieldXX::GUEST_RSP, 0);

    // From Volume 3, Section 24.4.2: If the “VMCS shadowing” VM-execution
    // control is 1, the VMREAD and VMWRITE instructions access the VMCS
    // referenced by this pointer (see Section 24.10). Otherwise, software
    // should set this field to FFFFFFFF_FFFFFFFFH to avoid VM-entry
    // failures (see Section 26.3.1.5).
    vmcs_write(VmcsField64::LINK_POINTER, LINK_POINTER_INVALIDATE);

    return NO_ERROR;
}

void vmx_exit(VmxState* vmx_state) {
    DEBUG_ASSERT(arch_ints_disabled());
    uint cpu_num = arch_curr_cpu_num();

    // Reload the task segment in order to restore its limit. VMX always
    // restores it with a limit of 0x67, which excludes the IO bitmap.
    seg_sel_t selector = TSS_SELECTOR(cpu_num);
    x86_clear_tss_busy(selector);
    x86_ltr(selector);

    // Reload the interrupt descriptor table in order to restore its limit. VMX
    // always restores it with a limit of 0xffff, which is too large.
    idt_load(idt_get_readonly());
}

static void next_rip(const ExitInfo& exit_info) {
    vmcs_write(VmcsFieldXX::GUEST_RIP, exit_info.guest_rip + exit_info.instruction_length);
}

static status_t handle_cpuid(const ExitInfo& exit_info, GuestState* guest_state) {
    switch (guest_state->rax) {
    case X86_CPUID_BASE:
        next_rip(exit_info);
        cpuid(X86_CPUID_BASE, (uint32_t*)&guest_state->rax, (uint32_t*)&guest_state->rbx,
              (uint32_t*)&guest_state->rcx, (uint32_t*)&guest_state->rdx);
        // Maximum input value for basic CPUID information.
        guest_state->rax = 0;
        return NO_ERROR;
    default:
        return ERR_NOT_SUPPORTED;
    }
}

static status_t vmexit_handler(const VmxState& vmx_state, GuestState* guest_state, FifoDispatcher* serial_fifo) {
    ExitInfo exit_info;

    switch (exit_info.exit_reason) {
    case ExitReason::EXTERNAL_INTERRUPT:
        dprintf(SPEW, "handling external interrupt\n\n");
        DEBUG_ASSERT(arch_ints_disabled());
        arch_enable_ints();
        arch_disable_ints();
        return NO_ERROR;
    case ExitReason::CPUID:
        dprintf(SPEW, "handling CPUID instruction\n\n");
        return handle_cpuid(exit_info, guest_state);
    case ExitReason::IO_INSTRUCTION: {
        dprintf(SPEW, "handling IO instruction\n\n");
        next_rip(exit_info);
#if WITH_LIB_MAGENTA
        IoInfo io_info(exit_info.exit_qualification);
        if (io_info.input || io_info.string || io_info.repeat || io_info.port != kUartIoPort)
            return NO_ERROR;
        uint8_t* data = reinterpret_cast<uint8_t*>(&guest_state->rax);
        uint32_t actual;
        return serial_fifo->Write(data, io_info.bytes, &actual);
#else // WITH_LIB_MAGENTA
        return NO_ERROR;
#endif // WITH_LIB_MAGENTA
    }
    case ExitReason::WRMSR:
        dprintf(SPEW, "handling WRMSR instruction\n\n");
        return ERR_NOT_SUPPORTED;
    default:
        dprintf(SPEW, "unhandled VM exit %u\n\n", static_cast<uint32_t>(exit_info.exit_reason));
        return ERR_NOT_SUPPORTED;
    }
}

status_t VmcsPerCpu::Enter(const VmcsContext& context, FifoDispatcher* serial_fifo) {
    AutoVmcsLoad vmcs_load(&page_);
    // FS is used for thread-local storage — save for this thread.
    vmcs_write(VmcsFieldXX::HOST_FS_BASE, read_msr(X86_MSR_IA32_FS_BASE));
    // CR3 is used to maintain the virtual address space — save for this thread.
    vmcs_write(VmcsFieldXX::HOST_CR3, x86_get_cr3());
    // Kernel GS stores the user-space GS (within the kernel) — as the calling
    // user-space thread may change, save this every time.
    edit_msr_list(&host_msr_page_, 3, X86_MSR_IA32_KERNEL_GS_BASE, read_msr(X86_MSR_IA32_KERNEL_GS_BASE));

    if (do_resume_) {
        dprintf(SPEW, "re-entering guest\n");
    } else {
        vmcs_write(VmcsFieldXX::GUEST_CR3, context.cr3());
        vmcs_write(VmcsFieldXX::GUEST_RIP, context.entry());
    }

    status_t status = vmx_enter(&vmx_state_, do_resume_);
    if (status != NO_ERROR) {
        uint64_t error = vmcs_read(VmcsField32::VM_INSTRUCTION_ERROR);
        dprintf(SPEW, "vmlaunch failed: %#" PRIx64 "\n", error);
    } else {
        do_resume_ = true;
        status = vmexit_handler(vmx_state_, &vmx_state_.guest_state, serial_fifo);
    }
    return status;
}

static int vmcs_setup(void* arg) {
    VmcsContext* context = static_cast<VmcsContext*>(arg);
    VmcsPerCpu* per_cpu = context->PerCpu();
    return per_cpu->Setup(context->Pml4Address());
}

// static
status_t VmcsContext::Create(mxtl::RefPtr<VmObject> guest_phys_mem,
                             mxtl::RefPtr<FifoDispatcher> serial_fifo,
                             mxtl::unique_ptr<VmcsContext>* context) {
    uint num_cpus = arch_max_num_cpus();

    AllocChecker ac;
    VmcsPerCpu* ctxs = new (&ac) VmcsPerCpu[num_cpus];
    if (!ac.check())
        return ERR_NO_MEMORY;

    mxtl::Array<VmcsPerCpu> cpu_ctxs(ctxs, num_cpus);
    mxtl::unique_ptr<VmcsContext> ctx(new (&ac) VmcsContext(serial_fifo, mxtl::move(cpu_ctxs)));
    if (!ac.check())
        return ERR_NO_MEMORY;

    status_t status = GuestPhysicalAddressSpace::Create(guest_phys_mem, &ctx->gpas_);
    if (status != NO_ERROR)
        return status;

    VmxInfo vmx_info;
    status = InitPerCpus(vmx_info, &ctx->per_cpus_);
    if (status != NO_ERROR)
        return status;

    status = percpu_exec(vmcs_setup, ctx.get());
    if (status != NO_ERROR)
        return status;

    *context = mxtl::move(ctx);
    return NO_ERROR;
}

VmcsContext::VmcsContext(mxtl::RefPtr<FifoDispatcher> serial_fifo,
                         mxtl::Array<VmcsPerCpu> per_cpus)
    : serial_fifo_(serial_fifo), per_cpus_(mxtl::move(per_cpus)) {}

static int vmcs_clear(void* arg) {
    VmcsContext* context = static_cast<VmcsContext*>(arg);
    VmcsPerCpu* per_cpu = context->PerCpu();
    return per_cpu->Clear();
}

VmcsContext::~VmcsContext() {
    __UNUSED status_t status = percpu_exec(vmcs_clear, this);
    DEBUG_ASSERT(status == NO_ERROR);
}

paddr_t VmcsContext::Pml4Address() {
    return gpas_->Pml4Address();
}

VmcsPerCpu* VmcsContext::PerCpu() {
    return &per_cpus_[arch_curr_cpu_num()];
}

status_t VmcsContext::set_cr3(uintptr_t guest_cr3) {
    if (guest_cr3 >= gpas_->size() - PAGE_SIZE)
        return ERR_INVALID_ARGS;
    cr3_ = guest_cr3;
    return NO_ERROR;
}

status_t VmcsContext::set_entry(uintptr_t guest_entry) {
    if (guest_entry >= gpas_->size())
        return ERR_INVALID_ARGS;
    entry_ = guest_entry;
    return NO_ERROR;
}

static int vmcs_launch(void* arg) {
    VmcsContext* context = static_cast<VmcsContext*>(arg);
    VmcsPerCpu* per_cpu = context->PerCpu();
    return per_cpu->Enter(*context, context->serial_fifo());
}

status_t VmcsContext::Enter() {
    if (cr3_ == UINTPTR_MAX)
        return ERR_BAD_STATE;
    if (entry_ == UINTPTR_MAX)
        return ERR_BAD_STATE;
    return percpu_exec(vmcs_launch, this);
}

status_t arch_hypervisor_create(mxtl::unique_ptr<HypervisorContext>* context) {
    // Check that the CPU supports VMX.
    if (!x86_feature_test(X86_FEATURE_VMX))
        return ERR_NOT_SUPPORTED;

    return VmxonContext::Create(context);
}

status_t arch_guest_create(mxtl::RefPtr<VmObject> guest_phys_mem,
                           mxtl::RefPtr<FifoDispatcher> serial_fifo,
                           mxtl::unique_ptr<GuestContext>* context) {
    return VmcsContext::Create(guest_phys_mem, serial_fifo, context);
}

status_t arch_guest_enter(const mxtl::unique_ptr<GuestContext>& context) {
    return context->Enter();
}

status_t x86_guest_set_cr3(const mxtl::unique_ptr<GuestContext>& context, uintptr_t guest_cr3) {
    return context->set_cr3(guest_cr3);
}

status_t arch_guest_set_entry(const mxtl::unique_ptr<GuestContext>& context,
                              uintptr_t guest_entry) {
    return context->set_entry(guest_entry);
}

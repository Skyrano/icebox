#include "nt_os.hpp"

#define FDP_MODULE "nt::mem"
#include "endian.hpp"
#include "log.hpp"
#include "nt_mmu.hpp"

bool nt::Os::is_kernel_address(uint64_t ptr)
{
    return !!(ptr & 0xFFF0000000000000);
}

namespace
{
    struct ntphy_t
    {
        uint64_t ptr;
        bool     valid_page;
        bool     zero_page;
    };

    constexpr uint64_t mask(int bits)
    {
        return ~(~uint64_t(0) << bits);
    }

    // we will need to call this function recursively...
    opt<ntphy_t> virtual_to_physical(nt::Os& os, uint64_t ptr, proc_t* proc, dtb_t dtb);

    void unswizzle_pte(nt::Os& os, MMPTE& pte)
    {
        // The algorithm seems a bit strange but the following pattern is mostly used when a page fault occurs...
        //
        // if(PhysicalMemoryLimitMask && !(PteValue & 0x10))
        // 	  PteValue = PteValue & ~PhysicalMemoryLimitMask;
        if(os.PhysicalMemoryLimitMask_ && !pte.u.soft.SwizzleBit)
            pte.u.value &= ~os.PhysicalMemoryLimitMask_;
    }

    ntphy_t physical_page(uint64_t pfn, uint64_t offset)
    {
        return ntphy_t{pfn + offset, true, false};
    }

    constexpr auto page_fault_required = ntphy_t{0, false, false};
    constexpr auto zero_page           = ntphy_t{0, false, true};

    ntphy_t prototype_pte_to_physical(nt::Os& os, uint64_t ptr, MMPTE pte)
    {
        const auto virt = virt_t{read_le64(&ptr)};
        if(pte.u.hard.Valid)
            return physical_page(pte.u.hard.PageFrameNumber * PAGE_SIZE, virt.u.f.offset);

        unswizzle_pte(os, pte);
        if(pte.u.soft.Prototype)
            return page_fault_required; // FileMapping (SUBSECTION)

        if(pte.u.soft.Transition)
            return physical_page(pte.u.trans.PageFrameNumber * PAGE_SIZE, virt.u.f.offset);

        if(!pte.u.soft.PageFileHigh)
            return zero_page;

        return page_fault_required; // pagefile
    }

    opt<ntphy_t> vad_pte_to_physical(nt::Os& os, uint64_t ptr, proc_t* proc)
    {
        if(!proc)
        {
            // invalid access for a kernel address
            if(os.is_kernel_address(ptr))
                return {};

            // cannot access VAD without a proc
            return page_fault_required;
        }

        const auto area = vm_area::find(os.core_, *proc, ptr);
        if(!area)
            return {};

        const auto area_span = vm_area::span(os.core_, *proc, *area);
        if(!area_span)
            return {};

        auto first_proto_pte = uint64_t{};
        auto ok              = memory::read_virtual_with_dtb(os.core_, proc->kdtb, &first_proto_pte, area->id + os.offsets_[MMVAD_FirstPrototypePte], sizeof first_proto_pte);
        if(!ok)
            return {};

        auto       pte     = MMPTE{};
        const auto pte_ptr = first_proto_pte + ((ptr - area_span->addr) / PAGE_SIZE) * sizeof pte;
        ok                 = memory::read_virtual_with_dtb(os.core_, proc->kdtb, &pte.u.value, pte_ptr, sizeof pte.u.value);
        if(!ok)
            return {};

        return prototype_pte_to_physical(os, ptr, pte);
    }

    opt<ntphy_t> pte_to_physical(nt::Os& os, uint64_t ptr, proc_t* proc, MMPTE pte)
    {
        const auto virt = virt_t{read_le64(&ptr)};
        if(pte.u.hard.Valid)
            return physical_page(pte.u.hard.PageFrameNumber * PAGE_SIZE, virt.u.f.offset);

        unswizzle_pte(os, pte);
        if(pte.u.soft.Prototype)
        {
            constexpr auto vad_mask = 0xFFFFFFFF00000000ULL;
            if((pte.u.proto.ProtoAddress & vad_mask) == vad_mask)
                return vad_pte_to_physical(os, ptr, proc);

            // resolve prototype PTE from PagedPool so always use the kernel dtb
            const auto addr = static_cast<uint64_t>(pte.u.proto.ProtoAddress);
            const auto ok   = memory::read_virtual_with_dtb(os.core_, os.kernel_dtb(), &pte, addr, sizeof pte);
            if(!ok)
                return page_fault_required; // unable to read the prototype PTE in PagedPool

            return prototype_pte_to_physical(os, ptr, pte);
        }

        if(pte.u.soft.Transition)
            return physical_page(pte.u.trans.PageFrameNumber * PAGE_SIZE, virt.u.f.offset);

        if(!pte.u.value)
            return vad_pte_to_physical(os, ptr, proc);

        if(!pte.u.soft.PageFileHigh)
            return zero_page;

        return page_fault_required; // pagefile
    }

    opt<MMPTE> read_pml4e(nt::Os& os, const virt_t& virt, dtb_t dtb)
    {
        const auto pml4e_base = dtb.val & (mask(40) << 12);
        const auto pml4e_ptr  = pml4e_base + virt.u.f.pml4 * 8ULL;
        auto       pml4e      = MMPTE{};
        const auto ok         = memory::read_physical(os.core_, &pml4e, pml4e_ptr, sizeof pml4e);
        if(!ok)
            return {};

        if(!pml4e.u.hard.Valid)
            return {};

        return pml4e;
    }

    opt<MMPTE> read_pdpe(nt::Os& os, const virt_t& virt, const MMPTE& pml4e)
    {
        auto       pdpe     = MMPTE{};
        const auto pdpe_ptr = pml4e.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pdp * sizeof pdpe;
        auto       ok       = memory::read_physical(os.core_, &pdpe, pdpe_ptr, sizeof pdpe);
        if(!ok)
            return {};

        if(!pdpe.u.hard.Valid)
            return {};

        return pdpe;
    }

    opt<MMPTE> read_pte(nt::Os& os, const virt_t& virt, const MMPTE& pde)
    {
        auto pte     = MMPTE{};
        auto pte_ptr = pde.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pt * sizeof pte;
        auto ok      = memory::read_physical(os.core_, &pte, pte_ptr, sizeof pte);
        if(!ok)
            return {};

        return pte;
    }

    opt<MMPTE> read_pde(nt::Os& os, const virt_t& virt, const MMPTE& pdpe)
    {
        auto       pde     = MMPTE{};
        const auto pde_ptr = pdpe.u.hard.PageFrameNumber * PAGE_SIZE + virt.u.f.pd * sizeof pde;
        auto       ok      = memory::read_physical(os.core_, &pde, pde_ptr, sizeof pde);
        if(!ok)
            return {};

        return pde;
    }

    opt<ntphy_t> virtual_to_physical(nt::Os& os, uint64_t ptr, proc_t* proc, dtb_t dtb)
    {
        const auto virt  = virt_t{read_le64(&ptr)};
        const auto pml4e = read_pml4e(os, virt, dtb);
        if(!pml4e)
            return {};

        const auto pdpe = read_pdpe(os, virt, *pml4e);
        if(!pdpe)
            return {};

        // 1g page
        if(pdpe->u.hard.LargePage)
        {
            const auto offset = ptr & mask(30);
            const auto base   = pdpe->u.value & (mask(22) << 30);
            return physical_page(base, offset);
        }

        const auto pde = read_pde(os, virt, *pdpe);
        if(!pde)
            return {};

        if(!pde->u.hard.Valid)
            return pte_to_physical(os, ptr, proc, *pde);

        // 2mb page
        if(pde->u.hard.LargePage)
        {
            const auto offset = ptr & mask(21);
            const auto base   = pde->u.value & (mask(31) << 21);
            return physical_page(base, offset);
        }

        const auto pte = read_pte(os, virt, *pde);
        if(!pte)
            return {};

        return pte_to_physical(os, ptr, proc, *pte);
    }
}

bool nt::Os::read_page(void* dst, uint64_t ptr, proc_t* proc, dtb_t dtb)
{
    const auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return false;

    if(nt_phy->zero_page)
    {
        memset(dst, 0, PAGE_SIZE);
        return true;
    }

    if(nt_phy->valid_page)
        return memory::read_physical(core_, dst, nt_phy->ptr, PAGE_SIZE);

    return false;
}

namespace
{
    bool is_zero(const void* vptr, size_t size)
    {
        const auto* ptr = reinterpret_cast<const uint8_t*>(vptr);
        for(size_t i = 0; i < size; ++i)
            if(ptr[i])
                return false;
        return true;
    }
}

bool nt::Os::write_page(uint64_t ptr, const void* src, proc_t* proc, dtb_t dtb)
{
    const auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return false;

    if(nt_phy->valid_page)
        return memory::write_physical(core_, nt_phy->ptr, src, PAGE_SIZE);

    // last chance: check whether we write null bytes into zero page
    // in which case we can skip it...
    if(nt_phy->zero_page)
        return is_zero(src, PAGE_SIZE);

    return false;
}

opt<phy_t> nt::Os::virtual_to_physical(proc_t* proc, dtb_t dtb, uint64_t ptr)
{
    auto nt_phy = ::virtual_to_physical(*this, ptr, proc, dtb);
    if(!nt_phy)
        return {};

    if(nt_phy->valid_page)
        return phy_t{nt_phy->ptr};

    return {};
}

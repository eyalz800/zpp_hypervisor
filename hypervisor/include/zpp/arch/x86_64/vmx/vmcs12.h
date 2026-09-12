#pragma once
#include "zpp/arch/x86_64/vmx/vmcs_fields.h"
#include <cstddef>
#include <cstdint>

namespace zpp::arch::x86_64::vmx
{
/**
 * One VMCS field encoding, taken apart.
 *
 * SDM Table 27-22, "Structure of VMCS Component Encoding", gives the whole
 * of it: bit 0 is the access type, bits 9:1 an index, bits 11:10 a type,
 * bit 12 reserved, bits 14:13 a width and bits 31:15 reserved. Nothing
 * here is a choice - it is that table, spelled out once so that no caller
 * shifts and masks it again.
 *
 * Having it as a type is what lets a shadow VMCS be stored by these three
 * coordinates rather than as a structure of named members. The layout of a
 * real VMCS region is deliberately not architectural - SDM 27.11.1 says
 * "the format used to store the VMCS data is implementation-specific and
 * not architecturally defined" - so there is no layout to copy. The
 * encoding space, by contrast, is fully specified, which makes it the
 * right thing to index by.
 */
class vmcs_field_encoding
{
public:
    /**
     * The field types, from bits 11:10.
     */
    enum class type : std::uint64_t
    {
        control = 0,
        exit_information = 1,
        guest_state = 2,
        host_state = 3,
    };

    /**
     * The field widths, from bits 14:13.
     */
    enum class width : std::uint64_t
    {
        bits_16 = 0,
        bits_64 = 1,
        bits_32 = 2,
        natural = 3,
    };

    /**
     * The highest index a shadow stores, plus one.
     *
     * Derived from Appendix B, which enumerates every field the
     * architecture defines, and from the requirement that a shadow fit in
     * the 4096 bytes of a VMCS region. The largest index Appendix B uses
     * per width and type is:
     *
     *   16-bit    control 5   exit info -    guest 10   host  6
     *   64-bit    control 41  exit info 2    guest 24   host 13
     *   32-bit    control 19  exit info 7    guest 23   host  0
     *   natural   control 7   exit info 5    guest 22   host 14
     *
     * Twenty-eight covers all of it except 64-bit control indices 28 to
     * 41, encodings 00002038H upwards. Those are the fields belonging to
     * features this VMM does not report support for - the tertiary
     * controls and the pointers and bitmaps that go with them - so
     * refusing them is consistent with the capability MSRs rather than a
     * hole in them. Twenty-eight is also what fits: sixteen slots of
     * eight bytes for each of the sixteen width-and-type pairs comes to
     * 3584 bytes, and the region is 4096.
     *
     * The number is not a secret kept from the guest. IA32_VMX_VMCS_ENUM
     * exists to report "the highest index value used for any VMCS
     * encoding" (SDM A.9), so a guest hypervisor is told exactly this.
     */
    static constexpr std::uint64_t index_capacity = 28;

    /**
     * Constructs the decoded form of a raw encoding.
     */
    constexpr explicit vmcs_field_encoding(std::uint64_t value) :
        m_value(value)
    {
    }

    /**
     * The raw encoding.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

    /**
     * Whether this names the high 32 bits of a 64-bit field rather than
     * the whole of it. Bit 0.
     */
    constexpr bool high_access() const
    {
        return 0 != (m_value & 1);
    }

    /**
     * Bits 9:1.
     */
    constexpr std::uint64_t index() const
    {
        return (m_value >> 1) & 0x1ff;
    }

    /**
     * Bits 11:10.
     */
    constexpr type field_type() const
    {
        return type((m_value >> 10) & 0x3);
    }

    /**
     * Bits 14:13.
     */
    constexpr width field_width() const
    {
        return width((m_value >> 13) & 0x3);
    }

    /**
     * Whether this encoding names a field a shadow has storage for.
     *
     * Three things are checked, and the first two are rules from Table
     * 27-22 rather than policy:
     *
     * - bit 12 and everything above bit 14 are reserved and must be 0.
     *   The SDM states the reservation for bits 31:15; the high half is
     *   covered because VMREAD and VMWRITE "fail if given, in 64-bit
     *   mode, an operand that sets an encoding bit beyond bit 32" (SDM
     *   27.11.2), and any bit in 63:32 is beyond every defined field.
     * - the access type must be full for 16-bit, 32-bit and natural-width
     *   fields, which Table 27-22 says outright: "must be full for
     *   16-bit, 32-bit, and natural-width fields".
     * - the index must be one the shadow has room for, which is the
     *   subset described on index_capacity and reported through
     *   IA32_VMX_VMCS_ENUM.
     *
     * What this does *not* do is check the encoding against a list of the
     * fields that exist. A real processor answers VMfailValid with error
     * 12, "VMREAD/VMWRITE from/to unsupported VMCS component", for a
     * structurally valid encoding naming a field it does not implement,
     * and this accepts those instead. The gap is deliberate and its
     * direction is the safe one: within the index range the shadow
     * accepts more than hardware would, never less, so no guest
     * hypervisor is refused a field it may legitimately use. Closing it
     * needs Appendix B as data; vmcs_fields.h is not that - it is an enum
     * of the fields this VMM itself uses, so it is a subset, and checking
     * against it would refuse fields that do exist. That is the worse
     * error of the two.
     */
    constexpr bool valid() const
    {
        constexpr std::uint64_t reserved =
            (1ull << 12) | ~((1ull << 15) - 1);

        if (0 != (m_value & reserved)) {
            return false;
        }

        if (high_access() && (width::bits_64 != field_width())) {
            return false;
        }

        return index() < index_capacity;
    }

    /**
     * Whether writing this field is refused as a write to a read-only
     * component.
     *
     * The VM-exit information fields are the read-only ones, and whether
     * they may be written at all is a processor capability: SDM A.6 says
     * of IA32_VMX_MISC bit 29 that "if bit 29 is read as 1, software can
     * use VMWRITE to write to any supported field in the VMCS; otherwise,
     * VMWRITE cannot be used to modify VM-exit information fields". A VMM
     * reporting that bit clear must refuse them, with error 13, "VMWRITE
     * to read-only VMCS component".
     */
    constexpr bool read_only() const
    {
        return type::exit_information == field_type();
    }

private:
    std::uint64_t m_value{};
};

/**
 * A shadow VMCS: the VMCS a guest hypervisor believes it is operating on.
 *
 * Shaped to sit in the guest's own VMCS region, which is what makes a VMCS
 * that moves between processors keep its contents - a guest hypervisor
 * migrating a virtual processor does VMCLEAR on one and VMPTRLD on
 * another, and anything held only in per-processor memory here would be
 * lost across that. The region is the guest's to allocate and ours to lay
 * out: SDM 27.11.1 says the storage format "is implementation-specific and
 * not architecturally defined", and that software "should never access or
 * modify the VMCS data of an active VMCS using ordinary memory
 * operations", so a guest that reads or writes anything here but the
 * revision identifier is already outside what the architecture promises
 * it. KVM does the same thing, with the same reasoning, in its own
 * vmcs12 layout.
 *
 * Two of the four header words are architectural. The revision identifier
 * must be the first dword because VMPTRLD checks it there against
 * IA32_VMX_BASIC (SDM 33.3, VMPTRLD: "IF revision identifier in
 * referenced VMCS region does not match VMCS revision identifier
 * supported by processor THEN VMfailValid"), and the VMX-abort indicator
 * follows it because SDM 27.2 puts it in the second. The launch state is
 * ours: it is not a VMCS field, cannot be reached with VMREAD, and has to
 * live somewhere a VMCLEAR of a VMCS that is not current can still reach.
 *
 * The fields themselves are indexed by width, type and index rather than
 * laid out as named members. That costs a little storage - every field
 * gets 64 bits whatever its width - and buys the property that matters:
 * there is no table mapping encodings to offsets that can disagree with
 * the encodings themselves.
 */
class vmcs12
{
public:
    /**
     * The revision identifier this VMM reports in IA32_VMX_BASIC, and
     * therefore the one a guest hypervisor writes into the first dword of
     * a region before VMPTRLD.
     *
     * Deliberately not the hardware's. The two structures have nothing in
     * common - a real VMCS holds whatever the processor puts there, this
     * holds the layout below - so sharing an identifier would let a region
     * prepared for one be accepted by the other. KVM makes the same choice
     * for the same reason, and notes that the value must change whenever
     * the content or layout of the shadow does. That applies here: change
     * it if anything below moves.
     */
    static constexpr std::uint32_t revision = 0x7a707001;

    /**
     * The launch state, which VMLAUNCH and VMRESUME each require a
     * particular value of.
     *
     * SDM 33.3, VMLAUNCH/VMRESUME: "VMLAUNCH fails if the launch state of
     * current VMCS is not 'clear' ... VMRESUME fails if the launch state
     * of the current VMCS is not 'launched'".
     */
    enum class launch_state : std::uint32_t
    {
        clear = 0,
        launched = 1,
    };

    /**
     * Reads a field. Zero for one never written, which is what a cleared
     * shadow leaves behind.
     *
     * Width is honoured on the way out, as SDM 27.11.2 describes: a
     * 16-bit field answers in bits 15:0 with the rest cleared, a 32-bit
     * field in bits 31:0, and the high access type of a 64-bit field
     * answers with bits 63:32 of the field in bits 31:0.
     */
    constexpr std::uint64_t read(vmcs_field_encoding encoding) const
    {
        auto value = slot(encoding);

        if (encoding.high_access()) {
            return value >> 32;
        }

        switch (encoding.field_width()) {
        case vmcs_field_encoding::width::bits_16:
            return value & 0xffff;
        case vmcs_field_encoding::width::bits_32:
            return value & 0xffffffff;
        default:
            return value;
        }
    }

    /**
     * Writes a field, honouring its width the same way.
     *
     * The one asymmetry is the high access type, which SDM 27.11.2 says
     * "writes the value of bits 31:0 of the source operand to bits 63:32
     * of the field" - so it is a merge rather than a store.
     */
    constexpr void write(vmcs_field_encoding encoding, std::uint64_t value)
    {
        auto & target = slot(encoding);

        if (encoding.high_access()) {
            target = (target & 0xffffffff) | (value << 32);
            return;
        }

        switch (encoding.field_width()) {
        case vmcs_field_encoding::width::bits_16:
            target = value & 0xffff;
            return;
        case vmcs_field_encoding::width::bits_32:
            target = value & 0xffffffff;
            return;
        default:
            target = value;
            return;
        }
    }

    /**
     * Reads a field named by an encoding this VMM already has a name for,
     * so code building a real VMCS out of a shadow spells fields rather
     * than numbers.
     */
    constexpr std::uint64_t read(vmcs_fields::vmcs_field field) const
    {
        return read(vmcs_field_encoding(field));
    }

    /**
     * Writes a field named the same way.
     */
    constexpr void write(vmcs_fields::vmcs_field field,
                         std::uint64_t value)
    {
        write(vmcs_field_encoding(field), value);
    }

    /**
     * Puts the whole region into the state VMCLEAR leaves it in: every
     * field forgotten, the launch state clear, the revision identifier
     * and abort indicator as VMPTRLD will want to find them.
     *
     * The revision identifier is written rather than preserved because
     * this is also what prepares a region the guest has only just
     * allocated, and a caller that has validated the guest's copy has
     * nothing to preserve.
     */
    constexpr void clear()
    {
        this->m_revision_id = revision;
        this->m_abort_indicator = 0;
        this->m_launch_state = launch_state::clear;
        this->m_reserved = 0;

        for (auto & by_type : this->m_fields) {
            for (auto & by_index : by_type) {
                for (auto & field : by_index) {
                    field = 0;
                }
            }
        }

        for (auto & byte : this->m_padding) {
            byte = 0;
        }
    }

    /**
     * The revision identifier as it is in the region, which is the one
     * thing here a guest is expected to have written itself.
     */
    constexpr std::uint32_t revision_id() const
    {
        return this->m_revision_id;
    }

    /**
     * The launch state.
     * @{
     */
    constexpr launch_state state() const
    {
        return this->m_launch_state;
    }

    constexpr void state(launch_state value)
    {
        static_assert(offsetof(vmcs12, m_launch_state) ==
                          launch_state_offset,
                      "the launch state has moved within the region");

        this->m_launch_state = value;
    }

    /**
     * Where the launch state sits in the region, in bytes.
     *
     * For the caller that has to write *only* this - VMCLEAR of a VMCS
     * this processor has never loaded. The architecture requires such a
     * VMCS to keep its data, so the write has to be this narrow; KVM
     * writes the same four bytes at `offsetof(struct vmcs12,
     * launch_state)` in `handle_vmclear`. Checked against the real
     * offset in `state` above, where the class is complete.
     */
    static constexpr std::size_t launch_state_offset = 8;
    /**
     * @}
     */

private:
    /**
     * The storage for one field, by width, type and index. Callers must
     * have checked valid() first: an out of range index here would run
     * off the array, so this traps rather than clamping.
     */
    constexpr std::uint64_t & slot(vmcs_field_encoding encoding)
    {
        if (!encoding.valid()) {
            __builtin_trap();
        }

        return this
            ->m_fields[static_cast<std::uint64_t>(encoding.field_width())]
                      [static_cast<std::uint64_t>(encoding.field_type())]
                      [encoding.index()];
    }

    /**
     * The same, for a constant shadow.
     */
    constexpr std::uint64_t slot(vmcs_field_encoding encoding) const
    {
        if (!encoding.valid()) {
            __builtin_trap();
        }

        return this
            ->m_fields[static_cast<std::uint64_t>(encoding.field_width())]
                      [static_cast<std::uint64_t>(encoding.field_type())]
                      [encoding.index()];
    }

    /**
     * Must be the first dword: VMPTRLD checks it there.
     */
    std::uint32_t m_revision_id{revision};

    /**
     * The second dword, where SDM 27.2 puts the VMX-abort indicator.
     * Never written by anything here, because a VMX abort is a failure of
     * a real VM exit and no real VM exit uses this region.
     */
    std::uint32_t m_abort_indicator{};

    /**
     * Clear until a VMLAUNCH succeeds.
     */
    launch_state m_launch_state{launch_state::clear};

    /**
     * Keeps the field array eight-byte aligned.
     */
    std::uint32_t m_reserved{};

    /**
     * Every field, by width, type and index.
     */
    std::uint64_t m_fields[4][4][vmcs_field_encoding::index_capacity]{};

    /**
     * The rest of the region, so that a shadow is exactly the size of the
     * page it lives in and a copy either way moves the whole of it.
     */
    std::uint8_t m_padding[4096 - 16 -
                           (4 * 4 * vmcs_field_encoding::index_capacity *
                            sizeof(std::uint64_t))]{};
};

/**
 * The whole point of the padding: a shadow is a VMCS region.
 */
static_assert(sizeof(vmcs12) == 4096);

} // namespace zpp::arch::x86_64::vmx

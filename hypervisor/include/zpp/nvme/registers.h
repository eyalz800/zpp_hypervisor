#pragma once
#include <cstdint>

namespace zpp::nvme
{
/**
 * Byte offsets of the controller registers inside the memory mapped
 * register block that BAR0 points at.
 *
 * Verified against Linux `include/linux/nvme.h`, `enum` at line 130 of
 * v6.12 (the anonymous enum whose members are `NVME_REG_*`):
 *
 *     NVME_REG_CAP   = 0x0000,  NVME_REG_VS   = 0x0008,
 *     NVME_REG_INTMS = 0x000c,  NVME_REG_INTMC = 0x0010,
 *     NVME_REG_CC    = 0x0014,  NVME_REG_CSTS = 0x001c,
 *     NVME_REG_NSSR  = 0x0020,  NVME_REG_AQA  = 0x0024,
 *     NVME_REG_ASQ   = 0x0028,  NVME_REG_ACQ  = 0x0030,
 *     NVME_REG_DBS   = 0x1000,
 *
 * and cross-checked against the ordering of `struct
 * spdk_nvme_registers` in SPDK `include/spdk/nvme_spec.h` (v24.09,
 * line 483 onwards), which lays the same block out as a structure and
 * therefore encodes the same offsets independently.
 *
 * Note the gap at 0x18: it is reserved, which is why `csts` is at
 * 0x1c and not 0x18. SPDK spells that gap `uint32_t reserved1`.
 */
enum class register_offset : std::uint32_t
{
    /**
     * CAP - Controller Capabilities, 64 bit, read only.
     */
    capabilities = 0x00,

    /**
     * VS - Version, 32 bit, read only.
     */
    version = 0x08,

    /**
     * INTMS - Interrupt Mask Set, 32 bit.
     */
    interrupt_mask_set = 0x0c,

    /**
     * INTMC - Interrupt Mask Clear, 32 bit.
     */
    interrupt_mask_clear = 0x10,

    /**
     * CC - Controller Configuration, 32 bit.
     */
    configuration = 0x14,

    /**
     * CSTS - Controller Status, 32 bit, read only.
     */
    status = 0x1c,

    /**
     * NSSR - NVM Subsystem Reset, 32 bit, write only.
     */
    subsystem_reset = 0x20,

    /**
     * AQA - Admin Queue Attributes, 32 bit.
     */
    admin_queue_attributes = 0x24,

    /**
     * ASQ - Admin Submission Queue Base Address, 64 bit.
     */
    admin_submission_queue_base = 0x28,

    /**
     * ACQ - Admin Completion Queue Base Address, 64 bit.
     */
    admin_completion_queue_base = 0x30,

    /**
     * The first doorbell register, SQ0TDBL. Every other doorbell is a
     * multiple of the doorbell stride past this one - see
     * submission_queue_doorbell_offset().
     */
    doorbell_base = 0x1000,
};

/**
 * Returns the byte offset of a register within the BAR0 block.
 */
constexpr std::uint32_t offset_of(register_offset which)
{
    return static_cast<std::uint32_t>(which);
}

/**
 * CC.SHN - Shutdown Notification. The value the host writes to ask the
 * controller to shut down; the controller reports its progress back
 * through CSTS.SHST.
 *
 * Verified against Linux `include/linux/nvme.h` lines 216-218, which
 * spell these pre-shifted by NVME_CC_SHN_SHIFT (14):
 *
 *     NVME_CC_SHN_NONE   = 0 << NVME_CC_SHN_SHIFT,
 *     NVME_CC_SHN_NORMAL = 1 << NVME_CC_SHN_SHIFT,
 *     NVME_CC_SHN_ABRUPT = 2 << NVME_CC_SHN_SHIFT,
 *
 * The values here are *unshifted* field values, which is what
 * controller_configuration::shutdown_notification() takes and returns.
 * SPDK agrees on the unshifted form in `enum spdk_nvme_shn_value`
 * (nvme_spec.h line 158): SHN_NORMAL = 0x1, SHN_ABRUPT = 0x2.
 */
enum class shutdown_notification : std::uint32_t
{
    none = 0,
    normal = 1,
    abrupt = 2,
};

/**
 * CSTS.SHST - Shutdown Status, the controller's answer to CC.SHN.
 *
 * Verified against SPDK `enum spdk_nvme_shst_value`, nvme_spec.h line
 * 186: SHST_NORMAL = 0x0, SHST_OCCURRING = 0x1, SHST_COMPLETE = 0x2,
 * and against Linux lines 230-232, which spell the same three values
 * pre-shifted by 2 (NVME_CSTS_SHST_NORMAL / _OCCUR / _CMPLT).
 */
enum class shutdown_status : std::uint32_t
{
    normal = 0,
    occurring = 1,
    complete = 2,
};

/**
 * CAP - the Controller Capabilities register, 64 bit and read only.
 *
 * Modelled as a value with accessors rather than as a struct of
 * bitfields, matching zpp::arch::x86_64::vmx::epte: the register is
 * read as one 64 bit load and then decoded, and bitfield layout is not
 * something the standard pins down.
 *
 * Field positions verified against Linux `include/linux/nvme.h` lines
 * 164-171:
 *
 *     #define NVME_CAP_MQES(cap)    ((cap) & 0xffff)
 *     #define NVME_CAP_TIMEOUT(cap) (((cap) >> 24) & 0xff)
 *     #define NVME_CAP_STRIDE(cap)  (((cap) >> 32) & 0xf)
 *     #define NVME_CAP_NSSRC(cap)   (((cap) >> 36) & 0x1)
 *     #define NVME_CAP_CSS(cap)     (((cap) >> 37) & 0xff)
 *     #define NVME_CAP_MPSMIN(cap)  (((cap) >> 48) & 0xf)
 *     #define NVME_CAP_MPSMAX(cap)  (((cap) >> 52) & 0xf)
 *
 * Linux defines no accessor for CQR or AMS, so those two came from
 * SPDK's `union spdk_nvme_cap_register` (nvme_spec.h line 61), whose
 * bitfields run mqes:16, cqr:1, ams:2, reserved:5, to:8, dstrd:4,
 * nssrs:1, css:8, bps:1, reserved:2, mpsmin:4, mpsmax:4 - which places
 * cqr at 16 and ams at 18:17, and independently agrees with every
 * position Linux does define.
 */
class controller_capabilities
{
public:
    /**
     * Constructs a zero capabilities register value.
     */
    constexpr controller_capabilities() = default;

    /**
     * Constructs a capabilities register from a given value.
     */
    constexpr controller_capabilities(std::uint64_t value) : m_value(value)
    {
    }

    /**
     * MQES - Maximum Queue Entries Supported, bits 15:0.
     *
     * Zero's based: the field holds one less than the entry count, so
     * a controller reporting 0 supports queues of exactly one entry.
     * Use maximum_queue_entries() unless the raw field is wanted.
     */
    constexpr std::uint32_t maximum_queue_entries_field() const
    {
        return m_value & 0xffff;
    }

    /**
     * MQES decoded into an actual entry count, that is, the field plus
     * one.
     */
    constexpr std::uint32_t maximum_queue_entries() const
    {
        return maximum_queue_entries_field() + 1;
    }

    /**
     * CQR - Contiguous Queues Required, bit 16. When set, every queue
     * this host creates must be physically contiguous memory.
     */
    constexpr bool contiguous_queues_required() const
    {
        return m_value & (std::uint64_t{1} << 16);
    }

    /**
     * AMS - Arbitration Mechanism Supported, bits 18:17.
     */
    constexpr std::uint32_t arbitration_mechanism_supported() const
    {
        return (m_value >> 17) & 0x3;
    }

    /**
     * TO - Timeout, bits 31:24, in 500 millisecond units. It bounds
     * the wait for CSTS.RDY to follow a change to CC.EN.
     *
     * The unit is verified by Linux `drivers/nvme/host/core.c` line
     * 2451, which converts the field to seconds as
     * `(NVME_CAP_TIMEOUT(ctrl->cap) + 1) / 2`.
     */
    constexpr std::uint32_t timeout() const
    {
        return (m_value >> 24) & 0xff;
    }

    /**
     * TO expressed in milliseconds.
     */
    constexpr std::uint32_t timeout_in_milliseconds() const
    {
        return timeout() * 500;
    }

    /**
     * DSTRD - Doorbell Stride, bits 35:32. Doorbells are spaced
     * `4 << DSTRD` bytes apart - see
     * submission_queue_doorbell_offset().
     */
    constexpr std::uint32_t doorbell_stride() const
    {
        return (m_value >> 32) & 0xf;
    }

    /**
     * NSSRS - NVM Subsystem Reset Supported, bit 36.
     */
    constexpr bool subsystem_reset_supported() const
    {
        return m_value & (std::uint64_t{1} << 36);
    }

    /**
     * CSS - Command Sets Supported, bits 44:37. A bitmap, not an
     * index: bit 0 is the NVM command set.
     */
    constexpr std::uint32_t command_sets_supported() const
    {
        return (m_value >> 37) & 0xff;
    }

    /**
     * MPSMIN - Memory Page Size Minimum, bits 51:48. Encoded as
     * `2^(12 + MPSMIN)` bytes, verified by Linux
     * `drivers/nvme/host/core.c` line 2466,
     * `dev_page_min = NVME_CAP_MPSMIN(ctrl->cap) + 12`.
     */
    constexpr std::uint32_t memory_page_size_minimum() const
    {
        return (m_value >> 48) & 0xf;
    }

    /**
     * MPSMAX - Memory Page Size Maximum, bits 55:52, encoded the same
     * way as MPSMIN.
     */
    constexpr std::uint32_t memory_page_size_maximum() const
    {
        return (m_value >> 52) & 0xf;
    }

    /**
     * Returns the whole register value.
     */
    constexpr std::uint64_t value() const
    {
        return m_value;
    }

private:
    std::uint64_t m_value{};
};

/**
 * VS - the Version register, 32 bit and read only.
 *
 * Field positions verified against Linux `include/linux/nvme.h`:
 *
 *     #define NVME_VS(major, minor, tertiary) \
 *         (((major) << 16) | ((minor) << 8) | (tertiary))
 *
 * and against SPDK's `union spdk_nvme_vs_register` (nvme_spec.h line
 * 208), whose bitfields are ter:8, mnr:8, mjr:16, with static asserts
 * that SPDK_NVME_VERSION(1, 2, 1) == 0x00010201.
 */
class controller_version
{
public:
    /**
     * Constructs a zero version register value.
     */
    constexpr controller_version() = default;

    /**
     * Constructs a version register from a given value.
     */
    constexpr controller_version(std::uint32_t value) : m_value(value)
    {
    }

    /**
     * TER - the tertiary version number, bits 7:0.
     */
    constexpr std::uint32_t tertiary() const
    {
        return m_value & 0xff;
    }

    /**
     * MNR - the minor version number, bits 15:8.
     */
    constexpr std::uint32_t minor() const
    {
        return (m_value >> 8) & 0xff;
    }

    /**
     * MJR - the major version number, bits 31:16.
     */
    constexpr std::uint32_t major() const
    {
        return (m_value >> 16) & 0xffff;
    }

    /**
     * Returns the whole register value.
     */
    constexpr std::uint32_t value() const
    {
        return m_value;
    }

private:
    std::uint32_t m_value{};
};

/**
 * Composes a version register value from its three components, in the
 * same encoding controller_version decodes. Useful for comparing a
 * controller against a required revision.
 */
constexpr controller_version make_version(std::uint32_t major,
                                          std::uint32_t minor,
                                          std::uint32_t tertiary)
{
    return controller_version{(major << 16) | (minor << 8) | tertiary};
}

/**
 * CC - the Controller Configuration register, 32 bit.
 *
 * Field positions verified against Linux `include/linux/nvme.h` lines
 * 202-222:
 *
 *     NVME_CC_ENABLE = 1 << 0,   NVME_CC_CSS_SHIFT    = 4,
 *     NVME_CC_MPS_SHIFT = 7,     NVME_CC_AMS_SHIFT    = 11,
 *     NVME_CC_SHN_SHIFT = 14,    NVME_CC_IOSQES_SHIFT = 16,
 *     NVME_CC_IOCQES_SHIFT = 20,
 *
 * with the widths taken from SPDK's `union spdk_nvme_cc_register`
 * (nvme_spec.h line 127): en:1, reserved:3, css:3, mps:4, ams:3,
 * shn:2, iosqes:4, iocqes:4. Linux gives shifts only, SPDK gives
 * widths only, and the two are consistent.
 */
class controller_configuration
{
public:
    /**
     * Constructs a zero configuration register value.
     */
    constexpr controller_configuration() = default;

    /**
     * Constructs a configuration register from a given value.
     */
    constexpr controller_configuration(std::uint32_t value) :
        m_value(value)
    {
    }

    /**
     * EN - Enable, bit 0.
     */
    constexpr bool enable() const
    {
        return m_value & 1;
    }

    /**
     * Sets EN to the specified value.
     */
    constexpr void enable(bool value)
    {
        m_value = (m_value & ~0x1u) | std::uint32_t{value};
    }

    /**
     * CSS - I/O Command Set Selected, bits 6:4. Unlike CAP.CSS this
     * is an index, not a bitmap: 0 selects the NVM command set.
     */
    constexpr std::uint32_t command_set_selected() const
    {
        return (m_value >> 4) & 0x7;
    }

    /**
     * Sets CSS to the specified value.
     */
    constexpr void command_set_selected(std::uint32_t value)
    {
        m_value = (m_value & ~(0x7u << 4)) | ((value & 0x7) << 4);
    }

    /**
     * MPS - Memory Page Size, bits 10:7, encoded as
     * `2^(12 + MPS)` bytes. Verified by Linux
     * `drivers/nvme/host/core.c` line 2488, which writes
     * `(NVME_CTRL_PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT`. It must lie
     * between CAP.MPSMIN and CAP.MPSMAX.
     */
    constexpr std::uint32_t memory_page_size() const
    {
        return (m_value >> 7) & 0xf;
    }

    /**
     * Sets MPS to the specified value.
     */
    constexpr void memory_page_size(std::uint32_t value)
    {
        m_value = (m_value & ~(0xfu << 7)) | ((value & 0xf) << 7);
    }

    /**
     * AMS - Arbitration Mechanism Selected, bits 13:11.
     */
    constexpr std::uint32_t arbitration_mechanism() const
    {
        return (m_value >> 11) & 0x7;
    }

    /**
     * Sets AMS to the specified value.
     */
    constexpr void arbitration_mechanism(std::uint32_t value)
    {
        m_value = (m_value & ~(0x7u << 11)) | ((value & 0x7) << 11);
    }

    /**
     * SHN - Shutdown Notification, bits 15:14.
     */
    constexpr nvme::shutdown_notification shutdown_notification() const
    {
        return static_cast<nvme::shutdown_notification>((m_value >> 14) &
                                                        0x3);
    }

    /**
     * Sets SHN to the specified value.
     */
    constexpr void shutdown_notification(nvme::shutdown_notification value)
    {
        m_value = (m_value & ~(0x3u << 14)) |
                  ((static_cast<std::uint32_t>(value) & 0x3) << 14);
    }

    /**
     * IOSQES - I/O Submission Queue Entry Size, bits 19:16, as a
     * power of two. A submission entry is 64 bytes, so this is 6.
     */
    constexpr std::uint32_t submission_queue_entry_size() const
    {
        return (m_value >> 16) & 0xf;
    }

    /**
     * Sets IOSQES to the specified value.
     */
    constexpr void submission_queue_entry_size(std::uint32_t value)
    {
        m_value = (m_value & ~(0xfu << 16)) | ((value & 0xf) << 16);
    }

    /**
     * IOCQES - I/O Completion Queue Entry Size, bits 23:20, as a
     * power of two. A completion entry is 16 bytes, so this is 4.
     */
    constexpr std::uint32_t completion_queue_entry_size() const
    {
        return (m_value >> 20) & 0xf;
    }

    /**
     * Sets IOCQES to the specified value.
     */
    constexpr void completion_queue_entry_size(std::uint32_t value)
    {
        m_value = (m_value & ~(0xfu << 20)) | ((value & 0xf) << 20);
    }

    /**
     * Returns the whole register value.
     */
    constexpr std::uint32_t value() const
    {
        return m_value;
    }

private:
    std::uint32_t m_value{};
};

/**
 * CSTS - the Controller Status register, 32 bit and read only.
 *
 * Field positions verified against Linux `include/linux/nvme.h` lines
 * 226-233:
 *
 *     NVME_CSTS_RDY = 1 << 0,          NVME_CSTS_CFS = 1 << 1,
 *     NVME_CSTS_NSSRO = 1 << 4,        NVME_CSTS_PP  = 1 << 5,
 *     NVME_CSTS_SHST_MASK = 3 << 2,
 *
 * and against SPDK's `union spdk_nvme_csts_register` (nvme_spec.h line
 * 163): rdy:1, cfs:1, shst:2, nssro:1, pp:1.
 */
class controller_status
{
public:
    /**
     * Constructs a zero status register value.
     */
    constexpr controller_status() = default;

    /**
     * Constructs a status register from a given value.
     */
    constexpr controller_status(std::uint32_t value) : m_value(value)
    {
    }

    /**
     * RDY - Ready, bit 0. Follows CC.EN, and only once it is set may
     * a command be submitted.
     */
    constexpr bool ready() const
    {
        return m_value & 1;
    }

    /**
     * CFS - Controller Fatal Status, bit 1. Once set the controller
     * is done, and the only recovery is a reset.
     */
    constexpr bool fatal_status() const
    {
        return m_value & (1u << 1);
    }

    /**
     * SHST - Shutdown Status, bits 3:2.
     */
    constexpr nvme::shutdown_status shutdown_status() const
    {
        return static_cast<nvme::shutdown_status>((m_value >> 2) & 0x3);
    }

    /**
     * NSSRO - NVM Subsystem Reset Occurred, bit 4.
     */
    constexpr bool subsystem_reset_occurred() const
    {
        return m_value & (1u << 4);
    }

    /**
     * PP - Processing Paused, bit 5.
     */
    constexpr bool processing_paused() const
    {
        return m_value & (1u << 5);
    }

    /**
     * Returns the whole register value.
     */
    constexpr std::uint32_t value() const
    {
        return m_value;
    }

private:
    std::uint32_t m_value{};
};

/**
 * AQA - the Admin Queue Attributes register, 32 bit.
 *
 * Both size fields are **zero's based**: the value written is one less
 * than the number of entries. Verified against Linux
 * `drivers/nvme/host/pci.c` lines 1796-1799, which build the whole
 * register from a single depth:
 *
 *     aqa = nvmeq->q_depth - 1;
 *     aqa |= aqa << 16;
 *     writel(aqa, dev->bar + NVME_REG_AQA);
 *
 * That also pins ASQS to the low half and ACQS to the high half. The
 * widths - 12 bits each rather than 16 - come from SPDK's `union
 * spdk_nvme_aqa_register` (nvme_spec.h line 192): asqs:12,
 * reserved:4, acqs:12, reserved:4. So the admin queues cap out at 4096
 * entries where an I/O queue caps out at 65536.
 */
class admin_queue_attributes
{
public:
    /**
     * Constructs a zero admin queue attributes value.
     */
    constexpr admin_queue_attributes() = default;

    /**
     * Constructs an admin queue attributes register from a value.
     */
    constexpr admin_queue_attributes(std::uint32_t value) : m_value(value)
    {
    }

    /**
     * ASQS - Admin Submission Queue Size, bits 11:0, zero's based.
     * Returns the raw field.
     */
    constexpr std::uint32_t submission_queue_size_field() const
    {
        return m_value & 0xfff;
    }

    /**
     * ASQS as an entry count, that is, the field plus one.
     */
    constexpr std::uint32_t submission_queue_size() const
    {
        return submission_queue_size_field() + 1;
    }

    /**
     * Sets ASQS from an entry count, subtracting the one that makes
     * the field zero's based. A count of zero is not representable and
     * is not meaningful.
     */
    constexpr void submission_queue_size(std::uint32_t entries)
    {
        m_value = (m_value & ~0xfffu) | ((entries - 1) & 0xfff);
    }

    /**
     * ACQS - Admin Completion Queue Size, bits 27:16, zero's based.
     * Returns the raw field.
     */
    constexpr std::uint32_t completion_queue_size_field() const
    {
        return (m_value >> 16) & 0xfff;
    }

    /**
     * ACQS as an entry count, that is, the field plus one.
     */
    constexpr std::uint32_t completion_queue_size() const
    {
        return completion_queue_size_field() + 1;
    }

    /**
     * Sets ACQS from an entry count, subtracting the one that makes
     * the field zero's based.
     */
    constexpr void completion_queue_size(std::uint32_t entries)
    {
        m_value =
            (m_value & ~(0xfffu << 16)) | (((entries - 1) & 0xfff) << 16);
    }

    /**
     * Returns the whole register value.
     */
    constexpr std::uint32_t value() const
    {
        return m_value;
    }

private:
    std::uint32_t m_value{};
};

/**
 * The spacing, in bytes, between consecutive doorbell registers, given
 * the raw CAP.DSTRD field. The specification writes it as
 * `4 << CAP.DSTRD`, so a DSTRD of 0 gives adjacent 32 bit registers.
 *
 * Verified against Linux `drivers/nvme/host/pci.c` line 2565,
 * `dev->db_stride = 1 << NVME_CAP_STRIDE(dev->ctrl.cap)`, where
 * db_stride counts `u32` elements rather than bytes - the factor of
 * four below is that element size.
 */
constexpr std::uint32_t doorbell_spacing(std::uint32_t doorbell_stride)
{
    return 4u << doorbell_stride;
}

/**
 * The byte offset of SQyTDBL, the tail doorbell of submission queue
 * `queue_id`, from the start of the BAR0 register block.
 *
 * `SQyTDBL = 0x1000 + (2y * (4 << CAP.DSTRD))`
 *
 * Verified against Linux `drivers/nvme/host/pci.c`: line 2566 sets
 * `dev->dbs = dev->bar + 4096` and line 1586 sets
 * `nvmeq->q_db = &dev->dbs[qid * 2 * dev->db_stride]`. Since `dbs` is
 * a `u32 __iomem *`, indexing it multiplies by four, giving
 * `4096 + qid * 2 * (1 << DSTRD) * 4`, which is the formula above.
 *
 * Queue 0 is the admin queue, so passing 0 gives the admin submission
 * queue's doorbell - Linux relies on exactly that at line 2362,
 * `adminq->q_db = dev->dbs`.
 */
constexpr std::uint32_t submission_queue_doorbell_offset(
    std::uint32_t queue_id, std::uint32_t doorbell_stride)
{
    return offset_of(register_offset::doorbell_base) +
           (2 * queue_id) * doorbell_spacing(doorbell_stride);
}

/**
 * The byte offset of CQyHDBL, the head doorbell of completion queue
 * `queue_id`, from the start of the BAR0 register block.
 *
 * `CQyHDBL = 0x1000 + ((2y + 1) * (4 << CAP.DSTRD))`
 *
 * Verified against Linux `drivers/nvme/host/pci.c` line 1003, which
 * rings it as `writel(head, nvmeq->q_db + nvmeq->dev->db_stride)` -
 * one whole stride, in `u32` units, past the submission doorbell of
 * the same queue.
 */
constexpr std::uint32_t completion_queue_doorbell_offset(
    std::uint32_t queue_id, std::uint32_t doorbell_stride)
{
    return offset_of(register_offset::doorbell_base) +
           (2 * queue_id + 1) * doorbell_spacing(doorbell_stride);
}

} // namespace zpp::nvme

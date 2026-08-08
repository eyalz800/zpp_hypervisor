#pragma once
#include <cstdint>

namespace zpp::nvme
{
/**
 * Admin command set opcodes.
 *
 * Verified against Linux `include/linux/nvme.h`, `enum
 * nvme_admin_opcode`:
 *
 *     nvme_admin_delete_sq    = 0x00,
 *     nvme_admin_create_sq    = 0x01,
 *     nvme_admin_get_log_page = 0x02,
 *     nvme_admin_delete_cq    = 0x04,
 *     nvme_admin_create_cq    = 0x05,
 *     nvme_admin_identify     = 0x06,
 *     nvme_admin_abort_cmd    = 0x08,
 *     nvme_admin_set_features = 0x09,
 *     nvme_admin_get_features = 0x0a,
 *     nvme_admin_async_event  = 0x0c,
 *
 * Note 0x03 and 0x07 are absent from that list, and from ours: they
 * are reserved.
 */
enum class admin_opcode : std::uint8_t
{
    delete_io_submission_queue = 0x00,
    create_io_submission_queue = 0x01,
    get_log_page = 0x02,
    delete_io_completion_queue = 0x04,
    create_io_completion_queue = 0x05,
    identify = 0x06,
    abort = 0x08,
    set_features = 0x09,
    get_features = 0x0a,
    asynchronous_event_request = 0x0c,
};

/**
 * NVM command set opcodes - the three this design uses.
 *
 * Verified against Linux `include/linux/nvme.h`, `enum nvme_opcode`:
 * nvme_cmd_flush = 0x00, nvme_cmd_write = 0x01, nvme_cmd_read = 0x02.
 */
enum class nvm_opcode : std::uint8_t
{
    flush = 0x00,
    write = 0x01,
    read = 0x02,
};

/**
 * Feature identifiers, as passed in CDW10 of Get Features and Set
 * Features.
 *
 * Verified against Linux `include/linux/nvme.h` lines 1218-1224
 * (NVME_FEAT_ARBITRATION = 0x01, NVME_FEAT_POWER_MGMT = 0x02,
 * NVME_FEAT_NUM_QUEUES = 0x07) and against SPDK
 * `include/spdk/nvme_spec.h` lines 1878-1890
 * (SPDK_NVME_FEAT_ARBITRATION, _POWER_MANAGEMENT, _NUMBER_OF_QUEUES
 * with the same three values).
 */
enum class feature_identifier : std::uint8_t
{
    arbitration = 0x01,
    power_management = 0x02,
    number_of_queues = 0x07,
};

/**
 * The queue priority written into CDW11 of Create I/O Submission
 * Queue, meaningful only when the weighted round robin arbitration
 * mechanism is selected in CC.AMS.
 *
 * Verified against Linux `include/linux/nvme.h` lines 1214-1217, where
 * the values are spelled pre-shifted by one because they sit at CDW11
 * bits 2:1:
 *
 *     NVME_SQ_PRIO_URGENT = (0 << 1),  NVME_SQ_PRIO_HIGH   = (1 << 1),
 *     NVME_SQ_PRIO_MEDIUM = (2 << 1),  NVME_SQ_PRIO_LOW    = (3 << 1),
 *
 * The values below are unshifted field values; the builder shifts
 * them. SPDK's `create_io_sq` bitfield (nvme_spec.h line 1230) agrees
 * that the field is `qprio : 2` immediately above `pc : 1`.
 */
enum class queue_priority : std::uint8_t
{
    urgent = 0,
    high = 1,
    medium = 2,
    low = 3,
};

/**
 * Status Code Type, CDW3 bits 27:25 of a completion entry.
 *
 * Verified against SPDK `include/spdk/nvme_spec.h` line 1512:
 * SCT_GENERIC = 0x0, SCT_COMMAND_SPECIFIC = 0x1, SCT_MEDIA_ERROR =
 * 0x2, SCT_PATH = 0x3, SCT_VENDOR_SPECIFIC = 0x7. Linux encodes the
 * same split differently - its NVME_SCT_* constants are the type
 * already shifted into bits 10:8 of its 16 bit status word
 * (NVME_SCT_COMMAND_SPECIFIC = 0x100, NVME_SCT_MEDIA_ERROR = 0x200,
 * NVME_SCT_PATH = 0x300) with `#define NVME_SCT(status) ((status) >> 8
 * & 7)` recovering the unshifted value, which is what we store.
 */
enum class status_code_type : std::uint8_t
{
    generic = 0x0,
    command_specific = 0x1,
    media_error = 0x2,
    path = 0x3,
    vendor_specific = 0x7,
};

/**
 * Status codes belonging to status_code_type::generic.
 *
 * Verified against Linux `include/linux/nvme.h` (NVME_SC_SUCCESS =
 * 0x0, NVME_SC_CMD_SEQ_ERROR = 0xc, both under the "Generic Command
 * Status" comment) and SPDK `include/spdk/nvme_spec.h` lines 1524 and
 * 1536 (SPDK_NVME_SC_SUCCESS = 0x00,
 * SPDK_NVME_SC_COMMAND_SEQUENCE_ERROR = 0x0c).
 *
 * A completion is successful only when the type is generic *and* the
 * code is `successful`: code 0x00 in another type means something
 * else entirely - it is Completion Queue Invalid under the command
 * specific type below.
 */
enum class generic_status_code : std::uint8_t
{
    successful = 0x00,
    command_sequence_error = 0x0c,
};

/**
 * Status codes belonging to status_code_type::command_specific.
 *
 * Verified against SPDK `enum
 * spdk_nvme_command_specific_status_code`, `include/spdk/nvme_spec.h`
 * line 1580:
 *
 *     SPDK_NVME_SC_COMPLETION_QUEUE_INVALID     = 0x00,
 *     SPDK_NVME_SC_INVALID_QUEUE_IDENTIFIER     = 0x01,
 *     SPDK_NVME_SC_INVALID_QUEUE_SIZE           = 0x02,
 *     SPDK_NVME_SC_ABORT_COMMAND_LIMIT_EXCEEDED = 0x03,
 *
 * and against Linux `include/linux/nvme.h` lines 1911-1915, which
 * spell the same four values with the type folded into bit 8:
 * NVME_SCT_COMMAND_SPECIFIC = 0x100, NVME_SC_CQ_INVALID = 0x100,
 * NVME_SC_QID_INVALID = 0x101, NVME_SC_QUEUE_SIZE = 0x102,
 * NVME_SC_ABORT_LIMIT = 0x103.
 *
 * **There is no "Maximum Queue Size Exceeded" code at 0x03.** Both
 * references independently give 0x03 as Abort Command Limit Exceeded,
 * and neither contains a status of that name anywhere. The condition
 * it describes - a queue requested larger than CAP.MQES allows - is
 * reported as `invalid_queue_size` (0x02); Linux's own comment naming
 * for 0x102 is NVME_SC_QUEUE_SIZE. Do not add an enumerator at 0x03
 * under the other name: it would silently mean "abort limit" on real
 * hardware.
 */
enum class command_specific_status_code : std::uint8_t
{
    completion_queue_invalid = 0x00,
    invalid_queue_identifier = 0x01,
    invalid_queue_size = 0x02,
    abort_command_limit_exceeded = 0x03,
};

/**
 * Builds CDW0, the first dword common to every submission entry, from
 * an opcode and a command identifier.
 *
 * Layout verified against SPDK `struct spdk_nvme_cmd`,
 * `include/spdk/nvme_spec.h` line 1380:
 *
 *     uint16_t opc  : 8;  // opcode, bits 7:0
 *     uint16_t fuse : 2;  // fused operation, bits 9:8
 *     uint16_t rsvd1: 4;  // bits 13:10
 *     uint16_t psdt : 2;  // PRP or SGL for data transfer, bits 15:14
 *     uint16_t cid;       // command identifier, bits 31:16
 *
 * `fused` and `psdt` default to zero, which means a non-fused command
 * using PRPs - the only shape this design issues.
 */
constexpr std::uint32_t make_command_dword0(std::uint8_t opcode,
                                            std::uint16_t command_id,
                                            std::uint8_t fused = 0,
                                            std::uint8_t psdt = 0)
{
    return std::uint32_t{opcode} |
           (std::uint32_t{static_cast<std::uint8_t>(fused & 0x3)} << 8) |
           (std::uint32_t{static_cast<std::uint8_t>(psdt & 0x3)} << 14) |
           (std::uint32_t{command_id} << 16);
}

/**
 * Builds CDW0 for an admin command.
 */
constexpr std::uint32_t make_command_dword0(admin_opcode opcode,
                                            std::uint16_t command_id)
{
    return make_command_dword0(static_cast<std::uint8_t>(opcode),
                               command_id);
}

/**
 * Builds CDW0 for an NVM command set command.
 */
constexpr std::uint32_t make_command_dword0(nvm_opcode opcode,
                                            std::uint16_t command_id)
{
    return make_command_dword0(static_cast<std::uint8_t>(opcode),
                               command_id);
}

/**
 * A submission queue entry, in the Common Command Format shared by
 * every command of every command set.
 *
 * Verified field by field against SPDK `struct spdk_nvme_cmd`
 * (`include/spdk/nvme_spec.h` line 1380, static asserted there to be
 * 64 bytes) and against Linux `struct nvme_common_command`
 * (`include/linux/nvme.h`), whose members run opcode, flags,
 * command_id, nsid, cdw2[2], metadata, dptr, cdw10..cdw15 - the same
 * order, with CDW0 split into its three parts and the data pointer
 * named rather than spelled as two PRPs.
 *
 * The command specific dwords are left as raw dwords rather than
 * overlaid with per command unions: this header is layout only, and
 * every command this design issues is built by one of the functions
 * below.
 */
struct submission_entry
{
    /**
     * CDW0 - opcode, fused operation, PSDT and command identifier.
     * Build it with make_command_dword0().
     */
    std::uint32_t command_dword0;

    /**
     * CDW1 - NSID, the namespace identifier. Zero when the command is
     * not namespace specific.
     */
    std::uint32_t namespace_id;

    /**
     * CDW2 - command specific, reserved for most commands.
     */
    std::uint32_t command_dword2;

    /**
     * CDW3 - command specific, reserved for most commands.
     */
    std::uint32_t command_dword3;

    /**
     * CDW4-5 - MPTR, the metadata pointer.
     */
    std::uint64_t metadata_pointer;

    /**
     * CDW6-7 - PRP1, the first entry of the data pointer. When PSDT
     * selects SGLs this dword pair is the first half of SGL1 instead.
     */
    std::uint64_t prp_entry_1;

    /**
     * CDW8-9 - PRP2, the second entry of the data pointer.
     */
    std::uint64_t prp_entry_2;

    /**
     * CDW10 - command specific.
     */
    std::uint32_t command_dword10;

    /**
     * CDW11 - command specific.
     */
    std::uint32_t command_dword11;

    /**
     * CDW12 - command specific.
     */
    std::uint32_t command_dword12;

    /**
     * CDW13 - command specific.
     */
    std::uint32_t command_dword13;

    /**
     * CDW14 - command specific.
     */
    std::uint32_t command_dword14;

    /**
     * CDW15 - command specific.
     */
    std::uint32_t command_dword15;

    /**
     * Returns the opcode, CDW0 bits 7:0. Which enumeration it names
     * depends on the queue the entry is submitted to.
     */
    constexpr std::uint8_t opcode() const
    {
        return static_cast<std::uint8_t>(command_dword0 & 0xff);
    }

    /**
     * Returns the command identifier, CDW0 bits 31:16.
     */
    constexpr std::uint16_t command_id() const
    {
        return static_cast<std::uint16_t>(command_dword0 >> 16);
    }

    /**
     * Sets the command identifier, CDW0 bits 31:16. The builders below
     * leave it zero, since the identifier is the submitter's to
     * allocate and must be unique among the commands outstanding on
     * the queue.
     */
    constexpr void command_id(std::uint16_t value)
    {
        command_dword0 =
            (command_dword0 & 0xffffu) | (std::uint32_t{value} << 16);
    }
};

static_assert(sizeof(submission_entry) == 64,
              "a submission queue entry is 64 bytes");

/**
 * A completion queue entry.
 *
 * Verified against SPDK `struct spdk_nvme_cpl`
 * (`include/spdk/nvme_spec.h` line 1447, static asserted there to be
 * 16 bytes): cdw0, cdw1, sqhd:16, sqid:16, cid:16, status:16, where
 * the status half is `struct spdk_nvme_status` - p:1, sc:8, sct:3,
 * crd:2, m:1, dnr:1 - so the phase tag is the *low* bit of that half,
 * DW3 bit 16, and everything above it is the status field.
 *
 * Linux confirms both halves from the driver side: `nvme_cqe_pending`
 * (`drivers/nvme/host/pci.c` line 990) tests
 * `(le16_to_cpu(hcqe->status) & 1) == nvmeq->cq_phase`, and
 * `nvme_try_complete_req` (`drivers/nvme/host/nvme.h` line 723) stores
 * `le16_to_cpu(status) >> 1` - shifting the phase tag off before
 * masking with NVME_SC_MASK (0x00ff) or NVME_SCT (>> 8 & 7).
 */
struct completion_entry
{
    /**
     * DW0 - command specific. Set Features returns its result here.
     */
    std::uint32_t command_specific;

    /**
     * DW1 - reserved.
     */
    std::uint32_t reserved;

    /**
     * DW2 - submission queue head pointer in bits 15:0, submission
     * queue identifier in bits 31:16.
     */
    std::uint32_t queue_information;

    /**
     * DW3 - command identifier in bits 15:0, phase tag at bit 16, and
     * the status field in bits 31:17.
     */
    std::uint32_t status_information;

    /**
     * SQHD - how far the controller has consumed the submission
     * queue, DW2 bits 15:0. Entries below it may be reused.
     */
    constexpr std::uint16_t submission_queue_head() const
    {
        return static_cast<std::uint16_t>(queue_information & 0xffff);
    }

    /**
     * SQID - the submission queue this completion answers, DW2 bits
     * 31:16.
     */
    constexpr std::uint16_t submission_queue_id() const
    {
        return static_cast<std::uint16_t>(queue_information >> 16);
    }

    /**
     * CID - the identifier of the command that completed, DW3 bits
     * 15:0. It is whatever the submitter put in CDW0 bits 31:16.
     */
    constexpr std::uint16_t command_id() const
    {
        return static_cast<std::uint16_t>(status_information & 0xffff);
    }

    /**
     * P - the phase tag, DW3 bit 16. The controller inverts the value
     * it writes each time it wraps the queue, so an entry is new
     * exactly when its phase differs from the phase of the previous
     * pass. This is the only field that may be read before the entry
     * is known to be valid, and it must be read first.
     */
    constexpr bool phase() const
    {
        return status_information & (1u << 16);
    }

    /**
     * The whole status field, DW3 bits 31:17, as a 15 bit value: SC in
     * bits 7:0, SCT in bits 10:8, CRD in 12:11, More at 13 and Do Not
     * Retry at 14. This is the same quantity Linux stores after its
     * `>> 1`, so its NVME_SC_MASK and NVME_SCT() apply to it
     * unchanged.
     */
    constexpr std::uint16_t status() const
    {
        return static_cast<std::uint16_t>(status_information >> 17);
    }

    /**
     * SC - the Status Code, status field bits 7:0. Meaningless without
     * status_code_type(), which says which enumeration it names.
     */
    constexpr std::uint8_t status_code() const
    {
        return static_cast<std::uint8_t>(status() & 0xff);
    }

    /**
     * SCT - the Status Code Type, status field bits 10:8.
     */
    constexpr nvme::status_code_type status_code_type() const
    {
        return static_cast<nvme::status_code_type>((status() >> 8) & 0x7);
    }
};

static_assert(sizeof(completion_entry) == 16,
              "a completion queue entry is 16 bytes");

/**
 * PC - Physically Contiguous, bit 0 of CDW11 of both queue creation
 * commands. This design always allocates its queues as one contiguous
 * physical run and always sets it; a controller reporting CAP.CQR
 * requires that anyway.
 *
 * Verified against Linux `include/linux/nvme.h` line 1212,
 * NVME_QUEUE_PHYS_CONTIG = (1 << 0), and SPDK's `pc : 1` as the lowest
 * bit of both `create_io_sq` and `create_io_cq` in `union
 * spdk_nvme_cmd_cdw11` (nvme_spec.h line 1229 onwards).
 */
inline constexpr std::uint32_t queue_physically_contiguous = 1u << 0;

/**
 * IEN - Interrupts Enabled, bit 1 of CDW11 of Create I/O Completion
 * Queue.
 *
 * Verified against Linux `include/linux/nvme.h` line 1213,
 * NVME_CQ_IRQ_ENABLED = (1 << 1).
 */
inline constexpr std::uint32_t completion_queue_interrupts_enabled = 1u
                                                                     << 1;

/**
 * Builds a Create I/O Completion Queue command.
 *
 * `entries` is a real entry count, not the encoded field: **QSIZE is
 * zero's based**, so CDW10 carries `entries - 1`. Verified against
 * Linux `drivers/nvme/host/pci.c` line 1208,
 * `c.create_cq.qsize = cpu_to_le16(nvmeq->q_depth - 1)`. Passing zero
 * is not meaningful and would underflow.
 *
 * CDW10 = QID (bits 15:0) | (entries - 1) << 16, and CDW11 = PC (bit
 * 0) | IEN (bit 1) | IV << 16. Layout verified against SPDK's
 * `create_io_q` in `union spdk_nvme_cmd_cdw10` (nvme_spec.h line 1129:
 * qid:16, qsize:16) and `create_io_cq` in `union spdk_nvme_cmd_cdw11`
 * (line 1239: pc:1, ien:1, reserved:14, iv:16), and against the field
 * order of Linux `struct nvme_create_cq` - prp1, cqid, qsize,
 * cq_flags, irq_vector - which packs into exactly those two dwords.
 *
 * `physical_address` is the physical base of the queue memory and goes
 * in PRP1, per `c.create_cq.prp1 = cpu_to_le64(nvmeq->cq_dma_addr)`
 * at pci.c line 1206.
 */
constexpr submission_entry
create_io_completion_queue(std::uint16_t queue_id,
                           std::uint16_t entries,
                           std::uint64_t physical_address,
                           bool interrupts_enabled,
                           std::uint16_t interrupt_vector)
{
    submission_entry entry{};
    entry.command_dword0 =
        make_command_dword0(admin_opcode::create_io_completion_queue, 0);
    entry.prp_entry_1 = physical_address;
    entry.command_dword10 =
        std::uint32_t{queue_id} |
        (std::uint32_t{static_cast<std::uint16_t>(entries - 1)} << 16);
    entry.command_dword11 =
        queue_physically_contiguous |
        (interrupts_enabled ? completion_queue_interrupts_enabled : 0) |
        (std::uint32_t{interrupt_vector} << 16);
    return entry;
}

/**
 * Builds a Create I/O Submission Queue command.
 *
 * As above, **QSIZE in CDW10 is zero's based** and carries
 * `entries - 1`; verified against Linux `drivers/nvme/host/pci.c` line
 * 1237, `c.create_sq.qsize = cpu_to_le16(nvmeq->q_depth - 1)`.
 *
 * CDW10 = QID (bits 15:0) | (entries - 1) << 16, and CDW11 = PC (bit
 * 0) | QPRIO << 1 | CQID << 16. Layout verified against SPDK's
 * `create_io_sq` in `union spdk_nvme_cmd_cdw11` (nvme_spec.h line
 * 1229: pc:1, qprio:2, reserved:13, cqid:16), and against the field
 * order of Linux `struct nvme_create_sq` - prp1, sqid, qsize,
 * sq_flags, cqid.
 *
 * The completion queue named by `completion_queue_id` must already
 * exist; creating a submission queue against one that does not is what
 * `command_specific_status_code::completion_queue_invalid` reports.
 */
constexpr submission_entry
create_io_submission_queue(std::uint16_t queue_id,
                           std::uint16_t entries,
                           std::uint64_t physical_address,
                           std::uint16_t completion_queue_id,
                           queue_priority priority)
{
    submission_entry entry{};
    entry.command_dword0 =
        make_command_dword0(admin_opcode::create_io_submission_queue, 0);
    entry.prp_entry_1 = physical_address;
    entry.command_dword10 =
        std::uint32_t{queue_id} |
        (std::uint32_t{static_cast<std::uint16_t>(entries - 1)} << 16);
    entry.command_dword11 =
        queue_physically_contiguous |
        ((std::uint32_t{static_cast<std::uint8_t>(priority)} & 0x3) << 1) |
        (std::uint32_t{completion_queue_id} << 16);
    return entry;
}

/**
 * Builds a Get Features command with no data pointer, which is the
 * form used for every feature whose value fits in the completion's
 * DW0.
 *
 * CDW10 = FID (bits 7:0) | SEL << 8. Verified against SPDK's
 * `get_features` in `union spdk_nvme_cmd_cdw10`
 * (`include/spdk/nvme_spec.h` line 1143: fid:8, sel:3, reserved:21)
 * and against the field order of Linux `struct nvme_features` -
 * dptr, fid, dword11 - which places FID at CDW10.
 *
 * Only the *width* of SEL was verified, three bits; the meaning of its
 * individual values (current, default, saved, supported capabilities)
 * was not looked up, so it is taken as a plain number rather than an
 * enumeration whose spellings would be guesses.
 */
constexpr submission_entry get_features(feature_identifier feature,
                                        std::uint8_t select)
{
    submission_entry entry{};
    entry.command_dword0 =
        make_command_dword0(admin_opcode::get_features, 0);
    entry.command_dword10 =
        std::uint32_t{static_cast<std::uint8_t>(feature)} |
        ((std::uint32_t{select} & 0x7) << 8);
    return entry;
}

/**
 * Builds a Set Features command for the Number of Queues feature.
 *
 * CDW10 = FID (0x07). CDW11 = NSQR (bits 15:0) | NCQR << 16, whose
 * layout is verified against SPDK's `union
 * spdk_nvme_feat_number_of_queues` (`include/spdk/nvme_spec.h` line
 * 804: nsqr:16, ncqr:16).
 *
 * **Both halves are zero's based, and this is the single easiest
 * value in the whole design to get wrong by one.** The field holds one
 * *less* than the number of queues wanted, so asking for four
 * submission queues writes 3. This function takes real counts and does
 * the subtraction, so callers never see the encoding; passing zero is
 * not meaningful and would underflow.
 *
 * Verified against Linux `drivers/nvme/host/core.c` line 1685, which
 * encodes both halves from one count:
 *
 *     u32 q_count = (*count - 1) | ((*count - 1) << 16);
 *     status = nvme_set_features(ctrl, NVME_FEAT_NUM_QUEUES, q_count,
 *                                NULL, 0, &result);
 *
 * The same encoding applies to the *answer*: the controller reports
 * what it granted in the completion's DW0, also zero's based, which is
 * why line 1703 reads it back as
 * `min(result & 0xffff, result >> 16) + 1`. The grant may be smaller
 * than the request and must be honoured.
 */
constexpr submission_entry set_features_number_of_queues(
    std::uint16_t submission_queues, std::uint16_t completion_queues)
{
    submission_entry entry{};
    entry.command_dword0 =
        make_command_dword0(admin_opcode::set_features, 0);
    entry.command_dword10 = std::uint32_t{
        static_cast<std::uint8_t>(feature_identifier::number_of_queues)};
    entry.command_dword11 =
        std::uint32_t{static_cast<std::uint16_t>(submission_queues - 1)} |
        (std::uint32_t{static_cast<std::uint16_t>(completion_queues - 1)}
         << 16);
    return entry;
}

/**
 * Builds a Set Features command asking for the largest Number of Queues
 * allocation this controller will give.
 *
 * Both halves of CDW11 are zero's based, so 0xffff in each asks for
 * 65536 queues - more than any controller has. Asking for more than
 * exists is the intended use rather than an abuse: the grant is reported
 * back in DW0 of the completion and may be smaller than the request,
 * which is why Linux reads it as
 * `min(result & 0xffff, result >> 16) + 1` and honours it
 * (`nvme_set_queue_count`, drivers/nvme/host/core.c, quoted at
 * core.c:1703 in the header comment on set_features_number_of_queues
 * above). So the answer to this is "everything you can have", stated in
 * one command.
 *
 * Why the *maximum* rather than a number: the allocation is frozen by
 * the first Set Features completed after a controller level reset and
 * cannot be raised afterwards - NVMe Base 5.2.30.1.5, quoted verbatim in
 * NVME-LOG.md, "After that first successful Set Features command, the
 * number of I/O queues allocated shall not change until a CLR occurs".
 * A reservation therefore gets exactly one attempt per reset and has no
 * way to ask again, so it asks for everything and chooses an identifier
 * afterwards from what the guest is then seen to create.
 *
 * Spelled as its own function rather than as
 * `set_features_number_of_queues(0, 0)`: that one takes real counts and
 * subtracts one, so zero would underflow into the right bits by
 * accident - a thing that reads as a bug for ever afterwards.
 */
constexpr submission_entry set_features_maximum_number_of_queues()
{
    submission_entry entry{};
    entry.command_dword0 =
        make_command_dword0(admin_opcode::set_features, 0);
    entry.command_dword10 = std::uint32_t{
        static_cast<std::uint8_t>(feature_identifier::number_of_queues)};
    entry.command_dword11 = 0xffffffffu;
    return entry;
}

/**
 * Reads a Number of Queues grant or request out of the dword that
 * carries it - DW0 of a Set Features completion, or CDW11 of the
 * command.
 *
 * The same encoding serves both, which is the whole reason these are
 * one pair of functions: NSQ in bits 15:0 and NCQ in bits 31:16, both
 * zero's based. Verified against SPDK's
 * `union spdk_nvme_feat_number_of_queues` (nsqr:16, ncqr:16) as cited on
 * set_features_number_of_queues above, and against Linux encoding a
 * request as `(*count - 1) | ((*count - 1) << 16)` and decoding a grant
 * as `min(result & 0xffff, result >> 16) + 1`.
 *
 * They return real counts, so a caller never sees the zero's base. The
 * count is at least one for every value, which is correct: a controller
 * that reports zero has granted one queue, not none.
 * @{
 */
constexpr std::uint32_t number_of_submission_queues(std::uint32_t value)
{
    return (value & 0xffffu) + 1;
}

constexpr std::uint32_t number_of_completion_queues(std::uint32_t value)
{
    return ((value >> 16) & 0xffffu) + 1;
}
/**
 * @}
 */

/**
 * FUA - Force Unit Access, CDW12 bit 30 of a Read or a Write. When set
 * the command does not complete until the data is on non-volatile
 * media.
 *
 * Verified against Linux `include/linux/nvme.h` line 996,
 * NVME_RW_FUA = 1 << 14, where the shift is relative to the 16 bit
 * `control` field of `struct nvme_rw_command`; `control` is the upper
 * half of CDW12 (the struct runs slba, length:16, control:16), so
 * bit 14 of control is bit 30 of CDW12.
 */
inline constexpr std::uint32_t read_write_force_unit_access = 1u << 30;

/**
 * LR - Limited Retry, CDW12 bit 31. Verified the same way, from
 * NVME_RW_LR = 1 << 15 at Linux `include/linux/nvme.h` line 995.
 * Neither builder below sets it: a limited-retry read that gives up
 * early is not what a boot path wants.
 */
inline constexpr std::uint32_t read_write_limited_retry = 1u << 31;

/**
 * Builds an NVM Write command.
 *
 * CDW10 and CDW11 together hold SLBA, the 64 bit starting logical
 * block address. CDW12 holds NLB in bits 15:0 and the control field in
 * bits 31:16. Layout verified against Linux `struct nvme_rw_command`
 * (`include/linux/nvme.h`), whose members after the data pointer are
 * `__le64 slba; __le16 length; __le16 control;`.
 *
 * **NLB is zero's based**: it holds one less than the number of
 * logical blocks to transfer, so a single block transfer writes 0.
 * Verified against Linux `drivers/nvme/host/core.c` line 997:
 *
 *     cmnd->rw.slba = cpu_to_le64(nvme_sect_to_lba(...));
 *     cmnd->rw.length =
 *         cpu_to_le16((blk_rq_bytes(req) >> lba_shift) - 1);
 *
 * This function takes a real block count and subtracts the one, so a
 * `block_count` of zero is not meaningful and would underflow. Note
 * the field is 16 bits, capping one command at 65536 blocks.
 *
 * `physical_address` is the physical base of the data buffer and goes
 * in PRP1. PRP2 is left zero, which is only correct when the transfer
 * fits within what PRP1 alone can describe - one memory page from the
 * offset PRP1 points at. The caller owns that constraint; this header
 * has no way to check it.
 */
constexpr submission_entry write(std::uint32_t namespace_id,
                                 std::uint64_t starting_lba,
                                 std::uint16_t block_count,
                                 std::uint64_t physical_address,
                                 bool force_unit_access)
{
    submission_entry entry{};
    entry.command_dword0 = make_command_dword0(nvm_opcode::write, 0);
    entry.namespace_id = namespace_id;
    entry.prp_entry_1 = physical_address;
    entry.command_dword10 =
        static_cast<std::uint32_t>(starting_lba & 0xffffffffu);
    entry.command_dword11 = static_cast<std::uint32_t>(starting_lba >> 32);
    entry.command_dword12 =
        std::uint32_t{static_cast<std::uint16_t>(block_count - 1)} |
        (force_unit_access ? read_write_force_unit_access : 0);
    return entry;
}

/**
 * Builds an NVM Read command. Identical in shape to write() above -
 * same SLBA split across CDW10 and CDW11, same zero's based NLB in
 * CDW12, same PRP1 caveat - differing only in the opcode, and in
 * having no Force Unit Access parameter, since on a read FUA asks the
 * controller to bypass its cache rather than to flush it and this
 * design has no reason to ask for that.
 */
constexpr submission_entry read(std::uint32_t namespace_id,
                                std::uint64_t starting_lba,
                                std::uint16_t block_count,
                                std::uint64_t physical_address)
{
    submission_entry entry{};
    entry.command_dword0 = make_command_dword0(nvm_opcode::read, 0);
    entry.namespace_id = namespace_id;
    entry.prp_entry_1 = physical_address;
    entry.command_dword10 =
        static_cast<std::uint32_t>(starting_lba & 0xffffffffu);
    entry.command_dword11 = static_cast<std::uint32_t>(starting_lba >> 32);
    entry.command_dword12 =
        std::uint32_t{static_cast<std::uint16_t>(block_count - 1)};
    return entry;
}

} // namespace zpp::nvme

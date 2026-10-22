// SPDX-License-Identifier: MIT
#include "shader_container.h"
#include "log.h"

#include <d3d12TokenizedProgramFormat.hpp>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace vgd::umd {

namespace {

constexpr uint32_t tag(const char (&s)[5])
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// D3D_NAME values with no D3D10_SB_NAME counterpart: what pixel shaders write.
constexpr uint32_t kNameTarget = 64, kNameDepth = 65;

// Program types, from the version token (DXBC) or ProgramVersion (DXIL): the same high 16 bits.
enum : uint32_t { kPixel = 0, kVertex = 1, kGeometry = 2, kHull = 3, kDomain = 4, kCompute = 5 };

struct Semantic {
    const char* name;
    UINT index;
};

// The SV_ name of a system value; null if it has none we know of. `nth` counts earlier entries of the same system
// value in the signature (clip and cull distances span registers).
const char* system_value_name(UINT sv, UINT nth, UINT* index)
{
    *index = 0;
    switch (sv) {
    case D3D10_SB_NAME_POSITION: return "SV_Position";
    case D3D10_SB_NAME_CLIP_DISTANCE: *index = nth; return "SV_ClipDistance";
    case D3D10_SB_NAME_CULL_DISTANCE: *index = nth; return "SV_CullDistance";
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: return "SV_RenderTargetArrayIndex";
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: return "SV_ViewportArrayIndex";
    case D3D10_SB_NAME_VERTEX_ID: return "SV_VertexID";
    case D3D10_SB_NAME_PRIMITIVE_ID: return "SV_PrimitiveID";
    case D3D10_SB_NAME_INSTANCE_ID: return "SV_InstanceID";
    case D3D10_SB_NAME_IS_FRONT_FACE: return "SV_IsFrontFace";
    case D3D10_SB_NAME_SAMPLE_INDEX: return "SV_SampleIndex";
    // Tessellation factors: one per edge or inside, numbered as the HLSL arrays are.
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR: return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR: *index = 2; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR: *index = 3; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR: return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR: *index = 1; return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR: return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR: *index = 2; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR: return "SV_InsideTessFactor";
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR: *index = 1; return "SV_TessFactor";
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR: return "SV_TessFactor";
    default: return nullptr;
    }
}

UINT first_component(BYTE mask)
{
    for (UINT c = 0; c < 4; c++)
        if (mask & (1u << c))
            return c;
    return 0;
}

struct Named {
    const char* name;
    UINT index;
    uint32_t sysval;   // D3D_NAME
};

// The semantic of entries[i] (see shader_container.h).
Named semantic_of(const std::vector<SignatureEntry>& entries, UINT i, bool pixel_output)
{
    const auto& e = entries[i];
    UINT nth = 0;
    for (UINT j = 0; j < i; j++)
        nth += entries[j].system_value == e.system_value;
    Named n{nullptr, 0, uint32_t(e.system_value)};
    if ((n.name = system_value_name(e.system_value, nth, &n.index)))
        return n;
    if (pixel_output && e.reg != ~0u)
        return {"SV_Target", e.reg, kNameTarget};
    if (pixel_output)
        return {"SV_Depth", 0, kNameDepth};   // oDepth and friends are declared by register type; the name is unread
    return {generic_semantic(e.stream), generic_semantic_index(e.reg, first_component(e.mask)), 0};
}

// --- Component types from the program's own instructions --------------------------------------------------------
// fxc types each signature entry by its HLSL declaration; the runtime does not pass that on. Where it matters to
// Vulkan, the type must match what the register is bound to: vertex shader inputs match their vertex attribute
// formats and pixel shader outputs their render targets. Values between stages are only passed along, and float
// on both sides carries any bits (integer ones are flat). So the types of those two are read off how the program
// uses each component: a vertex input it adds as an integer is an integer, as fxc declared it.

// How an instruction treats one operand: float, signed, unsigned, or an integer either way.
enum Use : uint8_t { kNoUse, kFloat, kSigned, kUnsigned, kInteger };

struct Uses {
    // Per register: the components used each way.
    uint8_t mask[32][5];

    void add(UINT reg, uint8_t components, Use u)
    {
        if (reg < 32 && u != kNoUse)
            mask[reg][u] |= components;
    }
};

// The use operand `i` of `op` gets (destinations first). Instructions not listed type nothing: moves and selects
// pass values on untyped, declarations and flow control are not uses.
Use operand_use(UINT op, UINT i)
{
    switch (op) {
    case D3D10_SB_OPCODE_ADD:
    case D3D10_SB_OPCODE_DIV:
    case D3D10_SB_OPCODE_DP2:
    case D3D10_SB_OPCODE_DP3:
    case D3D10_SB_OPCODE_DP4:
    case D3D10_SB_OPCODE_EXP:
    case D3D10_SB_OPCODE_FRC:
    case D3D10_SB_OPCODE_LOG:
    case D3D10_SB_OPCODE_MAD:
    case D3D10_SB_OPCODE_MIN:
    case D3D10_SB_OPCODE_MAX:
    case D3D10_SB_OPCODE_MUL:
    case D3D10_SB_OPCODE_ROUND_NE:
    case D3D10_SB_OPCODE_ROUND_NI:
    case D3D10_SB_OPCODE_ROUND_PI:
    case D3D10_SB_OPCODE_ROUND_Z:
    case D3D10_SB_OPCODE_RSQ:
    case D3D10_SB_OPCODE_SQRT:
    case D3D10_SB_OPCODE_SINCOS:
    case D3D11_SB_OPCODE_RCP:
    case D3D10_SB_OPCODE_DERIV_RTX:
    case D3D10_SB_OPCODE_DERIV_RTY:
    case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTX_FINE:
    case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
    case D3D11_SB_OPCODE_DERIV_RTY_FINE: return kFloat;
    // Comparisons write masks, not values of the compared type.
    case D3D10_SB_OPCODE_EQ:
    case D3D10_SB_OPCODE_GE:
    case D3D10_SB_OPCODE_LT:
    case D3D10_SB_OPCODE_NE: return i ? kFloat : kNoUse;
    case D3D10_SB_OPCODE_IGE:
    case D3D10_SB_OPCODE_ILT: return i ? kSigned : kNoUse;
    case D3D10_SB_OPCODE_UGE:
    case D3D10_SB_OPCODE_ULT: return i ? kUnsigned : kNoUse;
    case D3D10_SB_OPCODE_IEQ:
    case D3D10_SB_OPCODE_INE: return i ? kInteger : kNoUse;
    case D3D10_SB_OPCODE_FTOI: return i ? kFloat : kSigned;
    case D3D10_SB_OPCODE_FTOU: return i ? kFloat : kUnsigned;
    case D3D10_SB_OPCODE_ITOF: return i ? kSigned : kFloat;
    case D3D10_SB_OPCODE_UTOF: return i ? kUnsigned : kFloat;
    case D3D11_SB_OPCODE_F32TOF16: return i ? kFloat : kUnsigned;
    case D3D11_SB_OPCODE_F16TOF32: return i ? kUnsigned : kFloat;
    case D3D10_SB_OPCODE_IMAD:
    case D3D10_SB_OPCODE_IMAX:
    case D3D10_SB_OPCODE_IMIN:
    case D3D10_SB_OPCODE_INEG:
    case D3D10_SB_OPCODE_ISHR:
    case D3D11_SB_OPCODE_IBFE: return kSigned;
    case D3D10_SB_OPCODE_IMUL: return kSigned;   // two destinations, both signed
    case D3D10_SB_OPCODE_UMAD:
    case D3D10_SB_OPCODE_UMAX:
    case D3D10_SB_OPCODE_UMIN:
    case D3D10_SB_OPCODE_USHR:
    case D3D10_SB_OPCODE_UDIV:
    case D3D10_SB_OPCODE_UMUL:
    case D3D11_SB_OPCODE_UADDC:
    case D3D11_SB_OPCODE_USUBB:
    case D3D11_SB_OPCODE_UBFE:
    case D3D11_SB_OPCODE_BFI:
    case D3D11_SB_OPCODE_BFREV:
    case D3D11_SB_OPCODE_COUNTBITS:
    case D3D11_SB_OPCODE_FIRSTBIT_HI:
    case D3D11_SB_OPCODE_FIRSTBIT_LO:
    case D3D11_SB_OPCODE_FIRSTBIT_SHI: return kUnsigned;
    case D3D10_SB_OPCODE_IADD:
    case D3D10_SB_OPCODE_ISHL:
    case D3D10_SB_OPCODE_AND:
    case D3D10_SB_OPCODE_OR:
    case D3D10_SB_OPCODE_XOR:
    case D3D10_SB_OPCODE_NOT:
    case D3D10_SB_OPCODE_SWITCH: return kInteger;
    // Addresses: the operand after the destination.
    case D3D10_SB_OPCODE_LD:
    case D3D10_SB_OPCODE_LD_MS:
    case D3D11_SB_OPCODE_LD_UAV_TYPED:
    case D3D11_SB_OPCODE_LD_RAW:
    case D3D11_SB_OPCODE_LD_STRUCTURED: return i == 1 ? kInteger : kNoUse;
    case D3D10_SB_OPCODE_SAMPLE:
    case D3D10_SB_OPCODE_SAMPLE_C:
    case D3D10_SB_OPCODE_SAMPLE_C_LZ:
    case D3D10_SB_OPCODE_SAMPLE_L:
    case D3D10_SB_OPCODE_SAMPLE_D:
    case D3D10_SB_OPCODE_SAMPLE_B:
    case D3D10_1_SB_OPCODE_GATHER4:
    case D3D11_SB_OPCODE_GATHER4_C:
    case D3D10_1_SB_OPCODE_LOD: return i == 1 ? kFloat : kNoUse;
    default: return kNoUse;
    }
}

UINT destination_count(UINT op)
{
    switch (op) {
    case D3D10_SB_OPCODE_IMUL:
    case D3D10_SB_OPCODE_UMUL:
    case D3D10_SB_OPCODE_UDIV:
    case D3D11_SB_OPCODE_UADDC:
    case D3D11_SB_OPCODE_USUBB: return 2;
    default: return 1;
    }
}

// The components an operand token reads (swizzles, selects) or writes (masks).
uint8_t operand_components(UINT token)
{
    if (DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) != D3D10_SB_OPERAND_4_COMPONENT)
        return 1;
    switch (DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token)) {
    case D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE: return uint8_t(DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token) >> 4);
    case D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE: {
        uint8_t m = 0;
        for (UINT c = 0; c < 4; c++)
            m |= uint8_t(1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_SOURCE(token, c));
        return m;
    }
    default: return uint8_t(1u << DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(token));
    }
}

// Parses the operand at code[p] (ending before `end`), recording a use of `file` registers; returns the word
// after it, or `end` if it runs past. Relative indices are operands of their own, used as integers.
UINT scan_operand(const UINT* code, UINT p, UINT end, UINT file, Use use, Uses& uses)
{
    if (p >= end)
        return end;
    const UINT token = code[p++];
    for (UINT ext = token; DECODE_IS_D3D10_SB_OPERAND_EXTENDED(ext) && p < end;)
        ext = code[p++];
    const UINT type = DECODE_D3D10_SB_OPERAND_TYPE(token);
    const UINT dims = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
    UINT first_index = ~0u;
    for (UINT d = 0; d < dims && p < end; d++) {
        const UINT rep = DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, token);
        if (rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 || rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE) {
            if (d == 0)
                first_index = code[p];
            p++;
        } else if (rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE64 || rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE) {
            p += 2;
        }
        if (rep == D3D10_SB_OPERAND_INDEX_RELATIVE || rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE ||
            rep == D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE)
            p = scan_operand(code, p, end, file, kInteger, uses);
    }
    if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64) {
        const UINT n = DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 1;
        p += n * (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64 ? 2 : 1);
    }
    // Only one-dimensional files: vertex inputs and pixel outputs (indexed ones are not typed by use).
    if (type == file && dims == 1 && first_index != ~0u)
        uses.add(first_index, operand_components(token), use);
    return p < end ? p : end;
}

// How the program uses the registers of `file` (D3D10_SB_OPERAND_TYPE_INPUT or _OUTPUT), as sources or as
// destinations.
Uses scan_uses(const UINT* code, UINT file, bool destinations)
{
    Uses uses{};
    const UINT length = code[1];
    for (UINT pos = 2; pos < length;) {
        const UINT token = code[pos];
        const UINT op = DECODE_D3D10_SB_OPCODE_TYPE(token);
        const UINT len = op == D3D10_SB_OPCODE_CUSTOMDATA ? (pos + 1 < length ? code[pos + 1] : 0)
                                                           : DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
        if (len == 0 || pos + len > length)
            break;
        if (op != D3D10_SB_OPCODE_CUSTOMDATA && operand_use(op, 0) + operand_use(op, 1) + operand_use(op, 2) != 0) {
            const UINT end = pos + len;
            UINT p = pos + 1;
            for (UINT ext = token; DECODE_IS_D3D10_SB_OPCODE_EXTENDED(ext) && p < end;)
                ext = code[p++];
            const UINT dsts = destination_count(op);
            for (UINT i = 0; p < end; i++) {
                const bool dst = i < dsts;
                p = scan_operand(code, p, end, file, dst == destinations ? operand_use(op, i) : kNoUse, uses);
            }
        }
        pos += len;
    }
    return uses;
}

// The type of the entry's components by `uses`; 0 (unknown) if nothing types them. A component used as float
// anywhere stays float (an integer reinterpreted, asfloat()); signed wins over unsigned only when nothing reads it
// unsigned.
uint32_t type_by_use(const Uses& uses, const SignatureEntry& e)
{
    if (e.reg >= 32)
        return D3D10_SB_REGISTER_COMPONENT_UNKNOWN;
    const uint8_t* m = uses.mask[e.reg];
    if (m[kFloat] & e.mask)
        return D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    if ((m[kSigned] & e.mask) && !(m[kUnsigned] & e.mask))
        return D3D10_SB_REGISTER_COMPONENT_SINT32;
    if ((m[kUnsigned] | m[kInteger]) & e.mask)
        return D3D10_SB_REGISTER_COMPONENT_UINT32;
    return D3D10_SB_REGISTER_COMPONENT_UNKNOWN;
}

// Types the generic entries of `entries` by `uses` where the runtime left them unknown.
void type_by_use(std::vector<SignatureEntry>& entries, const Uses& uses)
{
    for (auto& e : entries)
        if (e.component_type == D3D10_SB_REGISTER_COMPONENT_UNKNOWN && e.system_value == D3D10_SB_NAME_UNDEFINED)
            e.component_type = type_by_use(uses, e);
}

// The entry's component type. The D3D11 runtime passes 0 (unknown) for every entry; a container must not, or
// dxbc-spirv types the I/O void and builds broken programs (DXVK's CS thread threw at DWM's first draw, RADV
// failed in spirv_to_nir). Entries typed by use above keep theirs; integer system values are uint as fxc declares
// them, everything else float.
uint32_t component_type_of(const SignatureEntry& e)
{
    if (e.component_type != D3D10_SB_REGISTER_COMPONENT_UNKNOWN)
        return e.component_type;
    switch (e.system_value) {
    case D3D10_SB_NAME_VERTEX_ID:
    case D3D10_SB_NAME_PRIMITIVE_ID:
    case D3D10_SB_NAME_INSTANCE_ID:
    case D3D10_SB_NAME_IS_FRONT_FACE:
    case D3D10_SB_NAME_SAMPLE_INDEX:
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX: return D3D10_SB_REGISTER_COMPONENT_UINT32;
    default: return D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    }
}

// One ISG1/OSG1/PSG1 part: count, 8, then per entry {stream, name offset, semantic index, system value, component
// type, register, mask | used mask << 8, min precision}, then the names; offsets count from the part's data.
std::vector<uint8_t> signature_part(const std::vector<SignatureEntry>& entries, bool pixel_output)
{
    const UINT count = static_cast<UINT>(entries.size());
    std::vector<uint32_t> words{count, 8};
    std::string names;
    std::vector<size_t> name_at(count);
    for (UINT i = 0; i < count; i++) {
        const auto& e = entries[i];
        const Named n = semantic_of(entries, i, pixel_output);
        name_at[i] = names.size();
        names += n.name;
        names += '\0';
        words.insert(words.end(), {e.stream, 0, n.index, n.sysval, component_type_of(e), e.reg,
                                   uint32_t(e.mask) | uint32_t(e.mask) << 8, e.min_precision});
    }
    const size_t names_offset = words.size() * 4;
    for (UINT i = 0; i < count; i++)
        words[2 + i * 8 + 1] = static_cast<uint32_t>(names_offset + name_at[i]);
    std::vector<uint8_t> part(names_offset + names.size());
    std::memcpy(part.data(), words.data(), names_offset);
    std::memcpy(part.data() + names_offset, names.data(), names.size());
    part.resize((part.size() + 3) & ~size_t(3));
    return part;
}

struct Part {
    uint32_t tag;
    std::vector<uint8_t> data;
};

// Header: "DXBC", checksum, version 1, total size, part count, part offsets; each part: tag, size, data.
void assemble(const std::vector<Part>& parts, DxbcChecksum checksum, std::vector<uint8_t>* out)
{
    size_t size = 32 + 4 * parts.size();
    for (const auto& p : parts)
        size += 8 + p.data.size();
    std::vector<uint8_t>& c = *out;
    c.assign(size, 0);
    auto put = [&](size_t at, uint32_t v) { std::memcpy(c.data() + at, &v, 4); };
    put(0, tag("DXBC"));
    put(20, 1);
    put(24, static_cast<uint32_t>(size));
    put(28, static_cast<uint32_t>(parts.size()));
    size_t at = 32 + 4 * parts.size();
    for (size_t i = 0; i < parts.size(); i++) {
        put(32 + 4 * i, static_cast<uint32_t>(at));
        put(at, parts[i].tag);
        put(at + 4, static_cast<uint32_t>(parts[i].data.size()));
        std::memcpy(c.data() + at + 8, parts[i].data.data(), parts[i].data.size());
        at += 8 + parts[i].data.size();
    }
    uint32_t sum[4];
    checksum(c.data(), c.size(), sum);
    std::memcpy(c.data() + 4, sum, sizeof(sum));
}

// With VGD_SHADER_DUMP naming a folder: each container goes there as <checksum>.dxbc, for offline compiles
// (dxbc-spirv's dxbc_disasm and dxbc_compiler).
void dump(const std::vector<uint8_t>& container)
{
    static char dir[MAX_PATH];
    static const bool enabled = setting("VGD_SHADER_DUMP", dir, sizeof(dir));
    if (!enabled || container.size() < 20)
        return;
    char path[MAX_PATH + 48];
    int len = std::snprintf(path, sizeof(path), "%s\\", dir);
    for (size_t i = 4; i < 20 && len < int(sizeof(path)) - 8; i++)
        len += std::snprintf(path + len, sizeof(path) - len, "%02x", container[i]);
    std::snprintf(path + len, sizeof(path) - len, ".dxbc");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;   // already dumped, or the folder is missing
    DWORD written;
    WriteFile(f, container.data(), static_cast<DWORD>(container.size()), &written, nullptr);
    CloseHandle(f);
}

} // namespace

const char* generic_semantic(UINT stream)
{
    // Stream-output entries are matched by name alone, so geometry shader streams 1 to 3 keep names of their own.
    static const char* const names[] = {"VGD_R", "VGD_S1R", "VGD_S2R", "VGD_S3R"};
    return names[stream < 4 ? stream : 0];
}

bool output_semantic(const std::vector<SignatureEntry>& output, UINT stream, UINT reg, BYTE mask, const char** name,
                     UINT* index, UINT* first)
{
    for (UINT i = 0; i < output.size(); i++) {
        const auto& e = output[i];
        if (e.stream != stream || e.reg != reg || !(e.mask & mask))
            continue;
        const Named n = semantic_of(output, i, false);
        *name = n.name;
        *index = n.index;
        *first = first_component(e.mask);
        return true;
    }
    return false;
}

void build_input_signature_container(const std::vector<SignatureEntry>& input, DxbcChecksum checksum,
                                     std::vector<uint8_t>* out)
{
    assemble({{tag("ISG1"), signature_part(input, false)}}, checksum, out);
}

HRESULT build_shader_container(const UINT* code, const ShaderSignatures& s, DxbcChecksum checksum,
                               std::vector<uint8_t>* out)
{
    // Both program forms keep their size in 32-bit words in the second word:
    //   DXBC: {version token, length in words, tokens...}
    //   DXIL: {ProgramVersion, size in words, "DXIL", DXIL version, bitcode offset, bitcode size, bitcode}
    if (!code || code[1] < 2 || code[1] > (16u << 20))
        return E_INVALIDARG;
    const size_t program_size = size_t(code[1]) * 4;
    const bool dxil = program_size >= 24 && code[2] == tag("DXIL");
    const uint32_t type = code[0] >> 16;
    const uint32_t major = dxil ? 6 : (code[0] >> 4) & 0xf;
    if (type > kCompute) {
        VGD_WARN("shader: program type %u not supported (version 0x%08x)", type, code[0]);
        return E_INVALIDARG;
    }

    std::vector<SignatureEntry> input = s.input, output = s.output;
    if (!dxil && type == kVertex)
        type_by_use(input, scan_uses(code, D3D10_SB_OPERAND_TYPE_INPUT, false));
    if (!dxil && type == kPixel)
        type_by_use(output, scan_uses(code, D3D10_SB_OPERAND_TYPE_OUTPUT, true));

    std::vector<Part> parts;
    if (type != kCompute) {
        parts.push_back({tag("ISG1"), signature_part(input, false)});
        parts.push_back({tag("OSG1"), signature_part(output, type == kPixel)});
        if (type == kHull || type == kDomain)
            parts.push_back({tag("PSG1"), signature_part(s.patch_constant, false)});
    }
    Part program{dxil ? tag("DXIL") : major >= 5 ? tag("SHEX") : tag("SHDR"), {}};
    program.data.resize(program_size);
    std::memcpy(program.data.data(), code, program_size);
    parts.push_back(std::move(program));
    assemble(parts, checksum, out);
    dump(*out);
    return S_OK;
}

} // namespace vgd::umd

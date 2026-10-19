#include "ctab.h"

namespace {

#pragma pack(push, 1)
struct CtabHeader {
    DWORD size, creator, version, constants, constantInfo, flags, target;
};
struct CtabConstantInfo {
    DWORD name;
    WORD registerSet, registerIndex, registerCount, reserved;
    DWORD typeInfo, defaultValue;
};
struct CtabTypeInfo {
    WORD typeClass, type, rows, columns, elements, structMembers;
    DWORD structMemberInfo;
};
#pragma pack(pop)

const DWORD kCommentToken = 0xFFFE;
const DWORD kEndToken = 0x0000FFFF;
const DWORD kCtabFourCC = 'BATC'; // "CTAB" little-endian

} // namespace

size_t ShaderCodeLength(const DWORD* code)
{
    // Instruction lengths are only encoded in bits 24-27 from SM2 on; SM1 needs a
    // per-opcode table. Scanning for the END token is robust enough here because
    // comment payloads are skipped explicitly and END never appears as an operand.
    size_t i = 1;
    const DWORD major = (code[0] >> 8) & 0xFF;
    for (;;) {
        DWORD tok = code[i];
        if (tok == kEndToken)
            return i + 1;
        if ((tok & 0xFFFF) == kCommentToken) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        if (major >= 2) {
            i += 1 + ((tok >> 24) & 0x0F);
        } else if ((tok & 0xFFFF) == 0x51) {
            i += 6; // def: destination + four raw float literals
        } else {
            // SM1: walk operand tokens, which all have bit 31 set.
            ++i;
            while (code[i] & 0x80000000)
                ++i;
        }
    }
}

bool ParseConstantTable(const DWORD* code, size_t dwordCount, std::vector<ShaderConstant>& out)
{
    out.clear();
    for (size_t i = 1; i < dwordCount;) {
        DWORD tok = code[i];
        if (tok == kEndToken)
            break;
        if ((tok & 0xFFFF) != kCommentToken) {
            // Constant tables precede the first instruction.
            break;
        }
        size_t len = (tok >> 16) & 0x7FFF;
        if (len >= 1 && code[i + 1] == kCtabFourCC && len * 4 >= sizeof(CtabHeader) + 4) {
            const BYTE* base = (const BYTE*)&code[i + 2];
            size_t bytes = (len - 1) * 4;
            const CtabHeader* hdr = (const CtabHeader*)base;
            if (hdr->constantInfo + hdr->constants * sizeof(CtabConstantInfo) > bytes)
                return false;
            const CtabConstantInfo* infos = (const CtabConstantInfo*)(base + hdr->constantInfo);
            for (DWORD c = 0; c < hdr->constants; ++c) {
                ShaderConstant sc;
                if (infos[c].name < bytes)
                    sc.name = (const char*)(base + infos[c].name);
                sc.registerSet = infos[c].registerSet;
                sc.registerIndex = infos[c].registerIndex;
                sc.registerCount = infos[c].registerCount;
                if (infos[c].typeInfo + sizeof(CtabTypeInfo) <= bytes) {
                    const CtabTypeInfo* ti = (const CtabTypeInfo*)(base + infos[c].typeInfo);
                    sc.typeClass = ti->typeClass;
                    sc.rows = ti->rows;
                    sc.columns = ti->columns;
                    sc.elements = ti->elements;
                }
                out.push_back(sc);
            }
            return true;
        }
        i += 1 + len;
    }
    return false;
}

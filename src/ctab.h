#pragma once
#include <windows.h>
#include <string>
#include <vector>

// One entry of a D3DX constant table (the CTAB comment block embedded in SM1-3 bytecode).
struct ShaderConstant {
    std::string name;
    int registerSet = 0; // 0 bool, 1 int4, 2 float4, 3 sampler
    int registerIndex = 0;
    int registerCount = 0;
    int typeClass = 0; // 0 scalar, 1 vector, 2 matrix rows, 3 matrix columns, 4 object, 5 struct
    int rows = 0, columns = 0, elements = 0;
};

// Parses the constant table; returns false if the bytecode carries none.
bool ParseConstantTable(const DWORD* code, size_t dwordCount, std::vector<ShaderConstant>& out);

// Size in DWORDs of SM1-3 bytecode (up to and including the END token).
size_t ShaderCodeLength(const DWORD* code);

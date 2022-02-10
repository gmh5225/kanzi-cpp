/*
Copyright 2011-2021 Frederic Langlet
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
you may obtain a copy of the License at

                http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "X86Codec.hpp"
#include "../Global.hpp"
#include "../Magic.hpp"

using namespace kanzi;
using namespace std;

bool X86Codec::forward(SliceArray<byte>& input, SliceArray<byte>& output, int count) THROW
{
    if (count == 0)
        return true;

    if ((count < MIN_BLOCK_SIZE) || (count > MAX_BLOCK_SIZE))
        return false;

    if (!SliceArray<byte>::isValid(input))
        throw std::invalid_argument("Invalid input block");

    if (!SliceArray<byte>::isValid(output))
        throw std::invalid_argument("Invalid output block");

    if (output._length - output._index < getMaxEncodedLength(count))
        return false;

    if (_pCtx != nullptr) {
        Global::DataType dt = (Global::DataType) _pCtx->getInt("dataType", Global::UNDEFINED);

        if ((dt != Global::UNDEFINED) && (dt != Global::EXE))
            return false;
    }

    int codeStart = 0;
    int codeEnd = count - 8;
    byte mode = detectType(&input._array[input._index], count - 8, codeStart, codeEnd);

    if (mode == NOT_EXE)
       return false;

    if (_pCtx != nullptr)
       _pCtx->putInt("dataType", Global::EXE);

    if (mode == X86)
        return forwardX86(input, output, count, codeStart, codeEnd);

    if (mode == ARM64)
       return forwardARM(input, output, count, codeStart, codeEnd);
   
    return false;
}

bool X86Codec::forwardX86(SliceArray<byte>& input, SliceArray<byte>& output, int count, int codeStart, int codeEnd) THROW
{
    byte* src = &input._array[input._index];
    byte* dst = &output._array[output._index];
    dst[0] = X86;
    int srcIdx = codeStart;
    int dstIdx = 9;
    int matches = 0;

    if (codeStart > 0) {
        memcpy(&dst[dstIdx], &src[0], codeStart);
        dstIdx += codeStart;        
    }

    while (srcIdx < codeEnd) {
        if (src[srcIdx] == X86_TWO_BYTE_PREFIX) {            
            dst[dstIdx++] = src[srcIdx++];            

            if ((src[srcIdx] & X86_MASK_JCC) != X86_INSTRUCTION_JCC) {
                // Not a relative jump
                if (src[srcIdx] == X86_ESCAPE)  
                    dst[dstIdx++] = X86_ESCAPE; 
            
                dst[dstIdx++] = src[srcIdx++];    
                continue;
            }
        }
        else if ((src[srcIdx] & X86_MASK_JUMP) != X86_INSTRUCTION_JUMP) {
            // Not a relative call
            if (src[srcIdx] == X86_ESCAPE)  
                dst[dstIdx++] = X86_ESCAPE; 
            
            dst[dstIdx++] = src[srcIdx++];              
            continue;
        }

        // Current instruction is a jump/call.
        const int sgn = int(src[srcIdx + 4]);
        const int offset = LittleEndian::readInt32(&src[srcIdx + 1]);
        
	    if (((sgn != 0) && (sgn != 0xFF)) || (offset == int(0xFF000000))) {
            dst[dstIdx++] = X86_ESCAPE;
            dst[dstIdx++] = src[srcIdx++];
            continue;
        } 

        // Absolute target address = srcIdx + 5 + offset. Let us ignore the +5 
        const int addr = srcIdx + ((sgn == 0) ? offset : -(-offset & X86_ADDR_MASK));
        dst[dstIdx++] = src[srcIdx++];
        BigEndian::writeInt32(&dst[dstIdx], addr ^ MASK_ADDRESS);
        srcIdx += 4;
        dstIdx += 4;
        matches++;       
    }

    // Worth it ?
    if (matches < 16)
        return false;

    LittleEndian::writeInt32(&dst[1], codeStart);
    LittleEndian::writeInt32(&dst[5], dstIdx);
    memcpy(&dst[dstIdx], &src[srcIdx], count - srcIdx);
    dstIdx += (count - srcIdx);
    input._index += count;
    output._index += dstIdx; 
    return true;
}

bool X86Codec::forwardARM(SliceArray<byte>& input, SliceArray<byte>& output, int count, int codeStart, int codeEnd) THROW
{
    byte* src = &input._array[input._index];
    byte* dst = &output._array[output._index]; 
    dst[0] = ARM64;
    int srcIdx = codeStart;
    int dstIdx = 9;
    int matches = 0;
    int fpRun = 0;

    if (codeStart > 0) {
        memcpy(&dst[dstIdx], &src[0], codeStart);
        dstIdx += codeStart;        
    }
    
    while (srcIdx < codeEnd) {
        const int instr = LittleEndian::readInt32(&src[srcIdx]);
        const int opcode1 = instr & ARM_B_OPCODE_MASK;
        //const int opcode2 = instr & ARM_CB_OPCODE_MASK;
        bool isBL = (opcode1 == ARM_OPCODE_B) || (opcode1 == ARM_OPCODE_BL); // inconditional jump
        bool isCB = false; // disable for now ... isCB = (opcode2 == ARM_OPCODE_CBZ) || (opcode2 == ARM_OPCODE_CBNZ); // conditional jump

        if ((isBL == false) && (isCB == false)) {
            // Not a relative jump
            memcpy(&dst[dstIdx], &src[srcIdx], 4);
            srcIdx += 4;
            dstIdx += 4;
            continue;
        }
        
        int addr, val;
        
        if (isBL == true) {
            // opcode(6) + sgn(1) + offset(25)
            // Absolute target address = srcIdx +/- (offet*4)
            const int offset = instr & ARM_B_ADDR_MASK;
            const int sgn = instr & ARM_B_ADDR_SGN_MASK;
            addr = srcIdx + 4 * ((sgn == 0) ? offset : ARM_B_OPCODE_MASK | offset);
            
            if (addr < 0)
                addr = 0;
            
            val = opcode1 | (addr >> 2);
        }
        else { // isCB == true
            // opcode(8) + sgn(1) + offset(18) + register(5)
            // Absolute target address = srcIdx +/- (offet*4)
            const int offset = (instr & ARM_CB_ADDR_MASK) >> ARM_CB_REG_BITS;
            const int sgn = instr & ARM_CB_ADDR_SGN_MASK;
            addr = srcIdx + 4 * ((sgn == 0) ? offset : 0xFFFC0000 | offset);
            
            if (addr < 0)
                addr = 0;
            
            val = (instr & ~ARM_CB_ADDR_MASK) | ((addr >> 2) << ARM_CB_REG_BITS);        
        }
        
        if (addr == 0) {
            fpRun++; 
            
            // Early exit if likely outside a code section
            if (fpRun == 4)
                break;
            
            LittleEndian::writeInt32(&dst[dstIdx], val); // 0 address as escape
            memcpy(&dst[dstIdx + 4], &src[srcIdx], 4);
            srcIdx += 4;
            dstIdx += 8;
            continue;
        }
        
        fpRun = 0;          
        LittleEndian::writeInt32(&dst[dstIdx], val);
        srcIdx += 4;
        dstIdx += 4;
        matches++;
    }
    
    // Worth it ?
    if (matches < 16)
        return false;

    LittleEndian::writeInt32(&dst[1], codeStart);
    LittleEndian::writeInt32(&dst[5], dstIdx);
    memcpy(&dst[dstIdx], &src[srcIdx], count - srcIdx);
    dstIdx += (count - srcIdx);
    input._index += count;
    output._index += dstIdx;
    return true;
}

bool X86Codec::inverse(SliceArray<byte>& input, SliceArray<byte>& output, int count) THROW
{
    if (count == 0)
        return true;

    if (!SliceArray<byte>::isValid(input))
        throw std::invalid_argument("Invalid input block");

    if (!SliceArray<byte>::isValid(output))
        throw std::invalid_argument("Invalid output block");

    byte mode = input._array[input._index];

    if (mode == X86)
        return inverseX86(input, output, count);

    if (mode == ARM64)
        return inverseARM(input, output, count);

    return false;
}

bool X86Codec::inverseX86(SliceArray<byte>& input, SliceArray<byte>& output, int count) THROW
{
    byte* src = &input._array[input._index];
    byte* dst = &output._array[output._index];
    int srcIdx = 9;
    int dstIdx = 0;
    const int codeStart = LittleEndian::readInt32(&src[1]);
    const int codeEnd = LittleEndian::readInt32(&src[5]);

    if (codeStart > 0) {
        memcpy(&dst[dstIdx], &src[9], codeStart);
        dstIdx += codeStart;
        srcIdx += codeStart;
    }    
    
    while (srcIdx < codeEnd) {
        if (src[srcIdx] == X86_TWO_BYTE_PREFIX) {
            dst[dstIdx++] = src[srcIdx++];   

            if ((src[srcIdx] & X86_MASK_JCC) != X86_INSTRUCTION_JCC) {
                // Not a relative jump
                if (src[srcIdx] == X86_ESCAPE)
                    srcIdx++;
			
                dst[dstIdx++] = src[srcIdx++];
                continue;
            }
        }
        else if ((src[srcIdx] & X86_MASK_JUMP) != X86_INSTRUCTION_JUMP) {
            // Not a relative call
            if (src[srcIdx] == X86_ESCAPE)
                srcIdx++;
            
            dst[dstIdx++] = src[srcIdx++];
            continue;
        }

        // Current instruction is a jump/call. Decode absolute address
        const int addr = BigEndian::readInt32(&src[srcIdx + 1]) ^ MASK_ADDRESS;  
        const int offset = addr - dstIdx;    		
        dst[dstIdx++] = src[srcIdx++];
        LittleEndian::writeInt32(&dst[dstIdx], (offset >= 0) ? offset : -(-offset & X86_ADDR_MASK));       
        srcIdx += 4;
        dstIdx += 4;
    }
	
    memcpy(&dst[dstIdx], &src[srcIdx], count - srcIdx);
    dstIdx += (count - srcIdx);
    input._index += count;
    output._index += dstIdx;
    return true;
}

bool X86Codec::inverseARM(SliceArray<byte>& input, SliceArray<byte>& output, int count) THROW
{
    byte* src = &input._array[input._index];
    byte* dst = &output._array[output._index];
    int srcIdx = 9;
    int dstIdx = 0;
    const int codeStart = LittleEndian::readInt32(&src[1]);
    const int codeEnd = LittleEndian::readInt32(&src[5]);
    int fpRun = 0;

    if (codeStart > 0) {
        memcpy(&dst[dstIdx], &src[9], codeStart);
        dstIdx += codeStart;
        srcIdx += codeStart;
    }  
    
    while (srcIdx < codeEnd) {
        const int instr = LittleEndian::readInt32(&src[srcIdx]);
        const int opcode1 = instr & ARM_B_OPCODE_MASK;
        //const int opcode2 = instr & ARM_CB_OPCODE_MASK;
        bool isBL = (opcode1 == ARM_OPCODE_B) || (opcode1 == ARM_OPCODE_BL); // inconditional jump
        bool isCB = false; // disable for now ... isCB = (opcode2 == ARM_OPCODE_CBZ) || (opcode2 == ARM_OPCODE_CBNZ); // conditional jump

        if ((isBL == false) && (isCB == false)) {                
            // Not a relative jump 
            memcpy(&dst[dstIdx], &src[srcIdx], 4);
            srcIdx += 4;
            dstIdx += 4;
            continue;
        }

        // Decode absolute address
        int val, addr;

        if (isBL == true) {
            addr = (instr & ARM_B_ADDR_MASK) << 2;
            const int offset = (addr - dstIdx) >> 2;
            val = opcode1 | (offset & ARM_B_ADDR_MASK);
        }
        else {
            addr = ((instr & ARM_CB_ADDR_MASK) >> ARM_CB_REG_BITS) << 2;
            const int offset = (addr - dstIdx) >> 2;
            val = (instr & ~ARM_CB_ADDR_MASK) | (offset << ARM_CB_REG_BITS);              
        }
        
        if (addr == 0) {
            // False positive
            fpRun++;
        
            // Early exit if likely outside a code section
            if (fpRun == 4)
                break;
            
            memcpy(&dst[dstIdx], &src[srcIdx + 4], 4);
            srcIdx += 8;
            dstIdx += 4;
            continue;
        } 
            
        fpRun = 0;       
        LittleEndian::writeInt32(&dst[dstIdx], val);
        srcIdx += 4;
        dstIdx += 4;
    }
 
    memcpy(&dst[dstIdx], &src[srcIdx], count - srcIdx);
    dstIdx += (count - srcIdx);
    input._index += count;
    output._index += dstIdx;
    return true;
}


byte X86Codec::detectType(byte src[], int count, int& codeStart, int& codeEnd)     
{
    // Let us check the first bytes ... but this may not be the first block
    // Best effort
    const uint magic = Magic::getType(src);
    int arch = 0;
    
    if (parseHeader(src, count, magic, arch, codeStart, codeEnd) == true) {
        if ((arch == ELF_X86_ARCH) || (arch == ELF_AMD64_ARCH))
            return X86;
       
        if ((arch == WIN_X86_ARCH) || (arch == WIN_AMD64_ARCH))
            return X86;
        
        if (arch == MAC_AMD64_ARCH)
            return X86;
       
        if ((arch == ELF_ARM64_ARCH) || (arch == WIN_ARM64_ARCH))
            return ARM64;
        
        if (arch == MAC_ARM64_ARCH)
            return ARM64;
    } 
    
    int jumpsX86 = 0;
    int jumpsARM64 = 0;
    int zeros = 0;
    int smallVals = 0;
    count = codeEnd - codeStart;

    for (int i = codeStart; i < codeEnd; i++) {       
        if (src[i] < 16) {
            smallVals++;
            
            if (src[i] == 0)
                zeros++;
        }

        // X86
        if ((src[i] & X86_MASK_JUMP) == X86_INSTRUCTION_JUMP) {
           if ((src[i + 4] == byte(0)) || (src[i + 4] == byte(0xFF))) {
              // Count relative jumps (CALL = E8/ JUMP = E9 .. .. .. 00/FF)
              jumpsX86++;
              continue;
           }
        } else if (src[i] == X86_TWO_BYTE_PREFIX) {
            i++;
            
            if ((src[i] == byte(0x38)) || (src[i] == byte(0x3A)))
               i++;  
               
            // Count relative conditional jumps (0x0F 0x8?) with 16/32 offsets
            if ((src[i] & X86_MASK_JCC) == X86_INSTRUCTION_JCC) {
               jumpsX86++;
               continue;
            }               
        }

        // ARM
        if ((i & 3) != 0)
            continue;
        
        const int instr = LittleEndian::readInt32(&src[i]);
        const int opcode1 = instr & ARM_B_OPCODE_MASK;
        const int opcode2 = instr & ARM_CB_OPCODE_MASK;

        if ((opcode1 == ARM_OPCODE_B) || (opcode1 == ARM_OPCODE_BL))
           jumpsARM64++;
        else if ((opcode2 == ARM_OPCODE_CBZ) || (opcode2 == ARM_OPCODE_CBNZ))
           jumpsARM64++;       
    }

    if (zeros < (count / 10))
        return NOT_EXE;
    
    // Filter out (some/many) multimedia files
    if (smallVals > (count / 2))
        return NOT_EXE;
    
    // Ad-hoc thresholds
    if (jumpsX86 >= (count / 200)) 
        return X86;
    
    if (jumpsARM64 >= (count / 200))
        return ARM64;

    // Number of jump instructions too small => either not a binary
    // or not worth the change, skip.
    return NOT_EXE;
}

// Return true if known header
bool X86Codec::parseHeader(byte src[], int count, uint magic, int& arch, int& codeStart, int& codeEnd) {
    if (magic == Magic::WIN_MAGIC) {
        if (count >= 64) {
            const int posPE = LittleEndian::readInt32(&src[60]);
            
            if ((posPE > 0) && (posPE <= count - 48) && (LittleEndian::readInt32(&src[posPE]) == WIN_PE)) {
                byte* pe = &src[posPE];
                codeStart = min(LittleEndian::readInt32(&pe[44]), count);
                codeEnd = min(codeStart + LittleEndian::readInt32(&pe[28]), count);
                arch = LittleEndian::readInt16(&pe[4]);
            }
            
            return true;
        }
    } 
    else if (magic == Magic::ELF_MAGIC) {
        bool isLittleEndian = src[5] == byte(1);
        
        if (count >= 64) { 
            if (isLittleEndian == true) {        
                // Super inaccurate, just jump over file and program headers
                if (src[4] == 2) {
                    // 64 bits
                    codeStart = 0x40 + int(LittleEndian::readInt16(&src[0x36])) * int(LittleEndian::readInt16(&src[0x38]));
                }
                else {
                    // 32 bits
                    codeStart = 0x34 + int(LittleEndian::readInt16(&src[0x2A])) * int(LittleEndian::readInt16(&src[0x2C]));
                }
                      
                arch = LittleEndian::readInt16(&src[18]);
            }
            else {
                // Super inaccurate, just jump over file and program headers
                if (src[4] == 2) {
                    // 64 bits
                    codeStart = 0x40 + int(BigEndian::readInt16(&src[0x36])) * int(LittleEndian::readInt16(&src[0x38]));
                }
                else {
                    // 32 bits
                    codeStart = 0x34 + int(BigEndian::readInt16(&src[0x2A])) * int(LittleEndian::readInt16(&src[0x2C]));
                }
                      
                arch = BigEndian::readInt16(&src[18]);       
            }
            
            codeStart = min(codeStart, count);
            return true;
        }
    }
    else if ((magic == Magic::MAC_MAGIC32) || (magic == Magic::MAC_CIGAM32) || 
             (magic == Magic::MAC_MAGIC64) || (magic == Magic::MAC_CIGAM64)) { 
        
        if (count >= 8)
            arch = LittleEndian::readInt32(&src[4]);
        
        codeStart = 0x20; // just ignore Mach-O header for now
        return true;
    }
    
    return false;
}
"""Compile the unchanged CM4 PDO mapper with SDK-generated PID packets on the host.

Usage: python tests/run_motor_pid_mapping_test.py D:/GitSpace/FSA/neofsa
Only build/motor-pid-mapping-check is written. No firmware build or hardware access.
"""
from pathlib import Path
import hashlib
import re
import subprocess
import sys

project = Path(__file__).resolve().parents[1]
firmware = Path(sys.argv[1])
sdk = Path(sys.argv[2]) if len(sys.argv) > 2 else project.parent / "ava_eth"
out = project / "build/motor-pid-mapping-check"
out.mkdir(parents=True, exist_ok=True)
mapper = firmware / "CM4/EthComm/User/ec_app.cpp"
before = hashlib.sha256(mapper.read_bytes()).digest()
source = re.sub(r'^#include.*$', '', mapper.read_text(encoding='utf-8'), flags=re.M)
ssc = firmware / "CM4/EthComm/EtherCAT/SSC/Src"
headers = '\n'.join((ssc / n).read_text(encoding='utf-8') for n in ('sdoserv.h', 'objdef.h'))
constants = []
for name in ('OBJACCESS_RXPDOMAPPING', 'OBJACCESS_TXPDOMAPPING', 'OBJFLAGS_OBJCODEMASK',
             'OBJFLAGS_OBJCODESHIFT', 'OBJCODE_VAR'):
    match = re.search(r'^\s*#define\s+' + name + r'\s+(.+?)(?:/\*|//|$)', headers, re.M)
    assert match, name
    constants.append(f'#define {name} {match[1]}')
objects = (ssc / 'FSANeoObjects.h').read_text(encoding='utf-8')
for index in ('2400', '2402', '2404', '2408', '240C', '2410', '2414', '2418'):
    desc = re.search(r'sEntryDesc0x' + index + r'\s*=\s*\{([^}]+)', objects, re.I)
    assert desc and 'OBJACCESS_RXPDOMAPPING' in desc[1] and 'OBJACCESS_TXPDOMAPPING' not in desc[1], index

harness = r'''
#include "ava/profile.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <iostream>
using UINT8=uint8_t; using UINT16=uint16_t; using UINT32=uint32_t;
CONSTANTS
struct Mapping { UINT16 count; UINT32 entries[12]; } mapping{};
struct TOBJECT { struct { UINT16 ObjFlags; } ObjDesc; void *pVarPtr; } mapObject{{0},&mapping}, scalar{{OBJCODE_VAR<<OBJFLAGS_OBJCODESHIFT},nullptr};
struct Desc { UINT16 BitLength, ObjAccess; } desc;
TOBJECT *OBJ_GetObjectHandle(UINT16 index) {
    if(index==0x1600 || index==0x1A00) return &mapObject;
    if(index==0x6040) { desc={16,OBJACCESS_RXPDOMAPPING}; return &scalar; }
    if(index==0x2080) { desc={8,OBJACCESS_RXPDOMAPPING}; return &scalar; }
    for(const auto &e:ava::neo_outputs()) if(index==e.index) { desc={e.bits,OBJACCESS_RXPDOMAPPING}; return &scalar; }
    for(const auto &g:ava::neo_gains()) if(index==g.index) { desc={g.bits,OBJACCESS_RXPDOMAPPING}; return &scalar; }
    return nullptr;
}
Desc *OBJ_GetEntryDesc(TOBJECT *, int) { return &desc; }
unsigned OBJ_GetEntryOffset(UINT16 sub, const TOBJECT *) { return (offsetof(Mapping,entries)+(sub-1)*4)*8; }
struct Assignment { UINT16 u16SubIndex0, aEntries[1]; } sRxPDOassign{1,{0x1600}},sTxPDOassign{1,{0x1A00}};
UINT16 nPdInputSize=28,nPdOutputSize=28;
struct Shared {
    alignas(4) UINT8 rt_ctrl_data[1024];
    alignas(4) UINT8 user_param_imm[1024];
    struct { struct { UINT32 v[0x600]; } index; } cia402_od;
} memory{};
Shared *shm=&memory;
SOURCE
int main() {
    std::array<float,8> values{};
    for(unsigned i=0;i<8;++i) values[i]=float(i+1)/3;
    auto apply=[&](uint32_t mask) {
        for(auto batch:ava::neo_pid_batches(mask,values)) {
            assert(batch.entries.size()<=12);
            mapping.count=UINT16(batch.entries.size());
            for(size_t i=0;i<batch.entries.size();++i) {
                auto e=batch.entries[i]; mapping.entries[i]=(UINT32(e.index)<<16)|(UINT32(e.sub)<<8)|e.bits;
            }
            $Sub$$APPL_OutputMapping(reinterpret_cast<UINT16*>(batch.output.data()));
        }
    };
    apply(255);
    for(unsigned i=0;i<8;++i) {
        const auto &g=ava::neo_gains()[i]; const auto encoded=ava::encode_gain(g,values[i]);
        assert(!std::memcmp(shm->user_param_imm+g.index-0x2400,encoded.data(),encoded.size()));
    }
    UINT8 previous[28]; std::memcpy(previous,shm->user_param_imm,28);
    shm->cia402_od.index.v[0x40]=15; shm->rt_ctrl_data[0x80]=6;
    values[7]=9.5f; apply(128);
    assert(!std::memcmp(previous,shm->user_param_imm,24));
    float last; std::memcpy(&last,shm->user_param_imm+24,4); assert(last==9.5f);
    assert(shm->cia402_od.index.v[0x40]==0 && shm->rt_ctrl_data[0x80]==0);
    UINT16 inputs[14]; std::memset(inputs,0xFF,sizeof(inputs));
    $Sub$$APPL_InputMapping(inputs);
    for(auto v:inputs) assert(!v); // Existing mapper cannot return these Rx-only gains.
    auto padding=ava::neo_live_pid_outputs();
    mapping.count=12;
    for(unsigned i=0;i<12;++i) mapping.entries[i]=(UINT32(padding[i].index)<<16)|padding[i].bits;
    auto packet=ava::neo_command(15,123.25f,4.5f,8);
    packet[27]=0;
    auto motionCheck=[&] {
        $Sub$$APPL_OutputMapping(reinterpret_cast<UINT16*>(packet.data()));
        assert(shm->cia402_od.index.v[0x40]==15);
        assert((shm->cia402_od.index.v[0x60]&255)==8);
        assert(!std::memcmp(shm->rt_ctrl_data+0x10,packet.data()+11,4));
        assert(!std::memcmp(shm->rt_ctrl_data+0x14,packet.data()+15,2));
        assert(!std::memcmp(shm->rt_ctrl_data+0x18,packet.data()+19,2));
        assert(!std::memcmp(shm->rt_ctrl_data+0x1A,packet.data()+21,2));
        assert(shm->rt_ctrl_data[0x80]==0);
    };
    // SSC OBJ_Write stores each mapping as low WORD then high WORD. Exercise
    // the actual mapper between every store, including SI0, as an ISR could.
    auto remap=[&](int gain) {
        const auto entries=ava::neo_live_pid_outputs(gain);
        mapping.count=12; motionCheck();
        for(unsigned i=0;i<12;++i) {
            UINT32 value=(UINT32(entries[i].index)<<16)|entries[i].bits;
            assert((mapping.entries[i]&65535)==(value&65535));
            mapping.entries[i]=(mapping.entries[i]&0xFFFF0000u)|(value&65535); motionCheck();
            mapping.entries[i]=value; motionCheck();
        }
    };
    for(unsigned i=0;i<8;++i) {
        std::memcpy(previous,shm->user_param_imm,28);
        const auto &g=ava::neo_gains()[i]; const auto encoded=ava::encode_gain(g,float(i+10)/3);
        std::copy(encoded.begin(),encoded.end(),packet.begin()+ava::neo_live_pid_offset(i));
        motionCheck(); assert(!std::memcmp(previous,shm->user_param_imm,28)); // Still padding.
        remap(int(i));
        std::memcpy(previous+g.index-0x2400,encoded.data(),encoded.size());
        assert(!std::memcmp(previous,shm->user_param_imm,28));
        remap(-1);
        std::fill(packet.begin()+2,packet.begin()+6,0); packet[17]=packet[18]=0;
        motionCheck(); assert(!std::memcmp(previous,shm->user_param_imm,28));
    }
    std::cout<<"Live PID: all 8 gains updated; enable, mode and motion targets preserved at every mapping WORD store\n";
    std::cout<<"Unchanged CM4 mapper: SDK PID packets update all 8 gains, preserve unselected values, clear enable and special_cmd; no gain readback\n";
}
'''.replace('CONSTANTS', '\n'.join(constants)).replace('SOURCE', source)
test = out / 'test.cpp'
test.write_text(harness, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['g++', '-std=c++17', '-O2', '-fstrict-aliasing', '-static',
                '-I' + str(sdk / 'sdk/c/src/include'), str(test),
                str(sdk / 'sdk/c/src/profile.cpp'), str(sdk / 'sdk/c/src/wire.cpp'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
assert hashlib.sha256(mapper.read_bytes()).digest() == before, 'Firmware source changed'

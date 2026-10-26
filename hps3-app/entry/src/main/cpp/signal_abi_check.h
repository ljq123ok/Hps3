#pragma once
#include "Emu/CPU/Backends/AArch64/AArch64Signal.h"
#include <cstring>

// Run inside the signed HAP because this device refuses standalone native
// executables. Exercise the actual SDK's mcontext layout and core decoder.
inline bool hps3_check_signal_abi()
{
    ucontext_t context{};
    auto* records = reinterpret_cast<unsigned char*>(context.uc_mcontext.__reserved);
    const auto set_header = [&](unsigned offset, u32 magic, u32 size)
    {
        const aarch64::aarch64_cpu_ctx_block head{magic, size};
        std::memcpy(records + offset, &head, sizeof(head));
    };
    const auto set_esr = [&](u64 value) { std::memcpy(records + 536, &value, sizeof(value)); };
    set_header(0, 0x46508001, 528);
    set_header(528, 0x45535201, 16);
    set_esr((0x24ull << 26) | 64);
    unsigned passed = 0;
    const auto check = [&](aarch64::fault_reason expected)
    { passed += aarch64::decode_fault_reason(&context) == expected; };
    check(aarch64::fault_reason::data_write);
    set_esr(0x24ull << 26);
    check(aarch64::fault_reason::data_read);
    set_header(0, 0x46508001, 0);
    check(aarch64::fault_reason::undefined);
    set_header(0, 0x46508001, 8192);
    check(aarch64::fault_reason::undefined);
    set_header(0, 0x46508001, 528);
    set_header(528, 0x45535201, 8);
    check(aarch64::fault_reason::undefined);
    std::memset(&context, 0, sizeof(context));
    check(aarch64::fault_reason::undefined);
    HPS3_LOG("HPS3_SIGNAL_ABI pass=%{public}u total=6 reserved_element=%{public}zu", passed,
        sizeof(context.uc_mcontext.__reserved[0]));
    return passed == 6;
}

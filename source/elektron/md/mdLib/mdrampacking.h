#pragma once

#include <array>

#include "mdtypes.h"
#include "dsp56kEmu/dsp.h"

namespace md
{
	// OS 1.63 prefixes a carried sample when the previous RAM recording count is
	// odd, but its original pair count uses only the new samples. An even batch can
	// therefore leave the final advertised sample unwritten. CompleteTail includes
	// the carried sample before rounding up; Original preserves the loaded program.
	inline constexpr dsp56k::TWord g_ramPackingAddress = 0x103602;
	inline constexpr std::array<dsp56k::TWord, 13> g_ramPackingOriginal = {
		0x200001,
		0x0140c0, 0x000001,
		0x200022,
		0x022ef4,
		0x0cc480, 0x000004,
		0x0266b5,
		0x4d5d00,
		0x547000, 0x0000ff,
		0x5d7000, 0x0000ff
	};
	inline constexpr std::array<dsp56k::TWord, 13> g_ramPackingCompleteTail = {
		0x200001,
		0x022ef4,
		0x0cc480, 0x000005,
		0x014180,
		0x0266b5,
		0x4d5d00,
		0x014180,
		0x200022,
		0x547000, 0x0000ff,
		0x5d7000, 0x0000ff
	};

	enum class RamPackingUpdate { Applied, AlreadyApplied, UnexpectedCode, Busy };

	// Call on the emulation owner between DSP dispatches, after the second-stage
	// loader has finished. The exact sequence checks make both directions atomic:
	// an unknown or partly changed program remains untouched.
	inline RamPackingUpdate setRamPackingMode(dsp56k::DSP& _dsp,
		const RamRecordingMode _mode)
	{
		const auto pc = _dsp.getPC().var;
		if(pc >= g_ramPackingAddress && pc < g_ramPackingAddress + g_ramPackingOriginal.size())
			return RamPackingUpdate::Busy;

		auto& memory = _dsp.memory();
		bool original = true;
		bool completeTail = true;
		for(size_t i = 0; i < g_ramPackingOriginal.size(); ++i)
		{
			const auto word = memory.get(dsp56k::MemArea_P, g_ramPackingAddress + i);
			original &= word == g_ramPackingOriginal[i];
			completeTail &= word == g_ramPackingCompleteTail[i];
		}

		const bool targetPresent = _mode == RamRecordingMode::CompleteTail
			? completeTail : original;
		if(targetPresent)
			return RamPackingUpdate::AlreadyApplied;
		if(!original && !completeTail)
			return RamPackingUpdate::UnexpectedCode;

		const auto& target = _mode == RamRecordingMode::CompleteTail
			? g_ramPackingCompleteTail : g_ramPackingOriginal;
		for(size_t i = 0; i < target.size(); ++i)
			memory.set(dsp56k::MemArea_P, g_ramPackingAddress + i, target[i]);
		for(size_t i = 0; i < target.size(); ++i)
			_dsp.clearOpcodeCache(g_ramPackingAddress + i);
		return RamPackingUpdate::Applied;
	}
}

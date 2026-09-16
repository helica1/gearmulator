#include "mdsampledirectory.h"

#include "mdhardware.h"

#include <vector>

namespace md::sampleDirectory
{
	namespace
	{
		constexpr uint32_t g_nameTable = 0x7244a;
		constexpr uint32_t g_nameEntrySize = 5;

		uint32_t word32(const uint8_t* _p)
		{
			const uint32_t low = (static_cast<uint32_t>(_p[0]) << 8) | _p[1];
			const uint32_t high = (static_cast<uint32_t>(_p[2]) << 8) | _p[3];
			return (high << 16) | low;
		}
	}

	uint32_t sectorsForFrames(const uint32_t _frames)
	{
		if(_frames <= g_headSectorFrames)
			return 1;
		return 1 + (_frames - g_headSectorFrames + g_continuationSectorFrames - 1) / g_continuationSectorFrames;
	}

	Directory read(Hardware& _hardware)
	{
		Directory dir;
		if(_hardware.getModel() != MachineModel::Machinedrum)
			return dir;

		auto& uc = _hardware.getUC();
		std::array<bool, g_slotCount> headerSeen{};
		for(uint32_t sector = g_firstSector; sector < g_endSector; ++sector)
		{
			uint8_t header[24];
			if(!uc.copyFlashDataRangeRealtime(header, sector * g_sectorSize, sizeof(header)))
				return dir;
			if(header[0] == 0x7a)
			{
				++dir.freeSectors;
				continue;
			}
			const auto slot = header[1];
			if(slot >= g_slotCount)
				continue;
			if(header[0] == 0x18 && header[2] == 0x00 && header[3] == 0x10 && !headerSeen[slot])
			{
				headerSeen[slot] = true;
				auto& s = dir.slots[slot];
				s.used = true;
				const auto periodNs = word32(header + 4);
				s.sampleRate = periodNs ? static_cast<uint32_t>((1000000000ull + periodNs / 2) / periodNs) : 0;
				s.frames = word32(header + 8);
				++s.sectors;
			}
			else if(header[0] == 0x1a)
				++dir.slots[slot].sectors;
		}

		std::vector<uint8_t> names(g_slotCount * g_nameEntrySize);
		if(!uc.copyPatchRamRange(names.data(), g_nameTable, names.size()))
			return dir;
		for(uint32_t slot = 0; slot < g_slotCount; ++slot)
		{
			auto& s = dir.slots[slot];
			if(!s.used)
				continue;
			for(uint32_t i = 0; i < 4; ++i)
			{
				const auto c = names[slot * g_nameEntrySize + i];
				s.name.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : ' ');
			}
		}
		dir.valid = true;
		return dir;
	}
}

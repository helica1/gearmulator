#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace md
{
	class Hardware;
}

namespace md::sampleDirectory
{
	// Machinedrum UW sample storage, identical on OS 1.63 and X.13 (found by uploading
	// samples with known names into a headless machine and diffing memory):
	//
	// flash 0x200000..0x79ffff, 64 KiB sectors
	//   sector byte 0 = 0x7A  erased / free
	//   sector byte 0 = 0x18  first sector of a sample, 24 byte header:
	//     [1] slot 0..47, [2..3] 0x0010,
	//     [4..7]  sample period in ns   (two big-endian 16 bit words, low word first)
	//     [8..11] length in frames      (same encoding), [12..15] loop start, [16..19] loop end
	//     [20..21] loop type (0x007F = off), then 16 bit big-endian sample data
	//   sector byte 0 = 0x1A  continuation sector, [1] slot, 4 byte header
	// battery RAM (patch RAM) 0x7244A: 48 x 5 bytes, 4 character name + 1 byte per slot.
	// Names of empty slots are stale, so a slot is in use only if flash has its header.

	constexpr uint32_t g_slotCount = 48;
	constexpr uint32_t g_sectorSize = 0x10000;
	constexpr uint32_t g_firstSector = 0x20;
	constexpr uint32_t g_endSector = 0x7a;	// exclusive
	constexpr uint32_t g_headSectorFrames = (g_sectorSize - 24) / 2;
	constexpr uint32_t g_continuationSectorFrames = (g_sectorSize - 4) / 2;

	struct Slot
	{
		bool used = false;
		std::string name;
		uint32_t frames = 0;
		uint32_t sampleRate = 0;
		uint32_t sectors = 0;
		double seconds() const { return sampleRate ? static_cast<double>(frames) / sampleRate : 0.0; }
	};

	struct Directory
	{
		bool valid = false;
		std::array<Slot, g_slotCount> slots{};
		uint32_t freeSectors = 0;
		uint32_t totalSectors = g_endSector - g_firstSector;
	};

	// Reads flash and battery RAM of a Machinedrum. Call with the device locked
	// (synthLib::Plugin::withDeviceLocked); it copies about 3 KiB.
	Directory read(Hardware& _hardware);

	uint32_t sectorsForFrames(uint32_t _frames);
}

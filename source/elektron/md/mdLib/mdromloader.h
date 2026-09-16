#pragma once

#include "mdrom.h"

#include "synthLib/romLoader.h"

namespace md
{
	class RomLoader : synthLib::RomLoader
	{
	public:
		static Rom findROM();
		static Rom findROM(MachineModel _model);
		static bool isSupportedImage(size_t _size, uint64_t _fingerprint,
			MachineModel _model);
		// FNV-1a over the bytes, the fingerprint used throughout mdLib.
		static uint64_t fingerprint(const uint8_t* _data, size_t _size);
		static uint64_t fingerprint(const std::vector<uint8_t>& _data) { return fingerprint(_data.data(), _data.size()); }
		// The exact stock OS image (Machinedrum UW 1.63 / Monomachine 1.32B).
		static bool isStockRom(const std::vector<uint8_t>& _data, MachineModel _model);
		// A full flash image whose first 16 KiB are the stock boot loader, carrying
		// any OS on top: the stock one or a community firmware (X.xx, EMS).
		static bool hasStockBootLoader(const std::vector<uint8_t>& _data, MachineModel _model);
		// Stock or alternative OS; what a Device accepts when handed an image.
		static bool isRomForModel(const std::vector<uint8_t>& _data, MachineModel _model);
	};
}

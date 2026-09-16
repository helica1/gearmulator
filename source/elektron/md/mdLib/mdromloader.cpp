#include "mdromloader.h"

#include <cstdlib>

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		const auto files = findFiles(".bin", g_romSize, g_romSize);

		// the stock image wins when several images sit next to each other
		Rom alternative;
		for(const auto& file : files)
		{
			Rom rom(file);
			if(!rom.isValid())
				continue;
			if(isStockRom(rom.data(), _model))
				return rom;
			if(!alternative.isValid() && hasStockBootLoader(rom.data(), _model))
				alternative = std::move(rom);
		}
		return alternative;
	}

	bool RomLoader::isSupportedImage(const size_t _size,
		const uint64_t _fingerprint, const MachineModel _model)
	{
		if(_size != g_romSize)
			return false;
		return _model == MachineModel::Monomachine
			? _fingerprint == g_mmOs132bFingerprint
			: _fingerprint == g_mdOs163Fingerprint;
	}

	uint64_t RomLoader::fingerprint(const uint8_t* _data, const size_t _size)
	{
		uint64_t fingerprint = 14695981039346656037ull;
		for(size_t i = 0; i < _size; ++i)
		{
			fingerprint ^= _data[i];
			fingerprint *= 1099511628211ull;
		}
		return fingerprint;
	}

	bool RomLoader::isStockRom(const std::vector<uint8_t>& _data, const MachineModel _model)
	{
		return _data.size() == g_romSize && isSupportedImage(_data.size(), fingerprint(_data), _model);
	}

	bool RomLoader::hasStockBootLoader(const std::vector<uint8_t>& _data, const MachineModel _model)
	{
		// The boot loader occupies the first 16 KiB of flash, the OS starts at 0x4000.
		constexpr size_t bootLoaderSize = 0x4000;
		if(_data.size() != g_romSize)
			return false;
		const auto boot = fingerprint(_data.data(), bootLoaderSize);
		return _model == MachineModel::Monomachine
			? boot == 0x6ddfe20a3c9c1517ull
			: boot == 0x17b4e9af660cd177ull;
	}

	bool RomLoader::isRomForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		return isStockRom(_data, _model) || hasStockBootLoader(_data, _model);
	}
}

#pragma once

#include "mdtypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace md::machines
{
	// Machine catalogue for the machine selector. IDs are the firmware's machine
	// model numbers as used by the ASSIGN MACHINE SysEx command (0x5B). Machinedrum
	// IDs from 128 upwards are UW machines, sent as (id - 128) with the UW flag.
	struct Machine
	{
		uint16_t id;
		const char* name;		// firmware name, e.g. "TRX-BD"
		const char* code;		// short code shown on the tile, e.g. "BD"
		const char* description;
		bool extendedOs;		// only on community OS builds (Machinedrum X.xx)
	};

	struct Family
	{
		const char* name;		// "TRX"
		const char* title;		// "Analog"
		const char* color;		// accent colour, #rrggbb
		bool uwOnly;
		std::vector<Machine> machines;
	};

	const std::vector<Family>& families(MachineModel _model);
	const Machine* find(MachineModel _model, uint16_t _id);

	uint32_t trackCount(MachineModel _model);
	std::string trackName(MachineModel _model, uint32_t _track);	// "BD".."M4" / "TRACK 1"..

	// complete SysEx messages including F0/F7
	std::vector<uint8_t> assignMachine(MachineModel _model, uint8_t _track, uint16_t _id);
	std::vector<uint8_t> currentTrackRequest(MachineModel _model);

	// catalogue plus state as JSON for the browser panel
	std::string toJson(MachineModel _model, bool _extendedOs, int _currentTrack,
		const std::vector<std::string>& _romSlotNames);
}

#include "mdmachines.h"

#include <cstdio>

namespace md::machines
{
	namespace
	{
		std::vector<Family> makeMachinedrum()
		{
			std::vector<Family> f;
			f.push_back({"GND", "Ground", "#8a8f98", false, {
				{0, "GND---", "--", "Empty machine", false},
				{1, "GND-SN", "SN", "Sine oscillator", false},
				{2, "GND-NS", "NS", "Noise generator", false},
				{3, "GND-IM", "IM", "Impulse", false},
				{4, "GND-SW", "SW", "Saw/triangle oscillators", true},
				{5, "GND-PU", "PU", "Pulse oscillators", true},
			}});
			f.push_back({"TRX", "Analog emulation", "#f06a3a", false, {
				{16, "TRX-BD", "BD", "Bass drum", false}, {17, "TRX-SD", "SD", "Snare drum", false},
				{18, "TRX-XT", "XT", "Tom", false}, {19, "TRX-CP", "CP", "Clap", false},
				{20, "TRX-RS", "RS", "Rimshot", false}, {21, "TRX-CB", "CB", "Cowbell", false},
				{22, "TRX-CH", "CH", "Closed hihat", false}, {23, "TRX-OH", "OH", "Open hihat", false},
				{24, "TRX-CY", "CY", "Cymbal", false}, {25, "TRX-MA", "MA", "Maracas", false},
				{26, "TRX-CL", "CL", "Claves", false}, {27, "TRX-XC", "XC", "Conga", false},
				{28, "TRX-B2", "B2", "Bass drum 2", false}, {29, "TRX-S2", "S2", "Snare drum 2", false},
			}});
			f.push_back({"EFM", "FM percussion", "#3fb6e8", false, {
				{32, "EFM-BD", "BD", "FM bass drum", false}, {33, "EFM-SD", "SD", "FM snare", false},
				{34, "EFM-XT", "XT", "FM tom", false}, {35, "EFM-CP", "CP", "FM clap", false},
				{36, "EFM-RS", "RS", "FM rimshot", false}, {37, "EFM-CB", "CB", "FM cowbell", false},
				{38, "EFM-HH", "HH", "FM hihat", false}, {39, "EFM-CY", "CY", "FM cymbal", false},
			}});
			f.push_back({"E12", "12-bit samples", "#e8c547", false, {
				{48, "E12-BD", "BD", "Bass drum", false}, {49, "E12-SD", "SD", "Snare", false},
				{50, "E12-HT", "HT", "Hi tom", false}, {51, "E12-LT", "LT", "Low tom", false},
				{52, "E12-CP", "CP", "Clap", false}, {53, "E12-RS", "RS", "Rimshot", false},
				{54, "E12-CB", "CB", "Cowbell", false}, {55, "E12-CH", "CH", "Closed hihat", false},
				{56, "E12-OH", "OH", "Open hihat", false}, {57, "E12-RC", "RC", "Ride", false},
				{58, "E12-CC", "CC", "Crash", false}, {59, "E12-BR", "BR", "Brush", false},
				{60, "E12-TA", "TA", "Tambourine", false}, {61, "E12-TR", "TR", "TR tom", false},
				{62, "E12-SH", "SH", "Shaker", false}, {63, "E12-BC", "BC", "Bell cymbal", false},
			}});
			f.push_back({"P-I", "Physical models", "#7ad17a", false, {
				{64, "P-I-BD", "BD", "Modelled bass drum", false}, {65, "P-I-SD", "SD", "Modelled snare", false},
				{66, "P-I-MT", "MT", "Membrane tom", false}, {67, "P-I-ML", "ML", "Mallet", false},
				{68, "P-I-MA", "MA", "Maraca", false}, {69, "P-I-RS", "RS", "Rimshot", false},
				{70, "P-I-RC", "RC", "Ride", false}, {71, "P-I-CC", "CC", "Crash", false},
				{72, "P-I-HH", "HH", "Hihat", false},
			}});
			f.push_back({"INP", "Audio input", "#c07ae8", false, {
				{80, "INP-GA", "GA", "Gate input A", false}, {81, "INP-GB", "GB", "Gate input B", false},
				{82, "INP-FA", "FA", "Follower input A", false}, {83, "INP-FB", "FB", "Follower input B", false},
				{84, "INP-EA", "EA", "Envelope input A", false}, {85, "INP-EB", "EB", "Envelope input B", false},
				{86, "INP-CA", "CA", "Compressor input A", true}, {87, "INP-CB", "CB", "Compressor input B", true},
			}});
			f.push_back({"NFX", "Neighbour FX", "#ff5f8f", false, {
				{7, "NFX-EV", "EV", "Envelope and ring mod", true},
				{8, "NFX-CO", "CO", "Compressor", true},
				{9, "NFX-UC", "UC", "Dual comb filter", true},
			}});
			Family mid{"MID", "MIDI tracks", "#5fd3c0", false, {}};
			static const char* const midCodes[] = {"01","02","03","04","05","06","07","08","09","10","11","12","13","14","15","16"};
			for(uint16_t i = 0; i < 16; ++i)
				mid.machines.push_back({static_cast<uint16_t>(96 + i), nullptr, midCodes[i], "MIDI machine", false});
			f.push_back(mid);
			f.push_back({"CTR", "Controls", "#b8b8b8", false, {
				{112, "CTR-AL", "AL", "All tracks control", false},
				{113, "CTR-8P", "8P", "Eight parameter control", false},
				{120, "CTR-RE", "RE", "Master reverb", false},
				{121, "CTR-GB", "GB", "Master gate box", false},
				{122, "CTR-EQ", "EQ", "Master EQ", false},
				{123, "CTR-DX", "DX", "Master dynamix", false},
			}});
			Family rom{"ROM", "UW sample slots", "#ff9d3a", true, {}};
			for(uint16_t slot = 0; slot < 48; ++slot)
			{
				const uint16_t id = slot < 32 ? static_cast<uint16_t>(128 + slot) : static_cast<uint16_t>(176 + slot - 32);
				rom.machines.push_back({id, nullptr, nullptr, "Plays sample slot", false});
			}
			f.push_back(rom);
			f.push_back({"RAM", "UW record & play", "#ff4a4a", true, {
				{160, "RAM-R1", "R1", "Record buffer 1", false}, {161, "RAM-R2", "R2", "Record buffer 2", false},
				{165, "RAM-R3", "R3", "Record buffer 3", false}, {166, "RAM-R4", "R4", "Record buffer 4", false},
				{162, "RAM-P1", "P1", "Play buffer 1", false}, {163, "RAM-P2", "P2", "Play buffer 2", false},
				{167, "RAM-P3", "P3", "Play buffer 3", false}, {168, "RAM-P4", "P4", "Play buffer 4", false},
			}});
			return f;
		}

		std::vector<Family> makeMonomachine()
		{
			std::vector<Family> f;
			f.push_back({"GND", "Ground", "#8a8f98", false, {
				{0, "GND-GND", "GND", "Empty machine", false},
				{1, "GND-SIN", "SIN", "Sine oscillator", false},
				{2, "GND-NOIS", "NOIS", "Noise generator", false},
			}});
			f.push_back({"SWAVE", "SuperWave", "#f06a3a", false, {
				{4, "SWAVE-SAW", "SAW", "Detuned saws", false},
				{5, "SWAVE-PULS", "PULS", "Detuned pulses", false},
				{14, "SWAVE-ENS", "ENS", "Ensemble", false},
			}});
			f.push_back({"SID", "SID", "#7ad17a", false, {
				{3, "SID-6581", "6581", "SID chip emulation", false},
			}});
			f.push_back({"DPRO", "DigiPRO", "#3fb6e8", false, {
				{6, "DPRO-WAVE", "WAVE", "Single-cycle waveforms", false},
				{7, "DPRO-BBOX", "BBOX", "Drum box", false},
				{32, "DPRO-DDRW", "DDRW", "Draw waveforms", false},
				{33, "DPRO-DENS", "DENS", "Dense ensemble", false},
			}});
			f.push_back({"FM+", "FM", "#e8c547", false, {
				{8, "FM+-STAT", "STAT", "Static FM", false},
				{9, "FM+-PAR", "PAR", "Parallel FM", false},
				{10, "FM+-DYN", "DYN", "Dynamic FM", false},
			}});
			f.push_back({"VO", "Voice", "#c07ae8", false, {
				{11, "VO-VO-6", "VO-6", "Vocal synthesis", false},
			}});
			f.push_back({"FX", "Effects", "#ff5f8f", false, {
				{12, "FX-THRU", "THRU", "External input", false},
				{13, "FX-REVERB", "REVB", "Reverb", false},
				{15, "FX-CHORUS", "CHOR", "Chorus", false},
				{16, "FX-DYNAMIX", "DYNX", "Dynamics", false},
				{17, "FX-RINGMOD", "RING", "Ring modulator", false},
			}});
			return f;
		}

		const std::vector<Family>& mdFamilies()
		{
			static const std::vector<Family> f = makeMachinedrum();
			return f;
		}

		const std::vector<Family>& mmFamilies()
		{
			static const std::vector<Family> f = makeMonomachine();
			return f;
		}

		std::string jsonString(const std::string& _s)
		{
			std::string out = "\"";
			for(const char c : _s)
			{
				const auto u = static_cast<unsigned char>(c);
				if(c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
				else if(u < 0x20 || u > 0x7e) out.push_back('?');
				else out.push_back(c);
			}
			return out + "\"";
		}
	}

	const std::vector<Family>& families(const MachineModel _model)
	{
		return _model == MachineModel::Monomachine ? mmFamilies() : mdFamilies();
	}

	const Machine* find(const MachineModel _model, const uint16_t _id)
	{
		for(const auto& family : families(_model))
			for(const auto& machine : family.machines)
				if(machine.id == _id)
					return &machine;
		return nullptr;
	}

	uint32_t trackCount(const MachineModel _model)
	{
		return _model == MachineModel::Monomachine ? 6 : 16;
	}

	std::string trackName(const MachineModel _model, const uint32_t _track)
	{
		if(_model == MachineModel::Monomachine)
			return "TRACK " + std::to_string(_track + 1);
		static const char* const names[] = {"BD","SD","HT","MT","LT","CP","RS","CB","CH","OH","RC","CC","M1","M2","M3","M4"};
		return _track < 16 ? names[_track] : "?";
	}

	std::vector<uint8_t> assignMachine(const MachineModel _model, const uint8_t _track, const uint16_t _id)
	{
		if(_model == MachineModel::Monomachine)
			return {0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5b, static_cast<uint8_t>(_track & 0x7f), static_cast<uint8_t>(_id & 0x7f), 0x00, 0xf7};
		const bool uw = _id >= 128;
		return {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, static_cast<uint8_t>(_track & 0x7f),
			static_cast<uint8_t>((uw ? _id - 128 : _id) & 0x7f), static_cast<uint8_t>(uw ? 1 : 0), 0xf7};
	}

	std::vector<uint8_t> currentTrackRequest(const MachineModel _model)
	{
		const uint8_t product = _model == MachineModel::Monomachine ? 0x03 : 0x02;
		return {0xf0, 0x00, 0x20, 0x3c, product, 0x00, 0x70, 0x22, 0xf7};
	}

	std::string toJson(const MachineModel _model, const bool _extendedOs, const int _currentTrack,
		const std::vector<std::string>& _romSlotNames)
	{
		std::string json = "{\"model\":";
		json += _model == MachineModel::Monomachine ? "\"mm\"" : "\"md\"";
		json += ",\"extended\":" + std::string(_extendedOs ? "true" : "false");
		json += ",\"track\":" + std::to_string(_currentTrack);
		json += ",\"trackName\":" + jsonString(_currentTrack >= 0 ? trackName(_model, static_cast<uint32_t>(_currentTrack)) : "");
		json += ",\"slots\":[";
		for(size_t i = 0; i < _romSlotNames.size(); ++i)
			json += (i ? "," : "") + jsonString(_romSlotNames[i]);
		json += "],\"families\":[";
		bool firstFamily = true;
		for(const auto& family : families(_model))
		{
			json += std::string(firstFamily ? "" : ",") + "{\"name\":" + jsonString(family.name) + ",\"title\":" + jsonString(family.title)
				+ ",\"color\":" + jsonString(family.color) + ",\"machines\":[";
			firstFamily = false;
			bool firstMachine = true;
			for(size_t i = 0; i < family.machines.size(); ++i)
			{
				const auto& m = family.machines[i];
				if(m.extendedOs && !_extendedOs)
					continue;
				char buf[16];
				std::string name = m.name ? m.name : "";
				std::string code = m.code ? m.code : "";
				if(!m.name && std::string(family.name) == "MID")
				{
					std::snprintf(buf, sizeof(buf), "MID-%s", m.code);
					name = buf;
				}
				else if(!m.name && std::string(family.name) == "ROM")
				{
					std::snprintf(buf, sizeof(buf), "ROM-%02zu", i + 1);
					name = buf;
					std::snprintf(buf, sizeof(buf), "%02zu", i + 1);
					code = buf;
				}
				json += std::string(firstMachine ? "" : ",") + "{\"id\":" + std::to_string(m.id) + ",\"name\":" + jsonString(name)
					+ ",\"code\":" + jsonString(code) + ",\"desc\":" + jsonString(m.description ? m.description : "")
					+ (std::string(family.name) == "ROM" ? ",\"slot\":" + std::to_string(i) : "") + "}";
				firstMachine = false;
			}
			json += "]}";
		}
		json += "]}";
		return json;
	}
}

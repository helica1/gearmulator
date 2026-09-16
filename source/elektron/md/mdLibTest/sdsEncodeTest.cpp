// Round trip for the WAV -> SDS encoder: the stream must pass the transfer
// validator and decode back to the input samples.

#include "mdLib/mdsdsencode.h"
#include "mdLib/mdsysexfile.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
	bool check(const bool _ok, const char* _message)
	{
		if(!_ok)
			std::printf("sdsEncodeTest: %s\n", _message);
		return _ok;
	}

	std::vector<int16_t> decode(const std::vector<uint8_t>& _stream)
	{
		std::vector<int16_t> samples;
		size_t words = 0;
		for(const auto& message : md::sds::splitMessages(_stream))
		{
			if(message.size() == md::sds::g_headerSize && message[1] == 0x7e && message[3] == 1)
				words = message[10] | (size_t(message[11]) << 7) | (size_t(message[12]) << 14);
			else if(message.size() == md::sds::g_packetSize && message[1] == 0x7e && message[3] == 2)
			{
				for(size_t i = 5; i + 3 <= 125 && words; i += 3, --words)
				{
					const uint32_t value = (uint32_t(message[i]) << 14) | (uint32_t(message[i + 1]) << 7) | message[i + 2];
					samples.push_back(static_cast<int16_t>(static_cast<uint16_t>((value >> 5) ^ 0x8000u)));
				}
			}
		}
		return samples;
	}
}

int main()
{
	std::vector<int16_t> input;
	for(size_t i = 0; i < 4097; ++i)
		input.push_back(static_cast<int16_t>(std::lround(32000.0 * std::sin(i * 0.05))));
	input[0] = -32768; input[1] = 32767; input[2] = 0;

	md::sds::Options options;
	options.slot = 5;
	options.name = "kick drum";
	options.sampleRateHz = 44100;
	const auto stream = md::sds::encode(input, options);
	if(!check(!stream.empty(), "encode failed")) return 1;
	if(!check(stream.size() == md::sds::encodedSize(input.size()), "encoded size mismatch")) return 1;

	const auto validation = md::validateMidiSysexStream(stream, md::MachineModel::Machinedrum);
	if(!check(validation == md::MidiSysexStreamValidation::Valid, md::midiSysexValidationMessage(validation))) return 1;

	std::vector<md::MidiSysexMessage> messages;
	md::parseMidiSysexFile(stream, md::MachineModel::Machinedrum, &messages);
	if(!check(messages.size() == 2 + (input.size() + 39) / 40, "unexpected message count")) return 1;
	if(!check(messages[0].kind == md::MidiSysexMessageKind::SdsHeader && messages[1].kind == md::MidiSysexMessageKind::SampleName
		&& messages.back().kind == md::MidiSysexMessageKind::SdsPacket && messages.back().lastSamplePacket, "message kinds")) return 1;

	if(!check(decode(stream) == input, "round trip differs")) return 1;

	if(!check(md::sds::makeName("kick drum") == "KICK" && md::sds::makeName("a") == "A   " && md::sds::makeName("") == "    ", "name")) return 1;
	if(!check(stream[4] == 5 && stream[21 + 7] == 5, "header and name message slot")) return 1;
	if(!check(std::string(stream.begin() + 21 + 8, stream.begin() + 21 + 12) == "KICK", "name text")) return 1;

	options.slot = 48;
	if(!check(md::sds::encode(input, options).empty(), "slot 48 accepted")) return 1;
	options.slot = 0; options.loop = true; options.loopStart = 10; options.loopEnd = 5;
	if(!check(md::sds::encode(input, options).empty(), "bad loop accepted")) return 1;
	options.loopEnd = 100;
	const auto looped = md::sds::encode(input, options);
	if(!check(!looped.empty() && md::validateMidiSysexStream(looped, md::MachineModel::Machinedrum) == md::MidiSysexStreamValidation::Valid, "loop encode")) return 1;

	std::puts("sdsEncodeTest: PASS");
	return 0;
}

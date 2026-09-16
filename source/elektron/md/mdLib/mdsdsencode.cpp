#include "mdsdsencode.h"

#include <algorithm>
#include <cctype>

namespace md::sds
{
	namespace
	{
		void push21(std::vector<uint8_t>& _out, const uint32_t _value)
		{
			// three 7-bit bytes, least significant first (SDS header fields)
			_out.push_back(_value & 0x7f);
			_out.push_back((_value >> 7) & 0x7f);
			_out.push_back((_value >> 14) & 0x7f);
		}
	}

	size_t encodedSize(const size_t _sampleCount)
	{
		const auto packets = (_sampleCount + g_wordsPerPacket - 1) / g_wordsPerPacket;
		return g_headerSize + g_nameSize + packets * g_packetSize;
	}

	std::string makeName(const std::string& _text)
	{
		std::string name;
		for(const char c : _text)
		{
			const auto u = static_cast<unsigned char>(c);
			if(u < 0x20 || u > 0x7e)
				continue;
			name.push_back(static_cast<char>(std::toupper(u)));
			if(name.size() == 4)
				break;
		}
		while(name.size() < 4)
			name.push_back(' ');
		return name;
	}

	std::vector<uint8_t> encode(const std::vector<int16_t>& _samples, const Options& _options)
	{
		if(_samples.empty() || _options.slot >= g_slotCount || _options.sampleRateHz == 0)
			return {};
		const auto words = static_cast<uint32_t>(_samples.size());
		if(words >= (1u << 21))
			return {};
		uint32_t loopStart = 0, loopEnd = 0;
		uint8_t loopType = 0x7f;
		if(_options.loop)
		{
			if(_options.loopStart > _options.loopEnd || _options.loopEnd > words)
				return {};
			loopStart = _options.loopStart;
			loopEnd = _options.loopEnd;
			loopType = 0;
		}
		const uint32_t periodNs = static_cast<uint32_t>(1000000000ull / _options.sampleRateHz);

		std::vector<uint8_t> out;
		out.reserve(encodedSize(_samples.size()));

		// SDS dump header, device 0 (the Machinedrum receiver)
		out.insert(out.end(), {0xf0, 0x7e, 0x00, 0x01, static_cast<uint8_t>(_options.slot & 0x7f), 0x00, g_bitsPerSample});
		push21(out, periodNs);
		push21(out, words);
		push21(out, loopStart);
		push21(out, loopEnd);
		out.push_back(loopType);
		out.push_back(0xf7);

		// Elektron sample name, must directly follow the header
		const auto name = makeName(_options.name);
		out.insert(out.end(), {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x73, static_cast<uint8_t>(_options.slot & 0x7f)});
		for(const char c : name)
			out.push_back(static_cast<uint8_t>(c) & 0x7f);
		out.push_back(0xf7);

		// data packets: 40 words of 3 bytes, most significant first, 16 bit
		// offset-binary sample left-justified in a 21 bit field
		size_t index = 0;
		for(uint32_t packet = 0; index < _samples.size(); ++packet)
		{
			const auto begin = out.size();
			out.insert(out.end(), {0xf0, 0x7e, 0x00, 0x02, static_cast<uint8_t>(packet & 0x7f)});
			for(size_t i = 0; i < g_wordsPerPacket; ++i, ++index)
			{
				uint32_t word = 0;
				if(index < _samples.size())
				{
					const auto unsigned16 = static_cast<uint16_t>(static_cast<uint16_t>(_samples[index]) ^ 0x8000u);
					word = static_cast<uint32_t>(unsigned16) << (g_bytesPerWord * 7 - g_bitsPerSample);
				}
				out.push_back((word >> 14) & 0x7f);
				out.push_back((word >> 7) & 0x7f);
				out.push_back(word & 0x7f);
			}
			uint8_t checksum = 0;
			for(size_t i = begin + 1; i < out.size(); ++i)
				checksum ^= out[i];
			out.push_back(checksum);
			out.push_back(0xf7);
		}
		return out;
	}

	std::vector<std::vector<uint8_t>> splitMessages(const std::vector<uint8_t>& _stream)
	{
		std::vector<std::vector<uint8_t>> messages;
		size_t begin = 0;
		for(size_t i = 0; i < _stream.size(); ++i)
		{
			if(_stream[i] == 0xf0)
				begin = i;
			else if(_stream[i] == 0xf7)
				messages.emplace_back(_stream.begin() + static_cast<std::ptrdiff_t>(begin), _stream.begin() + static_cast<std::ptrdiff_t>(i) + 1);
		}
		return messages;
	}
}

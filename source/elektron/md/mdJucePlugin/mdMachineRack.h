#pragma once

#include "mdLib/mdtypes.h"

#include "juce_events/juce_events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

namespace mdJucePlugin
{
	class Editor;
	class AudioPluginAudioProcessor;

	// Machine selector below the faceplate: family tabs and machine tiles. A tile
	// assigns its machine to the machine's current track after a confirmation.
	class MachineRack final : juce::Timer
	{
	public:
		MachineRack(Editor& _editor, AudioPluginAudioProcessor& _processor, Rml::Element* _root);
		~MachineRack() override;

	private:
		void timerCallback() override;
		void build();
		void selectFamily(size_t _index);
		void rebuildTiles();
		void refreshTrack();
		void refreshSlots();
		void confirm(uint16_t _machineId, const std::string& _name);
		void markAssigned(uint16_t _machineId);

		Editor& m_editor;
		AudioPluginAudioProcessor& m_processor;
		md::MachineModel m_model;
		Rml::Element* m_root = nullptr;
		Rml::Element* m_trackBadge = nullptr;
		Rml::Element* m_tabs = nullptr;
		Rml::Element* m_grid = nullptr;
		Rml::Element* m_hint = nullptr;
		Rml::Element* m_accentLeft = nullptr;
		Rml::Element* m_accentRight = nullptr;
		std::vector<Rml::Element*> m_tabElements;
		size_t m_family = 0;
		bool m_extendedOs = false;
		int m_track = -2;
		std::vector<std::string> m_slotNames;	// "" = empty
		uint16_t m_assignedId = 0xffff;
		uint32_t m_ticks = 0;
		std::shared_ptr<void> m_lifetime = std::make_shared<int>(0);
	};
}

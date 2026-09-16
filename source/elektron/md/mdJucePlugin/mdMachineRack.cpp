#include "mdMachineRack.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdmachines.h"

#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceUiLib/messageBox.h"

#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/ElementDocument.h"

#include <cstdio>

namespace mdJucePlugin
{
	namespace
	{
		constexpr const char* g_familyConfigKey = "machineRackFamily";

		Rml::Element* addChild(Rml::Element* _parent, const char* _class, const std::string& _rml = {})
		{
			auto element = _parent->GetOwnerDocument()->CreateElement("div");
			element->SetClassNames(_class);
			if(!_rml.empty())
				element->SetInnerRML(_rml);
			return _parent->AppendChild(std::move(element));
		}

		std::string escape(const std::string& _text)
		{
			std::string out;
			for(const char c : _text)
			{
				if(c == '<') out += "&lt;";
				else if(c == '>') out += "&gt;";
				else if(c == '&') out += "&amp;";
				else out.push_back(c);
			}
			return out;
		}

		std::string tileName(const md::machines::Family& _family, const md::machines::Machine& _machine, const size_t _index)
		{
			char buf[16];
			if(_machine.name)
				return _machine.name;
			if(std::string(_family.name) == "ROM")
			{
				std::snprintf(buf, sizeof(buf), "ROM-%02zu", _index + 1);
				return buf;
			}
			std::snprintf(buf, sizeof(buf), "MID-%s", _machine.code);
			return buf;
		}
	}

	MachineRack::MachineRack(Editor& _editor, AudioPluginAudioProcessor& _processor, Rml::Element* const _root)
		: m_editor(_editor), m_processor(_processor), m_model(_processor.getModel()), m_root(_root)
	{
		m_extendedOs = m_processor.isExtendedOs();
		build();
		refreshSlots();
		// last selected family, else TRX on the Machinedrum and SuperWave on the Monomachine
		size_t initial = 1;
		const auto remembered = m_processor.getConfig().getValue(g_familyConfigKey).toStdString();
		const auto& families = md::machines::families(m_model);
		for(size_t i = 0; i < families.size(); ++i)
			if(remembered == families[i].name)
				initial = i;
		selectFamily(initial);
		refreshTrack();
		startTimerHz(4);
	}

	MachineRack::~MachineRack()
	{
		stopTimer();
	}

	void MachineRack::build()
	{
		auto* const header = addChild(m_root, "rackHeader");
		addChild(header, "rackTitle", "MACHINES");
		m_trackBadge = addChild(header, "rackTrack", "TRACK --");
		m_tabs = addChild(header, "rackTabs");
		m_hint = addChild(m_root, "rackHint", "Tap a machine to load it on the current track");
		m_accentLeft = addChild(m_root, "rackAccentLeft");
		m_accentRight = addChild(m_root, "rackAccentRight");

		const auto& families = md::machines::families(m_model);
		for(size_t i = 0; i < families.size(); ++i)
		{
			const auto& family = families[i];
			if(family.uwOnly && m_model != md::MachineModel::Machinedrum)
				continue;
			auto* const tab = addChild(m_tabs, "rackTab", family.name);
			tab->SetAttribute("title", std::string(family.title));
			m_tabElements.push_back(tab);
			const std::weak_ptr<void> lifetime = m_lifetime;
			juceRmlUi::EventListener::AddClick(tab, [this, lifetime, i]
			{
				if(!lifetime.expired())
					selectFamily(i);
			});
		}
		m_grid = addChild(m_root, "rackGrid");
	}

	void MachineRack::selectFamily(const size_t _index)
	{
		const auto& families = md::machines::families(m_model);
		if(_index >= families.size())
			return;
		m_family = _index;
		const std::string color = families[_index].color;
		m_accentLeft->SetProperty("decorator", "horizontal-gradient(" + color + "00 " + color + "c0)");
		m_accentRight->SetProperty("decorator", "horizontal-gradient(" + color + "c0 " + color + "00)");
		m_processor.getConfig().setValue(g_familyConfigKey, juce::String(families[_index].name));
		for(size_t i = 0; i < m_tabElements.size(); ++i)
		{
			const bool active = i == _index;
			m_tabElements[i]->SetClass("active", active);
			if(active)
				m_tabElements[i]->SetProperty("background-color", families[i].color);
			else
				m_tabElements[i]->RemoveProperty("background-color");
		}
		rebuildTiles();
	}

	void MachineRack::rebuildTiles()
	{
		while(m_grid->GetNumChildren() > 0)
			m_grid->RemoveChild(m_grid->GetChild(0));

		const auto& family = md::machines::families(m_model)[m_family];
		const bool rom = std::string(family.name) == "ROM";
		m_grid->SetClass("compact", rom);
		m_hint->SetInnerRML(escape(std::string(family.title)) + " \u00b7 tap a machine to load it on the current track");

		size_t shown = 0;
		for(size_t i = 0; i < family.machines.size(); ++i)
		{
			const auto& machine = family.machines[i];
			if(machine.extendedOs && !m_extendedOs)
				continue;
			++shown;
			const auto name = tileName(family, machine, i);
			auto* const tile = addChild(m_grid, "rackTile");
			auto* const stripe = addChild(tile, "rackTileStripe");
			stripe->SetProperty("background-color", family.color);

			std::string code = machine.code ? machine.code : "";
			std::string label = name;
			bool empty = false;
			if(rom)
			{
				char buf[4];
				std::snprintf(buf, sizeof(buf), "%02zu", i + 1);
				code = buf;
				const auto& slotName = i < m_slotNames.size() ? m_slotNames[i] : std::string();
				empty = !m_slotNames.empty() && slotName.empty();
				label = m_slotNames.empty() ? "" : (empty ? "empty" : slotName);
			}
			addChild(tile, "rackTileCode", escape(code));
			addChild(tile, "rackTileName", escape(label));
			tile->SetClass("empty", empty);
			tile->SetClass("assigned", machine.id == m_assignedId);
			tile->SetAttribute("title", name + (machine.description ? std::string(" - ") + machine.description : std::string()));

			const std::weak_ptr<void> lifetime = m_lifetime;
			const auto id = machine.id;
			juceRmlUi::EventListener::AddClick(tile, [this, lifetime, id, name]
			{
				if(!lifetime.expired())
					confirm(id, name);
			});
		}
		if(shown == 0)
			addChild(m_grid, "rackEmpty", "These machines need a community OS such as Machinedrum X.13 (Settings, Firmware).");
	}

	void MachineRack::timerCallback()
	{
		++m_ticks;
		refreshTrack();
		const bool extended = m_processor.isExtendedOs();
		if(extended != m_extendedOs)
		{
			m_extendedOs = extended;
			rebuildTiles();
		}
		// sample names change rarely, read them every two seconds while ROM is shown
		const auto& family = md::machines::families(m_model)[m_family];
		if(std::string(family.name) == "ROM" && m_ticks % 8 == 0)
			refreshSlots();
	}

	void MachineRack::refreshTrack()
	{
		const auto track = m_processor.getCurrentTrack();
		if(track == m_track)
			return;
		m_track = track;
		m_trackBadge->SetInnerRML(track < 0 ? std::string("TRACK --")
			: "TRACK " + std::to_string(track + 1) + (m_model == md::MachineModel::Machinedrum
				? " \u00b7 " + md::machines::trackName(m_model, static_cast<uint32_t>(track)) : std::string()));
	}

	void MachineRack::refreshSlots()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return;
		const auto directory = m_processor.readSampleDirectory();
		std::vector<std::string> names;
		if(directory)
		{
			for(const auto& slot : directory->slots)
			{
				auto name = slot.used ? slot.name : std::string();
				while(!name.empty() && name.back() == ' ')
					name.pop_back();
				if(slot.used && name.empty())
					name = "----";
				names.push_back(name);
			}
		}
		if(names == m_slotNames)
			return;
		m_slotNames = std::move(names);
		const auto& family = md::machines::families(m_model)[m_family];
		if(std::string(family.name) == "ROM")
			rebuildTiles();
	}

	void MachineRack::confirm(const uint16_t _machineId, const std::string& _name)
	{
		const auto track = m_processor.getCurrentTrack();
		if(track < 0)
		{
			genericUI::MessageBox::showOk(genericUI::MessageBox::Icon::Warning, "Machine not loaded",
				"The current track is not known yet. Wait until the machine has booted.", m_editor.getRmlComponent());
			return;
		}
		const auto trackText = "track " + std::to_string(track + 1) + (m_model == md::MachineModel::Machinedrum
			? " (" + md::machines::trackName(m_model, static_cast<uint32_t>(track)) + ")" : std::string());
		const std::weak_ptr<void> lifetime = m_lifetime;
		genericUI::MessageBox::showYesNo(genericUI::MessageBox::Icon::Question, "Load " + _name + "?",
			"Load " + _name + " on " + trackText + "? The track's current machine is replaced.",
			[this, lifetime, _machineId, _name](const genericUI::MessageBox::Result _result)
			{
				if(lifetime.expired() || _result != genericUI::MessageBox::Result::Yes)
					return;
				if(m_processor.assignMachineToCurrentTrack(_machineId))
					markAssigned(_machineId);
				else
					genericUI::MessageBox::showOk(genericUI::MessageBox::Icon::Warning, "Machine not loaded",
						_name + " could not be loaded. The current track is not known.", m_editor.getRmlComponent());
			});
	}

	void MachineRack::markAssigned(const uint16_t _machineId)
	{
		m_assignedId = _machineId;
		rebuildTiles();
	}
}

#include "mdSampleDropTarget.h"

#include "mdEditor.h"
#include "mdSampleImport.h"

#include "juceRmlUi/rmlDragData.h"
#include "juceRmlUi/rmlDragSource.h"

#include "juce_core/juce_core.h"
#include "juce_events/juce_events.h"

namespace mdJucePlugin
{
	SampleDropTarget::SampleDropTarget(Rml::Element* const _panel, Editor& _editor)
		: DragTarget(_panel), m_editor(_editor)
	{
		setAcceptDragsOverChildren(true);
		setAllowLocations(false, false);
		setAllowShift(false);
	}

	bool SampleDropTarget::canDrop(const Rml::Event& _event, const juceRmlUi::DragSource* _source)
	{
		// only files from outside (Finder, DAW); internal drags on the panel are not ours
		const auto* files = _source ? dynamic_cast<const juceRmlUi::FileDragData*>(_source->getDragData()) : nullptr;
		return files && !files->files.empty() && canDropFiles(_event, files->files);
	}

	bool SampleDropTarget::canDropFiles(const Rml::Event&, const std::vector<std::string>& _files)
	{
		if(_files.empty())
			return false;
		bool audio = false, sysex = false;
		for(const auto& path : _files)
		{
			const juce::File file(path);
			if(sampleImport::isSysexFile(file))
				sysex = true;
			else if(sampleImport::isAudioFile(file))
				audio = true;
			else
				return false;
		}
		// one SysEx file, or any number of audio files, not a mix
		return audio != sysex && (!sysex || _files.size() == 1);
	}

	void SampleDropTarget::dropFiles(const Rml::Event& _event, const juceRmlUi::FileDragData* _data, const std::vector<std::string>& _files)
	{
		DragTarget::dropFiles(_event, _data, _files);
		// The drop arrives inside RmlUi event processing; dialogs and file decoding run afterwards.
		const auto lifetime = m_editor.getLifetimeToken();
		auto* editor = &m_editor;
		juce::MessageManager::callAsync([lifetime, editor, files = _files]
		{
			if(!lifetime.expired())
				editor->importDroppedFiles(files);
		});
	}
}

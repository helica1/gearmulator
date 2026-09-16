#pragma once

#include "juceRmlUi/rmlDragTarget.h"

#include <string>
#include <vector>

namespace mdJucePlugin
{
	class Editor;

	// Accepts audio and .syx files dropped anywhere on the Machinedrum panel,
	// from Finder or from a DAW (clips and browser items arrive as files).
	class SampleDropTarget final : public juceRmlUi::DragTarget
	{
	public:
		SampleDropTarget(Rml::Element* _panel, Editor& _editor);

		bool canDrop(const Rml::Event& _event, const juceRmlUi::DragSource* _source) override;
		bool canDropFiles(const Rml::Event& _event, const std::vector<std::string>& _files) override;
		void dropFiles(const Rml::Event& _event, const juceRmlUi::FileDragData* _data, const std::vector<std::string>& _files) override;

	private:
		Editor& m_editor;
	};
}

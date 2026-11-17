#pragma once

// The list of every voice line file the game could play: loose files under Data\Sound\Voice
// plus the name tables of the .bsa archives. Only names are read, never file data.

namespace voiceindex
{
	struct Line
	{
		std::uint32_t pluginIndex;  // Into Index::plugins
		std::uint32_t infoID;       // The 8 hex digits of the file name, as a number
		std::uint16_t sourceIndex;  // Into Index::sources (0 is loose)
		std::string   fileName;     // Lowercase, with extension
	};

	struct Index
	{
		std::vector<std::string>                           plugins;      // Lowercase plugin folder names
		std::vector<std::string>                           sources;      // "loose", then archive file names
		std::unordered_map<std::string, std::vector<Line>> byVoiceType;  // Lowercase voice type folder name
	};

	// Built on first use, then kept. Not thread safe: only the worker thread calls it
	const Index& Get();

	std::string ToLower(std::string_view a_text);

	// Path relative to Data, lowercase with backslashes, as the game's resource system takes it
	std::string PathOf(const Index& a_index, const std::string& a_voiceType, const Line& a_line);

	// "quest_topic_<8 hex>_<n>.ext" parsed from the right. False for names that don't match
	bool ParseFileName(std::string_view a_lowerName, std::uint32_t& a_infoID, std::string_view& a_extension);

	// The key requests use to exclude a line: "<plugin folder>|<8 hex>"
	std::string ExcludeKey(const Index& a_index, const Line& a_line);
}

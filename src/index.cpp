#include "index.h"

#include <tuple>

namespace voiceindex
{
	namespace
	{
		std::vector<std::string> g_loadOrder;  // Lowercase plugin names, in load order
	}

	namespace
	{
		namespace fs = std::filesystem;

		using PluginIndexes = std::unordered_map<std::string, std::uint32_t>;

		std::optional<fs::path> GetDataFolder()
		{
			// The game's folder is the one holding the running executable
			const std::wstring exe(REL::Module::get().filePath());
			if (exe.empty()) {
				return std::nullopt;
			}
			return fs::path(exe).parent_path() / "Data";
		}

		// Adds a file found at Sound\Voice\<plugin>\<voice type>\<name>, if its name is a voice line name
		void AddLine(Index& a_index, PluginIndexes& a_pluginIndexes, std::string_view a_plugin, std::string_view a_voiceType, std::string_view a_file, std::uint16_t a_source)
		{
			auto             name = ToLower(a_file);
			std::uint32_t    infoID;
			std::string_view extension;
			if (!ParseFileName(name, infoID, extension)) {
				return;
			}
			const auto [it, added] = a_pluginIndexes.emplace(ToLower(a_plugin), static_cast<std::uint32_t>(a_index.plugins.size()));
			if (added) {
				a_index.plugins.push_back(it->first);
			}
			a_index.byVoiceType[ToLower(a_voiceType)].push_back({ it->second, infoID, a_source, std::move(name) });
		}

		void ScanLooseFiles(Index& a_index, PluginIndexes& a_pluginIndexes, const fs::path& a_data)
		{
			const fs::directory_iterator end;
			std::error_code              ec;
			for (fs::directory_iterator plugin(a_data / "Sound" / "Voice", ec); !ec && plugin != end; plugin.increment(ec)) {
				std::error_code ec2;
				if (!plugin->is_directory(ec2)) {
					continue;
				}
				const auto pluginName = plugin->path().filename().string();
				for (fs::directory_iterator voice(plugin->path(), ec2); !ec2 && voice != end; voice.increment(ec2)) {
					std::error_code ec3;
					if (!voice->is_directory(ec3)) {
						continue;
					}
					const auto voiceName = voice->path().filename().string();
					for (fs::directory_iterator file(voice->path(), ec3); !ec3 && file != end; file.increment(ec3)) {
						AddLine(a_index, a_pluginIndexes, pluginName, voiceName, file->path().filename().string(), 0);
					}
				}
			}
		}

		template <class T>
		bool ReadValue(std::istream& a_in, T& a_value)
		{
			return static_cast<bool>(a_in.read(reinterpret_cast<char*>(&a_value), sizeof(T)));
		}

		// Reads the folder and file names of a Skyrim (version 104) or Skyrim SE (105) archive
		void ScanArchive(Index& a_index, PluginIndexes& a_pluginIndexes, const fs::path& a_path, std::uint16_t a_source)
		{
			const auto    archiveName = a_path.filename().string();
			std::ifstream in(a_path, std::ios::binary);
			char          magic[4];
			std::uint32_t version, offset, archiveFlags, folderCount, fileCount, folderNamesLength, fileNamesLength;
			if (!in.read(magic, 4) || std::string_view(magic, 4) != "BSA\0"sv || !ReadValue(in, version) || !ReadValue(in, offset) || !ReadValue(in, archiveFlags) ||
				!ReadValue(in, folderCount) || !ReadValue(in, fileCount) || !ReadValue(in, folderNamesLength) || !ReadValue(in, fileNamesLength)) {
				logs::warn("Archive {} has no readable header, skipped", archiveName);
				return;
			}
			// Flag 0x1: folder names are included, 0x2: file names are included
			if ((version != 104 && version != 105) || !(archiveFlags & 0x1) || !(archiveFlags & 0x2)) {
				logs::info("Archive {} (version {}, flags {:#x}) has no name tables this plugin reads, skipped", archiveName, version, archiveFlags);
				return;
			}

			// Folder records: the file count of each folder is all that's needed from them
			const std::streamoff       recordSize = version == 105 ? 24 : 16;
			std::vector<std::uint32_t> counts(folderCount);
			for (std::uint32_t i = 0; i < folderCount; ++i) {
				in.seekg(static_cast<std::streamoff>(offset) + i * recordSize + 8);  // Skips the name hash
				if (!ReadValue(in, counts[i])) {
					logs::warn("Archive {} has unreadable folder records, skipped", archiveName);
					return;
				}
			}

			// The folder blocks follow the folder records: a name, then that folder's file records
			struct Folder
			{
				std::uint32_t firstFile;
				std::string   plugin, voiceType;  // Empty unless the folder is sound\voice\<plugin>\<voice type>
			};
			std::vector<Folder> folders(folderCount);
			std::uint32_t       nextFile = 0;
			in.seekg(static_cast<std::streamoff>(offset) + folderCount * recordSize);
			for (std::uint32_t i = 0; i < folderCount; ++i) {
				std::uint8_t length;
				if (!ReadValue(in, length) || length == 0) {
					logs::warn("Archive {} has an unreadable folder name, skipped", archiveName);
					return;
				}
				std::string name(length, '\0');
				if (!in.read(name.data(), length)) {
					logs::warn("Archive {} has an unreadable folder name, skipped", archiveName);
					return;
				}
				name.resize(length - 1);  // The stored length includes the trailing NUL

				constexpr auto prefix = "sound\\voice\\"sv;
				const auto     lower = ToLower(name);
				folders[i].firstFile = nextFile;
				if (lower.starts_with(prefix)) {
					const auto rest = std::string_view(lower).substr(prefix.size());
					const auto slash = rest.find('\\');
					if (slash != std::string_view::npos && rest.find('\\', slash + 1) == std::string_view::npos) {
						folders[i].plugin = rest.substr(0, slash);
						folders[i].voiceType = rest.substr(slash + 1);
					}
				}
				nextFile += counts[i];
				in.seekg(static_cast<std::streamoff>(counts[i]) * 16, std::ios::cur);  // Skips the file records
			}
			if (nextFile != fileCount) {
				logs::warn("Archive {} lists {} files in its folders but says it has {}, skipped", archiveName, nextFile, fileCount);
				return;
			}

			// The file name block: one NUL ended name per file, in file record order
			std::string names(fileNamesLength, '\0');
			if (!in.read(names.data(), fileNamesLength)) {
				logs::warn("Archive {} has an unreadable file name block, skipped", archiveName);
				return;
			}
			std::vector<std::string_view> fileNames;
			fileNames.reserve(fileCount);
			for (std::size_t start = 0; start < names.size() && fileNames.size() < fileCount;) {
				const auto end = names.find('\0', start);
				if (end == std::string::npos) {
					break;
				}
				fileNames.emplace_back(names.data() + start, end - start);
				start = end + 1;
			}
			if (fileNames.size() != fileCount) {
				logs::warn("Archive {} has {} file names but says it has {} files, skipped", archiveName, fileNames.size(), fileCount);
				return;
			}

			for (std::uint32_t i = 0; i < folderCount; ++i) {
				if (folders[i].plugin.empty()) {
					continue;
				}
				for (std::uint32_t f = 0; f < counts[i]; ++f) {
					AddLine(a_index, a_pluginIndexes, folders[i].plugin, folders[i].voiceType, fileNames[folders[i].firstFile + f], a_source);
				}
			}
		}

		Index Build()
		{
			Index index;
			index.sources.emplace_back("loose");
			PluginIndexes pluginIndexes;

			const auto data = GetDataFolder();
			if (!data) {
				logs::error("Can't find the game's Data folder");
				return index;
			}
			ScanLooseFiles(index, pluginIndexes, *data);

			// The game loads a plugin's archives (<plugin>.bsa, <plugin> - <anything>.bsa) in plugin load order, after the ones its
			// ini lists, and a later archive's copy of a file wins. Ranked last loaded first, so the copy kept below is the one the game plays
			std::vector<fs::path> archives;
			const fs::directory_iterator end;
			std::error_code              ec;
			for (fs::directory_iterator it(*data, ec); !ec && it != end; it.increment(ec)) {
				if (ToLower(it->path().extension().string()) == ".bsa") {
					archives.push_back(it->path());
				}
			}
			const auto rank = [](const fs::path& a_archive) -> std::size_t {
				const auto name = ToLower(a_archive.stem().string());
				for (std::size_t i = g_loadOrder.size(); i-- > 0;) {
					const auto& plugin = g_loadOrder[i];
					const auto  stem = plugin.substr(0, plugin.rfind('.'));
					if (name == stem || name.starts_with(stem + " - ")) {
						return i + 1;
					}
				}
				return 0;  // An archive the ini lists (or one the game doesn't load)
			};
			std::ranges::sort(archives, [&](const fs::path& a, const fs::path& b) {
				const auto rankA = rank(a), rankB = rank(b);
				return rankA != rankB ? rankA > rankB : ToLower(a.filename().string()) < ToLower(b.filename().string());
			});
			for (const auto& archive : archives) {
				index.sources.push_back(archive.filename().string());
				ScanArchive(index, pluginIndexes, archive, static_cast<std::uint16_t>(index.sources.size() - 1));
			}

			// One entry per file: loose files were added first, so a stable sort keeps them over archive copies
			std::size_t total = 0;
			for (auto& [voiceType, lines] : index.byVoiceType) {
				std::ranges::stable_sort(lines, [](const Line& a, const Line& b) { return std::tie(a.pluginIndex, a.fileName) < std::tie(b.pluginIndex, b.fileName); });
				const auto duplicates = std::ranges::unique(lines, [](const Line& a, const Line& b) { return a.pluginIndex == b.pluginIndex && a.fileName == b.fileName; });
				lines.erase(duplicates.begin(), duplicates.end());
				lines.shrink_to_fit();
				total += lines.size();
			}
			logs::info("Voice line index: {} lines, {} voice types, {} plugin folders, {} archives", total, index.byVoiceType.size(), index.plugins.size(), archives.size());
			return index;
		}
	}

	void SetLoadOrder(std::vector<std::string> a_lowerPluginNames)
	{
		g_loadOrder = std::move(a_lowerPluginNames);
	}

	const Index& Get()
	{
		static const Index index = Build();
		return index;
	}

	std::string ToLower(std::string_view a_text)
	{
		std::string out(a_text);
		for (auto& c : out) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c - 'A' + 'a');
			}
		}
		return out;
	}

	std::string PathOf(const Index& a_index, const std::string& a_voiceType, const Line& a_line)
	{
		return std::format("sound\\voice\\{}\\{}\\{}", a_index.plugins[a_line.pluginIndex], a_voiceType, a_line.fileName);
	}

	bool ParseFileName(std::string_view a_lowerName, std::uint32_t& a_infoID, std::string_view& a_extension)
	{
		const auto dot = a_lowerName.rfind('.');
		if (dot == std::string_view::npos) {
			return false;
		}
		a_extension = a_lowerName.substr(dot + 1);
		if (a_extension != "fuz" && a_extension != "xwm" && a_extension != "wav") {
			return false;
		}
		const auto stem = a_lowerName.substr(0, dot);
		const auto lastUnderscore = stem.rfind('_');
		if (lastUnderscore == std::string_view::npos || lastUnderscore + 1 >= stem.size()) {
			return false;
		}
		const auto response = stem.substr(lastUnderscore + 1);
		if (!std::ranges::all_of(response, [](char c) { return c >= '0' && c <= '9'; })) {
			return false;
		}
		const auto head = stem.substr(0, lastUnderscore);
		const auto idUnderscore = head.rfind('_');
		if (idUnderscore == std::string_view::npos || head.size() - idUnderscore - 1 != 8) {
			return false;
		}
		const auto hex = head.substr(idUnderscore + 1);
		const auto [end, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), a_infoID, 16);
		return ec == std::errc{} && end == hex.data() + hex.size();
	}

	std::string ExcludeKey(const Index& a_index, const Line& a_line)
	{
		return std::format("{}|{:08x}", a_index.plugins[a_line.pluginIndex], a_line.infoID);
	}
}

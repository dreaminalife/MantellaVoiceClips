#include "fetch.h"

#include "index.h"

#include <nlohmann/json.hpp>

namespace fetch
{
	namespace
	{
		namespace fs = std::filesystem;
		using json = nlohmann::json;
		using voiceindex::Index;
		using voiceindex::Line;

		std::unordered_map<std::string, std::string> g_pluginNames;  // Lowercase -> as the game spells it

		fs::path FromUtf8(const std::string& a_text)
		{
			return fs::path(std::u8string(a_text.begin(), a_text.end()));
		}

		// ---- Reading a voice file through the game, so the game's own file priority decides which copy is used ----

		constexpr std::uint32_t Tag(const char (&a_text)[5])
		{
			return static_cast<std::uint32_t>(static_cast<unsigned char>(a_text[0])) | static_cast<std::uint32_t>(static_cast<unsigned char>(a_text[1])) << 8 |
			       static_cast<std::uint32_t>(static_cast<unsigned char>(a_text[2])) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(a_text[3])) << 24;
		}

		// Reads front to back only: skipped bytes are read and dropped
		class Reader
		{
		public:
			explicit Reader(const std::string& a_path) :
				_stream(a_path)
			{}

			bool Good() const { return _stream.good(); }

			// Reads from the underlying resource stream directly, as CommonLib's own Translation.cpp does
			bool Read(void* a_buffer, std::uint32_t a_size)
			{
				if (a_size == 0) {
					return true;
				}
				if (!_stream.good()) {
					return false;
				}
				std::uint64_t read = 0;
				_stream.stream->DoRead(a_buffer, a_size, read);
				return read == a_size;
			}

			bool Skip(std::uint64_t a_size)
			{
				char scratch[1024];
				while (a_size > 0) {
					const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_size, sizeof(scratch)));
					if (!Read(scratch, chunk)) {
						return false;
					}
					a_size -= chunk;
				}
				return true;
			}

		private:
			RE::BSResourceNiBinaryStream _stream;
		};

		struct AudioInfo
		{
			double        seconds{ 0.0 };
			std::uint64_t size{ 0 };  // The whole file, as it can be copied
		};

		// Works out the length from the headers alone. An xWMA file's 'dpds' table ends with the number of bytes
		// the sound has once decoded, which gives the true length. The data size over the average byte rate
		// overestimates it by about a fifth, so that is only used when there is no 'dpds'.
		std::optional<AudioInfo> ReadAudioInfo(Reader& a_reader, bool a_isFuz)
		{
			std::uint64_t prefix = 0;
			if (a_isFuz) {
				std::uint32_t head[3];  // "FUZE", version, size of the lip sync data
				if (!a_reader.Read(head, sizeof(head)) || head[0] != Tag("FUZE") || !a_reader.Skip(head[2])) {
					return std::nullopt;
				}
				prefix = sizeof(head) + head[2];
			}

			std::uint32_t riff[3];  // "RIFF", size, "XWMA" or "WAVE"
			if (!a_reader.Read(riff, sizeof(riff)) || riff[0] != Tag("RIFF") || (riff[2] != Tag("XWMA") && riff[2] != Tag("WAVE"))) {
				return std::nullopt;
			}
			const bool isXwma = riff[2] == Tag("XWMA");

			AudioInfo info;
			info.size = prefix + riff[1] + 8;

			bool          haveFormat = false;
			std::uint32_t channels = 0, sampleRate = 0, bytesPerSecond = 0, bitsPerSample = 0;
			std::uint32_t decodedBytes = 0, dataSize = 0;
			bool          haveDecodedBytes = false, haveData = false;
			for (int i = 0; i < 32 && !haveData; ++i) {
				std::uint32_t chunk[2];  // id, size
				if (!a_reader.Read(chunk, sizeof(chunk))) {
					break;
				}
				const auto size = chunk[1];
				const auto padding = size & 1;
				if (chunk[0] == Tag("fmt ")) {
					if (size < 16 || size > 256) {
						return std::nullopt;
					}
					std::vector<std::uint8_t> bytes(size);
					if (!a_reader.Read(bytes.data(), size) || !a_reader.Skip(padding)) {
						return std::nullopt;
					}
					std::uint16_t channels16, bits16;
					std::memcpy(&channels16, &bytes[2], 2);
					std::memcpy(&sampleRate, &bytes[4], 4);
					std::memcpy(&bytesPerSecond, &bytes[8], 4);
					std::memcpy(&bits16, &bytes[14], 2);
					channels = channels16;
					bitsPerSample = bits16;
					haveFormat = true;
				} else if (chunk[0] == Tag("dpds")) {
					if (size < 4 || size > (1u << 24)) {
						return std::nullopt;
					}
					std::vector<std::uint8_t> bytes(size);
					if (!a_reader.Read(bytes.data(), size) || !a_reader.Skip(padding)) {
						return std::nullopt;
					}
					std::memcpy(&decodedBytes, &bytes[size / 4 * 4 - 4], 4);  // The last whole entry
					haveDecodedBytes = true;
				} else if (chunk[0] == Tag("data")) {
					dataSize = size;
					haveData = true;
				} else if (!a_reader.Skip(static_cast<std::uint64_t>(size) + padding)) {
					return std::nullopt;
				}
			}

			if (!haveFormat) {
				return std::nullopt;
			}
			const double decodedBytesPerSecond = static_cast<double>(sampleRate) * channels * (bitsPerSample / 8);
			if (isXwma && haveDecodedBytes && decodedBytesPerSecond > 0) {
				info.seconds = decodedBytes / decodedBytesPerSecond;
			} else if (haveData && bytesPerSecond > 0) {
				info.seconds = static_cast<double>(dataSize) / bytesPerSecond;
			} else {
				return std::nullopt;
			}
			return info;
		}

		std::optional<AudioInfo> OpenAndReadInfo(const std::string& a_path)
		{
			Reader reader(a_path);
			if (!reader.Good()) {
				return std::nullopt;
			}
			return ReadAudioInfo(reader, a_path.ends_with(".fuz"));
		}

		// Writes the file's bytes unchanged. False if the game can't open it or it is shorter than its header says
		bool CopyToFile(const std::string& a_path, std::uint64_t a_size, const fs::path& a_target)
		{
			Reader reader(a_path);
			if (!reader.Good()) {
				return false;
			}
			std::ofstream out(a_target, std::ios::binary | std::ios::trunc);
			if (!out) {
				return false;
			}
			std::vector<char> buffer(64 * 1024);
			for (std::uint64_t left = a_size; left > 0;) {
				const auto chunk = static_cast<std::uint32_t>(std::min<std::uint64_t>(left, buffer.size()));
				if (!reader.Read(buffer.data(), chunk) || !out.write(buffer.data(), chunk)) {
					return false;
				}
				left -= chunk;
			}
			return static_cast<bool>(out.flush());
		}

		// ---- Whose line is it ----

		enum class Owner
		{
			kGone,    // No such INFO in the loaded game
			kShared,  // Nobody in particular: no GetIsID condition
			kNpc,     // Only the wanted NPC says it
			kOther,   // Someone else's
		};

		Owner ClassifyInfo(const RE::TESTopicInfo& a_info, const RE::TESNPC* a_npc)
		{
			using Function = RE::FUNCTION_DATA::FunctionID;
			bool anyGetIsID = false, forNpc = false;
			for (auto* item = a_info.objConditions.head; item; item = item->next) {
				const auto& data = item->data;
				if (data.functionData.function.get() != Function::kGetIsID) {
					continue;
				}
				anyGetIsID = true;
				// "GetIsID <NPC> == 1" run on the speaker
				const auto* param = static_cast<const RE::TESForm*>(data.functionData.params[0]);
				if (a_npc && param && param->As<RE::TESNPC>() == a_npc && data.object.get() == RE::CONDITIONITEMOBJECT::kSelf &&
					data.flags.opCode == RE::CONDITION_ITEM_DATA::OpCode::kEqualTo && !data.flags.global && data.comparisonValue.f == 1.0f) {
					forNpc = true;
				}
			}
			return forNpc ? Owner::kNpc : (anyGetIsID ? Owner::kOther : Owner::kShared);
		}

		// The INFO's form ID inside its plugin. A voice file's name can carry a load order byte (an override, or FE for a light plugin)
		// that LookupForm would add to the plugin's own
		std::uint32_t LocalInfoID(RE::TESDataHandler& a_dataHandler, const std::string& a_plugin, std::uint32_t a_infoID)
		{
			const auto* file = a_dataHandler.LookupModByName(a_plugin);
			return a_infoID & (file && file->IsLight() ? 0xFFFu : 0xFFFFFFu);
		}

		// ---- One request ----

		struct Clip
		{
			std::string   forWhat;
			std::string   path;
			std::string   plugin;
			std::uint32_t infoID;
			std::string   source;
			std::uint16_t sourceIndex;
			AudioInfo     info;
		};

		struct Outcome
		{
			std::string        status{ "nothing_found" };
			std::string        message;
			std::vector<Clip>  clips;
			std::vector<std::string> missing;
			std::vector<std::pair<std::string, std::size_t>> voiceTypes;  // For a list request: editor ID, NPCs using it
		};

		std::string DisplayPluginName(const std::string& a_folder)
		{
			const auto it = g_pluginNames.find(a_folder);
			return it != g_pluginNames.end() ? it->second : a_folder;
		}

		// The game's own plugins. Their lines are the original recordings, or a voice replacer's copies of them at the same paths.
		// Lines a mod adds sit in that mod's own folder and are often stitched together from other lines or made with AI
		bool IsGamePlugin(std::string_view a_lowerName)
		{
			return a_lowerName == "skyrim.esm" || a_lowerName == "update.esm" || a_lowerName == "dawnguard.esm" || a_lowerName == "hearthfires.esm" ||
			       a_lowerName == "dragonborn.esm";
		}

		bool IsGameLine(const Index& a_index, const Line& a_line)
		{
			return IsGamePlugin(a_index.plugins[a_line.pluginIndex]);
		}

		class Picker
		{
		public:
			Picker(const Request& a_request, const Index& a_index, const std::string& a_voiceType) :
				_request(a_request), _index(a_index), _voiceType(a_voiceType), _rng(std::random_device{}())
			{}

			// Shuffles the pool, puts the game's own lines first, then reads headers until there are enough lines of the right length.
			// Lines added by mods are only taken when the game's own lines run out
			std::vector<Clip> Pick(std::vector<const Line*> a_pool, const char* a_forWhat)
			{
				std::ranges::shuffle(a_pool, _rng);
				std::ranges::stable_partition(a_pool, [&](const Line* a_line) { return IsGameLine(_index, *a_line); });
				std::vector<Clip> picked;
				for (const auto* line : a_pool) {
					if (picked.size() >= _request.count) {
						break;
					}
					auto path = voiceindex::PathOf(_index, _voiceType, *line);
					const auto info = OpenAndReadInfo(path);
					if (!info || info->seconds < _request.minSeconds || info->seconds > _request.maxSeconds) {
						continue;
					}
					picked.push_back({ a_forWhat, std::move(path), DisplayPluginName(_index.plugins[line->pluginIndex]), line->infoID, _index.sources[line->sourceIndex], line->sourceIndex, *info });
				}
				return picked;
			}

		private:
			const Request&    _request;
			const Index&      _index;
			const std::string _voiceType;
			std::mt19937      _rng;
		};

		// The source (loose, or an archive) most of these lines are played from
		std::uint16_t MostCommonSource(const std::vector<const Line*>& a_lines)
		{
			std::unordered_map<std::uint16_t, std::size_t> counts;
			std::uint16_t                                  best = 0;
			std::size_t                                    bestCount = 0;
			for (const auto* line : a_lines) {
				const auto count = ++counts[line->sourceIndex];
				if (count > bestCount) {
					best = line->sourceIndex;
					bestCount = count;
				}
			}
			return best;
		}

		std::uint32_t MostCommonPlugin(const std::vector<const Line*>& a_lines)
		{
			std::unordered_map<std::uint32_t, std::size_t> counts;
			std::uint32_t                                  best = 0;
			std::size_t                                    bestCount = 0;
			for (const auto* line : a_lines) {
				const auto count = ++counts[line->pluginIndex];
				if (count > bestCount) {
					best = line->pluginIndex;
					bestCount = count;
				}
			}
			return best;
		}

		std::vector<const Line*> GameLines(const Index& a_index, const std::vector<const Line*>& a_lines)
		{
			std::vector<const Line*> result;
			std::ranges::copy_if(a_lines, std::back_inserter(result), [&](const Line* a_line) { return IsGameLine(a_index, *a_line); });
			return result;
		}

		// Whether a voice mod changed this NPC's voice and not their voice type's: their own lines are played from a different
		// place (a loose file or another archive) than the shared lines. Judged by the most common place, so a patch that fixes a
		// few lines (the Unofficial Patch) doesn't count. Lines that mods add (follower dialogue, comments) live in the mod's own
		// folder as loose files and say nothing about the voice, so the game's own lines are compared whenever the NPC has any
		bool SoundsDifferent(const Request& a_request, const Index& a_index, const std::vector<const Line*>& a_own, const std::vector<const Line*>& a_shared,
			const std::vector<Clip>& a_voiceClips)
		{
			const auto ownGame = GameLines(a_index, a_own);
			if (!ownGame.empty()) {
				const auto sharedGame = GameLines(a_index, a_shared);
				if (sharedGame.empty()) {
					// A voice type of the NPC's own in the game (Serana): the voice type's clip is already theirs
					logs::info("[{}] The voice type has no shared lines in the game's own plugins, so it is the NPC's voice", a_request.id);
					return false;
				}
				// Within the same plugin's lines where possible: the game's own lines for Dawnguard NPCs are in Dawnguard's archive
				const auto plugin = MostCommonPlugin(ownGame);
				std::vector<const Line*> sharedSamePlugin;
				std::ranges::copy_if(sharedGame, std::back_inserter(sharedSamePlugin), [&](const Line* a_line) { return a_line->pluginIndex == plugin; });
				const auto ownSource = MostCommonSource(ownGame);
				const auto sharedSource = MostCommonSource(sharedSamePlugin.empty() ? sharedGame : sharedSamePlugin);
				logs::info("[{}] The NPC's lines in the game's own plugins are played from {}, the shared ones from {}", a_request.id, a_index.sources[ownSource],
					a_index.sources[sharedSource]);
				return ownSource != sharedSource;
			}
			if (IsGamePlugin(voiceindex::ToLower(a_request.npcPlugin))) {
				// An NPC of the game whose only own lines were added by mods: those mods didn't change the voice
				logs::info("[{}] The NPC's own lines all come from mods, which add lines but keep the voice", a_request.id);
				return false;
			}
			// An NPC added by a mod: their own lines vs the voice type's
			const auto ownSource = MostCommonSource(a_own);
			const auto sharedSource = a_shared.empty() ? a_voiceClips.front().sourceIndex : MostCommonSource(a_shared);
			logs::info("[{}] The NPC's lines are played from {}, the shared ones from {}", a_request.id, a_index.sources[ownSource], a_index.sources[sharedSource]);
			return ownSource != sharedSource;
		}

		void PickLines(const Request& a_request, Outcome& a_outcome)
		{
			const auto& index = voiceindex::Get();
			const auto  voiceType = voiceindex::ToLower(a_request.voiceType);
			const auto  folder = index.byVoiceType.find(voiceType);
			if (folder == index.byVoiceType.end()) {
				a_outcome.message = std::format("No voice lines found for voice type {}", a_request.voiceType);
				return;
			}

			const bool wantNpc = a_request.npcLines != "none";
			auto*      dataHandler = RE::TESDataHandler::GetSingleton();
			const RE::TESNPC* npc = nullptr;
			if (wantNpc && dataHandler && !a_request.npcPlugin.empty() && a_request.npcLocalID != 0) {
				npc = dataHandler->LookupForm<RE::TESNPC>(a_request.npcLocalID, a_request.npcPlugin);
				if (!npc) {
					logs::warn("[{}] NPC {:06X} in {} not found", a_request.id, a_request.npcLocalID, a_request.npcPlugin);
				}
			}

			// Sort the voice type's lines into shared ones, the NPC's own and the rest
			const std::unordered_set<std::string> excluded(a_request.exclude.begin(), a_request.exclude.end());
			std::vector<const Line*>              usable, shared, own;
			std::unordered_map<std::uint64_t, Owner> owners;
			for (const auto& line : folder->second) {
				if (std::ranges::any_of(a_request.skipWords, [&](const std::string& word) { return line.fileName.find(word) != std::string::npos; }) ||
					excluded.contains(voiceindex::ExcludeKey(index, line))) {
					continue;
				}
				usable.push_back(&line);
				if (!a_request.voiceTypeLines && !npc) {
					continue;
				}
				const auto key = (static_cast<std::uint64_t>(line.pluginIndex) << 32) | line.infoID;
				auto       cached = owners.find(key);
				if (cached == owners.end()) {
					auto owner = Owner::kGone;
					const auto realName = g_pluginNames.find(index.plugins[line.pluginIndex]);
					if (dataHandler && realName != g_pluginNames.end()) {
						if (const auto* info = dataHandler->LookupForm<RE::TESTopicInfo>(LocalInfoID(*dataHandler, realName->second, line.infoID), realName->second)) {
							owner = ClassifyInfo(*info, npc);
						}
					}
					cached = owners.emplace(key, owner).first;
				}
				if (cached->second == Owner::kShared) {
					shared.push_back(&line);
				} else if (cached->second == Owner::kNpc) {
					own.push_back(&line);
				}
			}
			logs::info("[{}] {}: {} usable lines, {} shared, {} the NPC's own", a_request.id, a_request.voiceType, usable.size(), shared.size(), own.size());

			Picker picker(a_request, index, voiceType);
			std::vector<Clip> voiceClips, npcClips;
			if (a_request.voiceTypeLines) {
				voiceClips = picker.Pick(shared, "voice_type");
				if (voiceClips.empty()) {
					// Nothing shared qualifies (a unique voice type's lines are nearly all someone's own): take any line
					logs::info("[{}] No shared line qualifies, using any line of the voice type", a_request.id);
					voiceClips = picker.Pick(usable, "voice_type");
				}
			}
			if (wantNpc) {
				// A unique voice type's clip may already be one of the NPC's own lines: never deliver the same line twice
				std::unordered_set<std::string> taken;
				for (const auto& clip : voiceClips) {
					taken.insert(clip.path);
				}
				std::erase_if(own, [&](const Line* a_line) { return taken.contains(voiceindex::PathOf(index, voiceType, *a_line)); });
				npcClips = picker.Pick(own, "npc");
				if (a_request.npcLines == "if_different" && !npcClips.empty() && !voiceClips.empty() && !SoundsDifferent(a_request, index, own, shared, voiceClips)) {
					logs::info("[{}] The NPC sounds like the voice type: their own lines are not delivered", a_request.id);
					npcClips.clear();
				}
			}
			a_outcome.clips = std::move(voiceClips);
			a_outcome.clips.insert(a_outcome.clips.end(), std::make_move_iterator(npcClips.begin()), std::make_move_iterator(npcClips.end()));
			if (a_outcome.clips.empty()) {
				a_outcome.message = "No line of the right length was found";
			}
		}

		// The voice types the game's NPCs use and that have voice lines, most used first. An NPC without a voice type of its
		// own (most leveled ones) speaks with its race's default for its sex
		void ListVoiceTypes(const Request& a_request, Outcome& a_outcome)
		{
			auto* dataHandler = RE::TESDataHandler::GetSingleton();
			if (!dataHandler) {
				throw std::runtime_error("the game's data isn't loaded");
			}
			std::unordered_map<std::string, std::pair<std::string, std::size_t>> counts;  // Lowercase -> editor ID, NPCs
			for (auto* npc : dataHandler->GetFormArray<RE::TESNPC>()) {
				if (!npc) {
					continue;
				}
				const RE::BGSVoiceType* voice = npc->voiceType;
				if (!voice && npc->race) {
					voice = npc->race->defaultVoiceTypes[npc->IsFemale() ? RE::SEXES::kFemale : RE::SEXES::kMale];
				}
				const char* editorID = voice ? voice->GetFormEditorID() : nullptr;
				if (!editorID || !*editorID) {
					continue;
				}
				auto& entry = counts[voiceindex::ToLower(editorID)];
				if (entry.first.empty()) {
					entry.first = editorID;
				}
				++entry.second;
			}
			const auto& index = voiceindex::Get();
			for (auto& [lower, entry] : counts) {
				if (index.byVoiceType.contains(lower)) {
					a_outcome.voiceTypes.push_back(std::move(entry));
				}
			}
			std::ranges::sort(a_outcome.voiceTypes, [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
			logs::info("[{}] {} voice types used by NPCs have lines, out of {}", a_request.id, a_outcome.voiceTypes.size(), counts.size());
		}

		// Copies exactly the lines asked for, each only if it can still be opened
		void CheckLines(const Request& a_request, Outcome& a_outcome)
		{
			const auto& index = voiceindex::Get();
			for (const auto& wanted : a_request.lines) {
				auto path = voiceindex::ToLower(wanted.path);
				std::ranges::replace(path, '/', '\\');

				// sound\voice\<plugin>\<voice type>\<file>
				std::vector<std::string> parts;
				for (std::size_t start = 0; start <= path.size();) {
					const auto end = (std::min)(path.find('\\', start), path.size());
					parts.push_back(path.substr(start, end - start));
					start = end + 1;
				}
				std::uint32_t    infoID = 0;
				std::string_view extension;
				if (parts.size() != 5 || parts[0] != "sound" || parts[1] != "voice" || parts[2].empty() || parts[2] == ".." || parts[3].empty() || parts[3] == ".." ||
					!voiceindex::ParseFileName(parts[4], infoID, extension)) {
					a_outcome.missing.push_back(wanted.path);
					continue;
				}
				const auto info = OpenAndReadInfo(path);
				if (!info) {
					a_outcome.missing.push_back(wanted.path);
					continue;
				}

				std::string   source = "unknown";
				std::uint16_t sourceIndex = 0;
				if (const auto folder = index.byVoiceType.find(parts[3]); folder != index.byVoiceType.end()) {
					for (const auto& line : folder->second) {
						if (line.fileName == parts[4] && index.plugins[line.pluginIndex] == parts[2]) {
							source = index.sources[line.sourceIndex];
							sourceIndex = line.sourceIndex;
							break;
						}
					}
				}
				a_outcome.clips.push_back({ wanted.forWhat, std::move(path), DisplayPluginName(parts[2]), infoID, std::move(source), sourceIndex, *info });
			}
			if (a_outcome.clips.empty()) {
				a_outcome.message = "None of the lines could be opened";
			}
		}

		void WriteResult(const Request& a_request, const fs::path& a_folder, const Outcome& a_outcome)
		{
			json clips = json::array();
			for (std::size_t i = 0; i < a_outcome.clips.size(); ++i) {
				const auto& clip = a_outcome.clips[i];
				clips.push_back(json{ { "file", std::format("{}{}", i + 1, clip.path.substr(clip.path.rfind('.'))) },
					{ "for", clip.forWhat },
					{ "path", clip.path },
					{ "plugin", clip.plugin },
					{ "info", std::format("{:08X}", clip.infoID) },
					{ "size", clip.info.size },
					{ "source", clip.source },
					{ "seconds", std::round(clip.info.seconds * 100.0) / 100.0 } });
			}
			json result = { { "id", a_request.id }, { "status", a_outcome.status }, { "message", a_outcome.message }, { "clips", clips }, { "missing", a_outcome.missing } };
			if (a_request.listVoiceTypes) {
				json voiceTypes = json::array();
				for (const auto& [name, npcs] : a_outcome.voiceTypes) {
					voiceTypes.push_back(json{ { "name", name }, { "npcs", npcs } });
				}
				result["voice_types"] = std::move(voiceTypes);
			}

			// Written last and renamed into place, so result.json existing means the request is done
			const auto temporary = a_folder / "result.json.tmp";
			{
				std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
				out << result.dump(2, ' ', false, json::error_handler_t::replace);
				if (!out.flush()) {
					throw std::runtime_error("can't write result.json");
				}
			}
			fs::rename(temporary, a_folder / "result.json");
		}
	}

	std::optional<Request> Parse(const std::string& a_json)
	{
		try {
			const auto j = json::parse(a_json, nullptr, false);
			if (!j.is_object()) {
				return std::nullopt;
			}
			Request request;
			request.id = j.value("id", ""s);
			request.dest = j.value("dest", ""s);
			request.voiceType = j.value("voice_type", ""s);
			request.npcPlugin = j.value("npc_plugin", ""s);
			request.npcLocalID = j.value("npc_local_id", 0u);
			request.voiceTypeLines = j.value("voice_type_lines", true);
			request.npcLines = j.value("npc_lines", "none"s);
			request.count = j.value("count", 4u);
			request.minSeconds = j.value("min_seconds", 0.0);
			request.maxSeconds = j.value("max_seconds", 1e9);
			for (const auto& word : j.value("skip_words", json::array())) {
				if (auto lower = voiceindex::ToLower(word.get<std::string>()); !lower.empty()) {
					request.skipWords.push_back(std::move(lower));
				}
			}
			for (const auto& key : j.value("exclude", json::array())) {
				request.exclude.push_back(voiceindex::ToLower(key.get<std::string>()));
			}
			for (const auto& line : j.value("lines", json::array())) {
				request.lines.push_back({ line.value("path", ""s), line.value("for", "voice_type"s) });
			}
			request.listVoiceTypes = j.value("list_voice_types", false);
			if (request.id.empty() || request.dest.empty() || (request.lines.empty() && request.voiceType.empty() && !request.listVoiceTypes)) {
				return std::nullopt;
			}
			return request;
		} catch (const std::exception&) {
			return std::nullopt;
		}
	}

	void ReadPluginNames()
	{
		g_pluginNames.clear();
		std::vector<std::pair<std::uint32_t, std::string>> loaded;  // Load order position, lowercase name
		if (auto* dataHandler = RE::TESDataHandler::GetSingleton()) {
			for (const auto* file : dataHandler->files) {
				if (file && file->compileIndex != 0xFF) {
					const std::string name(file->GetFilename());
					g_pluginNames.emplace(voiceindex::ToLower(name), name);
					// Full plugins by their load order index; light ones (all at FE) after them, by their own index
					const auto position = file->IsLight() ? (0xFE000u | file->smallFileCompileIndex) : (static_cast<std::uint32_t>(file->compileIndex) << 12);
					loaded.emplace_back(position, voiceindex::ToLower(name));
				}
			}
		}
		std::ranges::sort(loaded);
		std::vector<std::string> loadOrder;
		for (auto& [position, name] : loaded) {
			loadOrder.push_back(std::move(name));
		}
		voiceindex::SetLoadOrder(std::move(loadOrder));
		logs::info("Found {} loaded plugins", g_pluginNames.size());
	}

	int Run(const Request& a_request)
	{
		logs::info("[{}] Request: voice type {}, NPC {:06X} in {}, npc lines {}, count {}, {} fixed lines", a_request.id, a_request.voiceType, a_request.npcLocalID,
			a_request.npcPlugin, a_request.npcLines, a_request.count, a_request.lines.size());

		const auto folder = FromUtf8(a_request.dest);
		Outcome    outcome;
		try {
			fs::create_directories(folder);
			fs::remove(folder / "result.json");  // A leftover from an earlier run would say "done" too soon

			if (a_request.listVoiceTypes) {
				ListVoiceTypes(a_request, outcome);
				outcome.status = outcome.voiceTypes.empty() ? "nothing_found" : "ok";
				WriteResult(a_request, folder, outcome);
				logs::info("[{}] Done: {} voice types", a_request.id, outcome.voiceTypes.size());
				return static_cast<int>(outcome.voiceTypes.size());
			}
			if (a_request.lines.empty()) {
				PickLines(a_request, outcome);
			} else {
				CheckLines(a_request, outcome);
			}

			// Copy what was chosen; a clip that can't be copied is left out
			std::vector<Clip> written;
			for (auto& clip : outcome.clips) {
				const auto name = std::format("{}{}", written.size() + 1, clip.path.substr(clip.path.rfind('.')));
				if (CopyToFile(clip.path, clip.info.size, folder / name)) {
					logs::info("[{}] {} <- {} ({}, {:.2f} s, {} bytes)", a_request.id, name, clip.path, clip.source, clip.info.seconds, clip.info.size);
					written.push_back(std::move(clip));
				} else {
					logs::warn("[{}] Could not copy {}", a_request.id, clip.path);
					outcome.missing.push_back(clip.path);
				}
			}
			outcome.clips = std::move(written);
			outcome.status = outcome.clips.empty() ? "nothing_found" : "ok";
			if (outcome.clips.empty() && outcome.message.empty()) {
				outcome.message = "No line could be copied";
			}
		} catch (const std::exception& e) {
			outcome.status = "error";
			outcome.message = e.what();
			outcome.clips.clear();
			logs::error("[{}] {}", a_request.id, e.what());
		}

		try {
			WriteResult(a_request, folder, outcome);
		} catch (const std::exception& e) {
			logs::error("[{}] Could not write result.json: {}", a_request.id, e.what());
			return -1;
		}
		logs::info("[{}] Done: {}, {} clips", a_request.id, outcome.status, outcome.clips.size());
		return outcome.status == "error" ? -1 : static_cast<int>(outcome.clips.size());
	}
}

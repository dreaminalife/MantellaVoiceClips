#pragma once

// One FetchClips request: picks voice lines, copies them to a folder and writes result.json there.

namespace fetch
{
	struct RequestLine
	{
		std::string path;
		std::string forWhat;  // "voice_type" or "npc"
	};

	struct Request
	{
		std::string              id;
		std::string              dest;
		std::string              voiceType;
		std::string              npcPlugin;
		std::uint32_t            npcLocalID{ 0 };
		bool                     voiceTypeLines{ true };
		std::string              npcLines{ "none" };  // "none", "if_different" or "always"
		std::uint32_t            count{ 4 };
		double                   minSeconds{ 0.0 };
		double                   maxSeconds{ 1e9 };
		std::vector<std::string> skipWords;  // Lowercase
		std::vector<std::string> exclude;    // Lowercase
		std::vector<RequestLine> lines;      // When not empty, copy exactly these
		bool                     listVoiceTypes{ false };  // Copy nothing: list the voice types the game's NPCs use instead
	};

	// Nothing if the text isn't a JSON object with at least an id and a dest
	std::optional<Request> Parse(const std::string& a_json);

	// Remembers the real spelling of every loaded plugin's file name. Call on the main thread once the game's data is loaded
	void ReadPluginNames();

	// Does the whole request. Returns the number of clips written, 0 when nothing was found, -1 on error
	int Run(const Request& a_request);
}

// MantellaVoiceClips: copies voice lines from the game for Mantella's voice services.
//
// Mantella-Spell hands this plugin a request (see README.md). A worker thread picks voice lines, copies the
// files to the folder the request names and writes result.json there. When it is done, a mod event tells
// Papyrus. The Papyrus call itself returns at once.

#include "fetch.h"

namespace
{
	std::mutex                g_lock;
	std::deque<fetch::Request> g_queue;
	bool                      g_working = false;  // A worker thread is running and will look at the queue again before it ends

	// Tells Papyrus on the main thread: Mantella_VoiceClipsDone, strArg = request id, numArg = clips written (0 none, -1 error)
	void SendDone(std::string a_id, int a_count)
	{
		SKSE::GetTaskInterface()->AddTask([id = std::move(a_id), a_count]() {
			if (auto* source = SKSE::GetModCallbackEventSource()) {
				SKSE::ModCallbackEvent event{ RE::BSFixedString("Mantella_VoiceClipsDone"), RE::BSFixedString(id.c_str()), static_cast<float>(a_count), nullptr };
				source->SendEvent(&event);
			}
		});
	}

	// Handles requests one at a time until the queue is empty
	void Work()
	{
		for (;;) {
			fetch::Request request;
			{
				std::scoped_lock lock(g_lock);
				if (g_queue.empty()) {
					g_working = false;
					return;
				}
				request = std::move(g_queue.front());
				g_queue.pop_front();
			}
			int count = -1;
			try {
				count = fetch::Run(request);
			} catch (const std::exception& e) {
				logs::error("[{}] {}", request.id, e.what());
			}
			SendDone(request.id, count);
		}
	}

	RE::BSFixedString GetSourcePlugin(RE::StaticFunctionTag*, RE::TESForm* a_form)
	{
		const auto* file = a_form ? a_form->GetFile(0) : nullptr;
		const std::string name(file ? file->GetFilename() : std::string_view());
		return RE::BSFixedString(name.c_str());
	}

	std::int32_t GetLocalFormID(RE::StaticFunctionTag*, RE::TESForm* a_form)
	{
		if (!a_form) {
			return 0;
		}
		const auto id = a_form->GetFormID();
		return static_cast<std::int32_t>((id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF));
	}

	bool FetchClips(RE::StaticFunctionTag*, std::string a_requestJson)
	{
		auto request = fetch::Parse(a_requestJson);
		if (!request) {
			logs::warn("FetchClips: the request can't be read");
			return false;
		}
		std::scoped_lock lock(g_lock);
		logs::info("[{}] Queued ({} waiting)", request->id, g_queue.size());
		g_queue.push_back(std::move(*request));
		if (!g_working) {
			g_working = true;
			std::thread(Work).detach();
		}
		return true;
	}

	bool RegisterFunctions(RE::BSScript::IVirtualMachine* a_vm)
	{
		constexpr auto script = "MantellaVoiceClips"sv;
		a_vm->RegisterFunction("GetSourcePlugin"sv, script, GetSourcePlugin);
		a_vm->RegisterFunction("GetLocalFormID"sv, script, GetLocalFormID);
		a_vm->RegisterFunction("FetchClips"sv, script, FetchClips);
		return true;
	}
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	SKSE::GetPapyrusInterface()->Register(RegisterFunctions);
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_message) {
		if (a_message->type == SKSE::MessagingInterface::kDataLoaded) {
			fetch::ReadPluginNames();
		}
	});

	logs::info("MantellaVoiceClips loaded");
	return true;
}

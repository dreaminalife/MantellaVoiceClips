# MantellaVoiceClips

An SKSE plugin for [Mantella](https://github.com/art-from-the-machine/Mantella). It finds voice lines in the game and copies them to a folder, so Mantella's voice services can use them as the sample of what a voice sounds like.

Mantella-Spell hands the plugin a request: which voice type, optionally which NPC, how many lines, how long they should be. The plugin lists the game's voice files (loose files and the names inside .bsa archives; archive contents are never unpacked), picks some at random (the game's own recordings first, lines added by mods only when those run out), and copies the chosen files as they are, with the game's own file priority deciding which copy is used. A voice replacer is therefore picked up like any other file. The work happens on a background thread and the Papyrus call returns at once.

One DLL works with Skyrim SE, AE and VR. It needs SKSE and the Address Library for SKSE Plugins.

## Papyrus

```papyrus
Scriptname MantellaVoiceClips Hidden

String Function GetSourcePlugin(Form akForm) Global Native
Int Function GetLocalFormID(Form akForm) Global Native
Bool Function FetchClips(String requestJson) Global Native
```

`GetSourcePlugin` gives the file name of the plugin that defines a form (`""` if none). `GetLocalFormID` gives its ID without the load order part. `FetchClips` queues a request; it returns false if the JSON can't be read. Requests are handled one at a time.

When a request is finished the plugin sends the mod event `Mantella_VoiceClipsDone` with `strArg` = the request id and `numArg` = the number of clip files written (0 = nothing found, -1 = error).

## Request

```json
{
  "id": "vc17",
  "dest": "C:\Users\x\Documents\My Games\Mantella\voice_clips\incoming\vc17",
  "voice_type": "FemaleEvenToned",
  "npc_plugin": "Skyrim.esm", "npc_local_id": 667284,
  "voice_type_lines": true,
  "npc_lines": "if_different",
  "count": 4,
  "min_seconds": 5.0, "max_seconds": 10.0,
  "skip_words": ["bardsongs"],
  "exclude": ["skyrim.esm|000dd60f"],
  "lines": []
}
```

- `voice_type`: the voice type's editor ID, which is the folder name under `Sound\Voice\<plugin>\`. Folders of that name are matched in every plugin.
- `npc_plugin`, `npc_local_id`: the NPC's base form. Can be left out for a request about the voice type alone.
- `voice_type_lines`: deliver up to `count` shared lines of the voice type (lines that aren't tied to one NPC by a `GetIsID` condition). If no shared line qualifies, such as for a unique voice like Serana's, any line of the voice type is used.
- `npc_lines`: `none`, `if_different` or `always`. The NPC's own lines are those with a `GetIsID` condition for this NPC. `always` delivers up to `count` of them. `if_different` delivers them only when most of the NPC's own lines come from a different place (a loose file or a particular .bsa) than most of the shared lines; this is how a voice replacer for one NPC is noticed, while a patch that fixes a few lines is not mistaken for one. Only lines in the game's own plugins (Skyrim.esm, Update.esm and the three DLCs) are compared when the NPC has any, since mods that add lines (follower dialogue, comments) keep them as loose files in their own folder without changing the voice. An NPC of the game whose only own lines come from mods keeps the shared clip; for an NPC added by a mod, all their own lines are compared.
- `count`: lines to deliver per kind (default 4). `min_seconds`, `max_seconds`: only lines in this length range.
- `skip_words`: lines whose file name contains one of these are dropped. `exclude`: lines never to deliver, as `<plugin folder>|<INFO id, 8 hex digits>`, lowercase.
- `lines`: if not empty, the picking fields are ignored and exactly these lines are copied, each only if it can still be opened: `{"path": "sound\voice\skyrim.esm\femaleeventoned\dialoguesolitude__000dd60f_1.fuz", "for": "voice_type"}` (`for` is `voice_type` or `npc`). The ones that can't be opened are listed in `missing`.

Line length is worked out from the file headers without decoding the sound.

## Result

The clip files are written to `dest` as `1.fuz`, `2.fuz`, and so on, then `result.json` is written last (through `result.json.tmp`), so its existence means the request is done:

```json
{"id": "vc17", "status": "ok", "message": "",
 "clips": [{"file": "1.fuz", "for": "voice_type", "path": "sound\voice\skyrim.esm\femaleeventoned\dialoguesolitude__000dd60f_1.fuz",
            "plugin": "Skyrim.esm", "info": "000DD60F", "size": 27047, "source": "loose", "seconds": 4.64}],
 "missing": []}
```

`status` is `ok`, `nothing_found` or `error` (an error always has a `message`). For each `for` value, the first clip is the pick and the rest are alternatives to listen to. `source` is `loose` or the name of the .bsa that lists the file.

## Building

The DLL is built by GitHub Actions on every push (`.github/workflows/build.yml`); download it from the run's Artifacts. To build locally you need VS 2022 with the C++ workload and [xmake](https://xmake.io) 3.0+:

```bat
git clone --recurse-submodules <this repository>
xmake config --mode=releasedbg
xmake build
```

The output is in `build/windows/x64/releasedbg/`.

## License

The source code in this repository is MIT licensed (see `LICENSE`). The built DLL statically links [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR) (`lib/commonlibsse-ng`), which is GPL-3.0 with a modding exception, so the DLL is distributed under those terms.

Started from the [commonlibsse-ng-template](https://github.com/libxse/commonlibsse-ng-template).

Scriptname MantellaVoiceClips Hidden

; File name of the plugin that defines the form (its first source file), "" if none
String Function GetSourcePlugin(Form akForm) Global Native

; The form's ID without its load order part: formID & 0xFFF for a light plugin (0xFE prefix), else & 0xFFFFFF
Int Function GetLocalFormID(Form akForm) Global Native

; Queues a request (JSON, see README.md). False if the JSON can't be read, true = queued. Returns at once.
; When it is done the plugin sends the mod event "Mantella_VoiceClipsDone" (strArg = request id,
; numArg = clip files written, 0 = nothing found, -1 = error) and result.json exists in the request's dest folder
Bool Function FetchClips(String requestJson) Global Native

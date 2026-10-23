-- Summoner's Rift's own music: the old Summoner's Rift soundtrack (owner, 2026-10-22: "is there a way we
-- can get the league of legends music to also play during this bg"), one 23-minute MP3 in an archive of
-- its own - patch-N.MPQ, which launchers from 1.1.25 install - so data updates never download it again.
--
-- PlayMusic loops it on the music channel, over the zone's own music, from the moment the player is in
-- the Rift until it leaves; the game's music volume and its music switch apply as to any music. A client
-- without patch-N plays nothing (PlayMusic of a missing file is silent). /riftmusic turns it off and on
-- for the account.

local RIFT_ZONE = "Summoner's Rift";
local SONG = "Sound\\Music\\Centurion\\SummonersRift\\OldSummonersRift.mp3";

local playing = false;

local function Enabled()
	return type(CENTURION_RiftMusic) ~= "table" or CENTURION_RiftMusic.off ~= true;
end

local function Update()
	local want = Enabled() and GetRealZoneText() == RIFT_ZONE;
	if ( want and not playing ) then
		PlayMusic(SONG);
		playing = true;
	elseif ( playing and not want ) then
		StopMusic();
		playing = false;
	end
end

local watch = CreateFrame("Frame");
watch:RegisterEvent("PLAYER_ENTERING_WORLD");
watch:RegisterEvent("ZONE_CHANGED_NEW_AREA");
watch:SetScript("OnEvent", Update);

SLASH_CENTURIONRIFTMUSIC1 = "/riftmusic";
SlashCmdList["CENTURIONRIFTMUSIC"] = function()
	if ( type(CENTURION_RiftMusic) ~= "table" ) then
		CENTURION_RiftMusic = {};
	end
	CENTURION_RiftMusic.off = Enabled();
	DEFAULT_CHAT_FRAME:AddMessage(Enabled() and "Summoner's Rift music: on." or "Summoner's Rift music: off.", 1, 0.82, 0.25);
	Update();
end

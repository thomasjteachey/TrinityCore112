export const DEFAULT_LAUNCHER_UPDATE_URL = 'https://centurionpvp.com/downloads/';
export const DEFAULT_REALMLIST = 'centurionpvp.com';
export const DEFAULT_AZEROTHCORE_REALMLIST = 'centurionpvp.com:3726';

export const REALM_IDS = [
	'legionnaire_plus',
	'legionnaire',
	'barracks',
	'barracks_plus',
	'centurion',
	'centurion_dev',
	'trinityworld',
	'townsendboys'
] as const;
export type RealmId = (typeof REALM_IDS)[number];

// Everything not listed here (centurion_dev included) only shows up, and can
// only stay selected, while the launcher is in dev mode.
export const PUBLIC_REALM_IDS = ['centurion'] as const satisfies readonly RealmId[];

// Legionnaire+ and Barracks+ merged into Centurion on 2026-09-17 and left the
// realmlist. They stay in REALM_IDS so a saved preference naming them still
// parses (an unknown id would reset every preference), but no mode offers them
// and a launcher that had one selected moves to DEFAULT_REALM_ID. Their FileMap
// entries stay too: that is what moves their realm-only archives (patch-enUS-8,
// patch-enUS-T, patch-4) out of Data on the first verify after the switch.
export const RETIRED_REALM_IDS = [
	'legionnaire_plus',
	'barracks_plus'
] as const satisfies readonly RealmId[];

export const DEFAULT_REALM_ID: RealmId = 'centurion';

export const isRetiredRealm = (id: RealmId) =>
	(RETIRED_REALM_IDS as readonly RealmId[]).includes(id);

type BuildInfo = {
	string: string;
	number: number;
};

const BUILD_12340: BuildInfo = {
	string: '12340',
	number: 12340
};

const BUILD_12341: BuildInfo = {
	string: '12341',
	number: 12341
};

const BUILD_12342: BuildInfo = {
	string: '12342',
	number: 12342
};

export const REALMS: Record<
	RealmId,
	{
		label: string;
		realmName: string;
		build: BuildInfo;
		realmlistType: 'trinitycore' | 'azerothcore';
	}
> = {
	legionnaire_plus: {
		label: 'Legionnaire+',
		realmName: 'Legionnaire Plus',
		build: BUILD_12341,
		realmlistType: 'trinitycore'
	},
	legionnaire: {
		label: 'Legionnaire',
		realmName: 'Legionnaire',
		build: BUILD_12340,
		realmlistType: 'trinitycore'
	},
	barracks: {
		label: 'Barracks',
		realmName: 'Barracks',
		build: BUILD_12342,
		realmlistType: 'trinitycore'
	},
	barracks_plus: {
		label: 'Barracks+',
		realmName: 'Barracks Plus',
		build: BUILD_12342,
		realmlistType: 'trinitycore'
	},
	// Barracks+-based realms (realmlist ids 7 and 8). realmName has to match
	// legionnaireauth.realmlist.name exactly for the client to land on them.
	centurion: {
		label: 'Centurion',
		realmName: 'Centurion',
		build: BUILD_12342,
		realmlistType: 'trinitycore'
	},
	centurion_dev: {
		label: 'Centurion Dev',
		realmName: 'CenturionDev',
		build: BUILD_12342,
		realmlistType: 'trinitycore'
	},
	trinityworld: {
		label: 'TRINITYWORLD',
		realmName: 'TRINITYWORLD',
		build: BUILD_12340,
		realmlistType: 'trinitycore'
	},
	townsendboys: {
		label: 'TOWNSENDBOYS',
		realmName: 'TOWNSENDBOYS',
		build: BUILD_12340,
		realmlistType: 'azerothcore'
	}
};

export const FileMap: Record<
	string,
	{
		extractPath: string;
		optional?: true;
		label?: string;
		description?: string;
		realms?: RealmId[];
		/**
		 * Remote basename to fetch instead when the launcher is in dev mode.
		 * The archive still contains the REAL .MPQ name, so it drops into the
		 * client exactly where the live patch would - only the download source
		 * differs. Path is relative to downloads/patches/.
		 */
		devFile?: string;
		/**
		 * Remote basename to fetch instead of the key, in every mode (devFile
		 * still wins in dev mode). Lets an archive take a new name on disk while
		 * the server keeps publishing it under the old one.
		 */
		remoteFile?: string;
		/** Archive entries written to disk under a different name. */
		extractAs?: Record<string, string>;
		/**
		 * The FileMap key this patch used to be installed under. A client whose
		 * cache still records it there, and nothing under this key, gets the files
		 * renamed per extractAs, keeping the recorded version, instead of
		 * downloading the archive again.
		 */
		migrateFrom?: string;
		/**
		 * Files in extractPath deleted whenever this patch is not in use, so an
		 * older archive that shipped under the same name on disk stops loading.
		 */
		removeWhenUnused?: string[];
	}
> = {
	['addons']: { extractPath: 'Interface/Addons' },
	['patch-enUS-4']: { extractPath: 'Data/enUS', realms: ['townsendboys'] },
	// Centurion and Centurion Dev take exactly the Barracks+ patch set: their
	// servers were cloned from Barracks+ (DBCs included) on 2026-09-16. Once
	// their data diverges they need a patch letter of their own, the way
	// Barracks+ split off onto patch-enUS-A.
	['patch-enUS-6']: {
		extractPath: 'Data/enUS',
		realms: [
			'legionnaire',
			'legionnaire_plus',
			'barracks',
			'barracks_plus',
			'centurion',
			'centurion_dev'
		]
	},
	['patch-enUS-7']: {
		extractPath: 'Data/enUS',
		realms: [
			'legionnaire',
			'legionnaire_plus',
			'barracks',
			'barracks_plus',
			'centurion',
			'centurion_dev'
		]
	},
	['patch-enUS-8']: {
		extractPath: 'Data/enUS',
		realms: ['legionnaire_plus']
	},
	// The dev DBC delta, layered above patch-enUS-8 exactly as patch-Z is
	// layered above patch-Y. 'T' sorts after '8', so the client loads this last
	// and it wins.
	//
	// It used to be patch-enUS-8's devFile, which meant the test archive
	// carried patch-enUS-8.MPQ and OVERWROTE the live patch on every dev
	// launch. Anything published straight into the live patch was silently
	// reverted the next time the launcher ran, and the only symptom was
	// "my change didn't take". Layering instead of replacing removes that
	// whole class of confusion, and keeps the download to the few DBCs the
	// forge actually modified rather than a 55MB copy of the base.
	['patch-enUS-T']: {
		extractPath: 'Data/enUS',
		realms: ['legionnaire_plus'],
		devFile: 'itemforge/patch-enUS-T-test'
	},
	['patch-enUS-9']: { extractPath: 'Data/enUS', realms: ['barracks'] },
	['patch-enUS-A']: {
		extractPath: 'Data/enUS',
		realms: ['barracks_plus', 'centurion', 'centurion_dev']
	},
	['patch-4']: { extractPath: 'Data', realms: ['legionnaire_plus'] },
	// The production art base. patch-Z used to carry all of it; it is now the
	// small dev-only delta layered above this, so a texture save no longer
	// makes every dev redownload 700MB. On patch day the delta is merged down
	// into the art base and the production patch-Z goes back to (nearly) empty.
	//
	// It was installed as patch-Y.MPQ until 2026-09-17, and moved down a letter
	// to free patch-Y for the optional World Terrain pack. The server still
	// publishes it as patch-Y.zip / patch-Y.version, because launchers from
	// before the move keep reading that name, and so do the forge and
	// promote_patch_z.py; the entry inside is still patch-Y.MPQ and is written
	// to disk as patch-X.MPQ. Clients that already hold it are renamed in place.
	['patch-X']: {
		extractPath: 'Data',
		remoteFile: 'patch-Y',
		extractAs: { 'patch-Y.MPQ': 'patch-X.MPQ' },
		migrateFrom: 'patch-Y'
	},
	['patch-Z']: { extractPath: 'Data', devFile: 'itemforge/patch-Z-test' },
	// The in-instance floor maps. Barracks Plus was added 2026-09-04: it had
	// never been on this list, because when the patch was introduced B+ still
	// shared 'barracks' patches, and the split onto its own patch-enUS-A left
	// this entry keyed on the old realm name alone. The symptom is a dungeon
	// map that draws blank parchment with an empty floor selector, since the
	// archive carrying WorldMapArea/DungeonMap and the Interface\WorldMap art
	// is simply absent from the client.
	['patch-dungeon-maps']: {
		extractPath: 'Data',
		realms: [
			'barracks',
			'barracks_plus',
			'centurion',
			'centurion_dev',
			'townsendboys'
		]
	},
	['hd-creatures']: {
		extractPath: 'Data',
		optional: true,
		label: 'HD Creatures',
		description: 'Higher resolution retail creature models'
	},
	['hd-textures']: {
		extractPath: 'Data',
		optional: true,
		label: 'HD Textures',
		description: 'Higher resolution retail textures'
	},
	['hd-spells']: {
		extractPath: 'Data',
		optional: true,
		label: 'HD Spells',
		description: 'Higher resolution retail spell visuals'
	},
	['hd-bgs']: {
		extractPath: 'Data',
		optional: true,
		label: 'HD Battlegrounds',
		description: 'Higher detail retail battleground maps'
	},
	['hd-misc']: {
		extractPath: 'Data',
		optional: true,
		label: 'HD Interface',
		description: 'Shadowlands style user interface'
	},
	// Reznik's world art (patch-Y.MPQ), lifted from his client patches: his
	// versions of World, Tileset, Dungeons and Environments textures, his
	// replacement doodads and WMOs where they cannot move collision, and his
	// lighting and Skywall skybox. No terrain ships, so every model replaces a
	// file clients already load under the same name and stock placements stay.
	// A model ships when neither version collides, or when its collision surface
	// matches the original at 95% or better; anything else keeps the original.
	//
	// His files replace the art base's copies too (the owner's call), so an art
	// base update to one of those files will not show for players with the toggle
	// on until it is copied into this archive as well. Never shipped from his
	// patches: DBCs other than Light.dbc, UI, creature, character, item and spell
	// art, sounds.
	//
	// Light.dbc is the realms' copy (patch-enUS-A and -8 are identical) with his
	// light edits merged in, minus battleground maps and the lobby's light.
	// Because 'Y' ranks above those patches, a later Light.dbc change there has
	// to be merged into this archive too, or players with the toggle on will not
	// see it.
	//
	// While the toggle is off, any patch-Y.MPQ is deleted: before the art base
	// moved to patch-X.MPQ, every client carried it under that name, and a
	// leftover copy would keep loading above patch-X and shadow its updates.
	['world-terrain']: {
		extractPath: 'Data',
		optional: true,
		label: 'Alt World',
		description: "Reznik's world textures, models, skyboxes and lighting",
		removeWhenUnused: ['patch-Y.MPQ']
	},
	// Client-side fixes: vanilla stealth movement speed, and stopping spell
	// animations from destroying melee swing animations.
	//
	// Ships dinput8.dll into the client root. Wow.exe statically imports
	// DirectInput8Create and dinput8 is not a KnownDLL, so the local copy loads
	// during process init.
	//
	// Mandatory (no `optional`), so it installs for everyone.
	//
	// Renamed from 'stealth-glide' in 1.1.8 once it covered more than stealth.
	// The old key is still served so launchers on 1.1.7 keep working; both
	// deliver the same dinput8.dll, so a client mid-migration just overwrites
	// the same file. Retire stealth-glide.* from the server once 1.1.7 is gone.
	['client-tweaks']: {
		extractPath: '.'
	}
};

import path from 'path';

import { screen } from 'electron';
import fs from 'fs-extra';

import Preferences from '~main/modules/preferences';
import { isNotUndef } from '~common/utils';
import Logger from '~main/modules/logger';
import Updater from '~main/modules/updater';
import {
        DEFAULT_AZEROTHCORE_REALMLIST,
        DEFAULT_REALM_ID,
        DEFAULT_REALMLIST,
        REALMS
} from '~common/constants';

// Data/<locale>/realmlist.wtf takes precedence over Config.wtf's realmList, so a
// stale one has to be removed. The locale directory used to be hardcoded as
// 'enUs', which only matched on case-insensitive Windows volumes. Under
// Proton/Wine the client sits on a case-sensitive filesystem, so the remove
// silently did nothing and the stale file kept overriding the realm.
const removeRealmlistOverrides = async (clientDir: string) => {
	const dataDir = path.join(clientDir, 'Data');
	if (!(await fs.pathExists(dataDir))) return;

	const entries = await fs.readdir(dataDir).catch(() => [] as string[]);
	await Promise.all(
		entries.map(async entry => {
			const localeDir = path.join(dataDir, entry);
			const stats = await fs.stat(localeDir).catch(() => undefined);
			if (!stats?.isDirectory()) return;

			const files = await fs.readdir(localeDir).catch(() => [] as string[]);
			await Promise.all(
				files
					.filter(file => file.toLowerCase() === 'realmlist.wtf')
					.map(file => fs.remove(path.join(localeDir, file)))
			);
		})
	);
};

export const patchConfig = async () => {
        const {
                clientDir,
                selectedRealm,
                realmList,
                azerothcoreRealmList,
                clientInitialized,
                borderlessFullscreen
        } = Preferences.data;
        if (!clientDir) return;

        const realmKey = selectedRealm ?? DEFAULT_REALM_ID;
        const realmConfig = REALMS[realmKey];

        await Updater.ensureRealmPatchesFor(realmKey);

        // Patch WoW.exe
        const exePath = path.join(clientDir, 'WoW.exe');
        const original = await fs.readFile(exePath);
        const buffer = Buffer.from(original);
        buffer.fill(0x00, 0x5f3a00, 0x5f3a00 + 6);
        buffer.write(realmConfig.build.string, 0x5f3a00, realmConfig.build.string.length);
        buffer.writeUInt16LE(realmConfig.build.number, 0x4c99f0);

        const writeByte = (offset: number, value: number) => buffer.writeUInt8(value, offset);
        const writeBytes = (offset: number, values: number[]) =>
                values.forEach((value, index) => buffer.writeUInt8(value, offset + index));
        const fillBytes = (offset: number, value: number, count: number) =>
                buffer.fill(value, offset, offset + count);

        // SIG & MD5 Protection Remover
        (
                [
                        [0x1f41bf, 0xeb],
                        [0x415a25, 0xeb],
			[0x415a3f, 0x03],
			[0x415a95, 0x03],
                        [0x415b46, 0xeb],
                        [0x415b5f, [0xb8, 0x03, 0x00, 0x00, 0x00, 0xeb, 0xed]]
		] as [number, number | number[]][]
	).forEach(([offset, value]) => {
		if (Array.isArray(value)) {
			value.forEach((v, i) => buffer.writeUInt8(v, offset + i));
		} else {
                        buffer.writeUInt8(value, offset);
                }
        });

        // Maximized-windowed rendered as true borderless fullscreen.
        //
        // Stock 3.3.5a 12340 has 0x74 (JZ) here, verified byte-for-byte against a
        // clean client; the patch makes it 0xEB (JMP) so the branch is always
        // taken. Plain windowed is unaffected either way - this only changes what
        // the maximized/borderless path does.
        //
        // Toggling it off writes the STOCK byte rather than simply skipping the
        // write, so an already-patched client is reverted on the next launch
        // instead of being stuck with borderless forever. Needed for Wine, Proton
        // and WoWSilicon, where the borderless path triggers a real display-mode
        // change that blanks other monitors and locks the mouse.
        writeByte(0x0e94, borderlessFullscreen ? 0xeb : 0x74);

        // melee swing on right-click
        fillBytes(0x2e1c67, 0x90, 11);

        // NPC attack animation when turning
        writeByte(0x33d7c9, 0xeb);

        // "ghost" attack when NPC evades from combat
        writeByte(0x0355bf, 0xeb);

        // missing pre cast animation when canceling channeled spells
        fillBytes(0x33e0d6, 0x90, 22);

        // mouse flickering and camera snapping issue when mouse has high report rate
        // credits to bonbigz
        writeBytes(0x469a2c, [0xe9, 0x71, 0xf0, 0x0b, 0x00, 0xf8, 0x13, 0xd4, 0x00, 0x8b, 0x1d, 0xfc]);
        writeBytes(0x528aa2, [
                0x8d,
                0x4d,
                0xf0,
                0x51,
                0x57,
                0xff,
                0x15,
                0xdc,
                0xf5,
                0x9d,
                0x00,
                0x8b,
                0x45,
                0xf0,
                0x8b,
                0x15,
                0xf8,
                0x13,
                0xd4,
                0x00,
                0xe9,
                0x7a,
                0x0f,
                0xf4,
                0xff
        ]);
        writeBytes(0x4691b1, [
                0x89,
                0xe5,
                0x8b,
                0x05,
                0xfc,
                0x13,
                0xd4,
                0x00,
                0x8b,
                0x0d,
                0xf8,
                0x13,
                0xd4,
                0x00,
                0xeb,
                0xc2,
                0x7d,
                0x03,
                0x83,
                0xc1,
                0x01,
                0x83,
                0xc0,
                0x32,
                0x83,
                0xc1,
                0x32,
                0x3b,
                0x0d,
                0xec,
                0xbc,
                0xca,
                0x00,
                0x7e,
                0x03,
                0x83,
                0xe9,
                0x01,
                0x3b,
                0x05,
                0xf0,
                0xbc,
                0xca,
                0x00,
                0x7e,
                0x03,
                0x83,
                0xe8,
                0x01,
                0x83,
                0xe9,
                0x32,
                0x83,
                0xe8,
                0x32,
                0x89,
                0x0d,
                0xf8,
                0x13,
                0xd4,
                0x00,
                0x89,
                0x05,
                0xfc,
                0x13,
                0xd4,
                0x00,
                0x89,
                0xec,
                0x5d,
                0xe9,
                0xb4,
                0xf7,
                0xff,
                0xff,
                0xec,
                0x5d,
                0xc3,
                0xc3
        ]);
        writeBytes(0x469183, [
                0x83,
                0xf8,
                0x32,
                0x7d,
                0x03,
                0x83,
                0xc0,
                0x01,
                0x83,
                0xf9,
                0x32,
                0xeb,
                0x31
        ]);

        // naked character issue (disables SPELL_AURA_X_RAY)
        writeByte(0x1ddc5d, 0xeb);

        // patches missiles impacting with terrain
        // (spell effects such as Typhoon will lose their visual effect once they impact with terrain)
        // (Typhoon is spanwed on the ground so it loses its visual most of the time)
        // WARNING: This might break other ground impact spell visuals (e.g. Wintergrasp Cannons)
        // writeByte(0x1fc99e, 0x00);
        // writeByte(0x1fc8c7, 0x00);
        // writeByte(0x1fc735, 0x00);

        // patch mail request timeout
        // you no longer need to wait 60 seconds or relog to receive new mail
        writeBytes(0x16d899, [0x05, 0x01, 0x00, 0x00, 0x00]);

        // patch area trigger timer to be more precise (250ms -> 50ms)
        writeByte(0x2db241, 50);

        // Return of "The Blue Moon"
        writeBytes(0x5cfbc0, [0xc7, 0x05, 0x74, 0x8e, 0xd3, 0x00, 0xff, 0xff, 0xff, 0xff, 0xc3]);

        // Allow chat commands while dead
        writeByte(0x10ca41, 0xeb);

        // Character list of up to 20 characters. The SMSG_CHAR_ENUM handler
        // (0x464C10) empties any list longer than this byte - stock 0x0A (10) -
        // via `cmp byte ptr [ebp-1], 0Ah` at 0x464C4C. The list storage is sized
        // from the count (0x464B30), so nothing else is fixed at 10. Harmless on
        // realms whose server still sends at most 10 (CharactersPerRealm).
        writeByte(0x6404f, 0x14);

        // Large-address-aware: lift the 2 GB address-space ceiling to 4 GB.
        //
        // The image is 32-bit and stock 3.3.5a leaves IMAGE_FILE_LARGE_ADDRESS_AWARE
        // (0x0020) clear in its COFF characteristics, so the process is capped at 2 GB
        // of user address space. Once that fragments the client dies with "Not enough
        // memory resources are available to process this command", blamed on whichever
        // allocation happened to land next - M2Shared.cpp:267, MapObjRead.cpp:391,
        // MapMem.cpp:558 - for requests as small as 88 KB. The HD patches, a high
        // farclip and wide resolutions make that routine rather than rare.
        //
        // Only the flag bit moves. The image is never relocated (no DYNAMICBASE, base
        // stays 0x400000), so every offset above and every hook address in dinput8.dll
        // still resolves. The PE checksum is deliberately left stale: the patches above
        // already invalidate it, and Windows verifies checksums only for drivers and
        // DLLs in protected processes, never for a user-mode game.
        //
        // 64-bit Windows honours the flag directly. Wine and Proton honour it too,
        // though there the 4 GB is shared with Wine's own 32-bit libraries. On a 32-bit
        // OS it is inert rather than harmful.
        //
        // e_lfanew is read instead of hardcoded, so unlike every patch above this one
        // writes to a computed offset - hence the signature guard, which keeps a
        // garbage value from scribbling on an arbitrary part of the image.
        const peHeaderOffset = buffer.readUInt32LE(0x3c);
        if (
                peHeaderOffset > 0 &&
                peHeaderOffset + 0x18 <= buffer.length &&
                buffer.readUInt32LE(peHeaderOffset) === 0x00004550 // 'PE\0\0'
        ) {
                const characteristics = peHeaderOffset + 0x16;
                buffer.writeUInt16LE(buffer.readUInt16LE(characteristics) | 0x0020, characteristics);
        }

	// The byte patches are deterministic, so from the second launch onward the
	// client is already patched and rewriting it is pure risk for no gain.
	if (buffer.equals(original)) {
		void Logger.log('WoW.exe is already patched, leaving it untouched');
	} else {
		// Temp file + rename. The previous plain writeFile truncated a multi-megabyte
		// executable in place on every launch, so an interrupted or failed write left
		// the player with a corrupt WoW.exe and no way back.
		const tempExePath = `${exePath}.launcher-tmp`;
		await fs.writeFile(tempExePath, buffer);
		await fs.move(tempExePath, exePath, { overwrite: true });
		void Logger.log('WoW.exe patched');
	}

	await removeRealmlistOverrides(clientDir);
	await fs.ensureDir(path.join(clientDir, 'WTF'));

	const configPath = path.join(clientDir, 'WTF', 'Config.wtf');
	const raw = (await fs.exists(configPath))
		? await fs.readFile(configPath, { encoding: 'utf-8' })
		: '';
	// Deliberately not removed here. The file is replaced atomically below, so a
	// crash between delete and write can no longer leave the player with no config.
	const eol = raw.includes('\r\n') ? '\r\n' : '\n';

	const configWtf = Object.fromEntries(
		raw
			.split('\n')
			.map(l => {
				// '.*' rather than '.+': WoW legitimately writes keys with empty
				// values, and the old pattern dropped every one of them on each launch.
				const [_, k, v] = l.match(/^\s*SET\s+(\w+)\s+"(.*)"/) ?? [];
				return !k ? undefined : [k, v ?? ''];
			})
			.filter(isNotUndef)
	);

	const primaryDisplay = screen.getPrimaryDisplay();
	// bounds is reported in DIPs. On a display running at 125% or 150% scaling
	// that seeds a non-native mode (2560x1440 at 125% would seed 2048x1152), so
	// convert back to physical pixels.
	const { scaleFactor } = primaryDisplay;
	const width = Math.round(primaryDisplay.bounds.width * scaleFactor);
	const height = Math.round(primaryDisplay.bounds.height * scaleFactor);

        const realmHost =
                realmConfig.realmlistType === 'azerothcore'
                        ? azerothcoreRealmList ?? DEFAULT_AZEROTHCORE_REALMLIST
                        : realmList ?? DEFAULT_REALMLIST;

        const realmInfo = {
                realmList: realmHost,
                patchList: realmHost,
                realmName: realmConfig.realmName
        };

	// Written only the first time we touch a given client. WoW drops any cvar
	// whose value equals the client default when it rewrites Config.wtf, so after
	// the player picks a default-valued option the key simply disappears from the
	// file. Seeding on every launch would read that absence as "never set" and put
	// the launcher's value back, which is what kept forcing maximized windowed
	// mode back on. Ordering still matters for the first run: these sit before the
	// ...configWtf spread so an existing config always wins.
	// Keyed purely off the stored flag. Presence of a Config.wtf says nothing:
	// the packaged client distributions ship one, so gating on its absence would
	// mean a brand new install never got seeded at all.
	const isFirstSeed = !clientInitialized;
	const seeded = isFirstSeed
		? {
				gxResolution: `${width}x${height}`,
				gxWindow: 1, // Maximized windowed mode
				gxMaximize: 1, // Maximized windowed mode
				gxCursor: 1, // Hardware cursor
				checkAddonVersion: 0 // Load out of date addons
		  }
		: {};

	const parsed = {
		...seeded,
		// gxColorBits: primaryDisplay.colorDepth,
		// gxDepthBits: primaryDisplay.colorDepth,
		// gxRefresh: 60,
		// gxMultisample: 8,
		// gxMultisampleQuality: 0,
		// gxTripleBuffer: 1,
		// anisotropic: 16,
		// frillDensity: 48,
		// fullAlpha: 1,
		// SmallCull: 0.01,
		// DistCull: 888.8,
		// shadowLevel: 0,
		// trilinear: 1,
		// specular: 1,
		// pixelShaders: 1,
		// M2UsePixelShaders: 1,
		// particleDensity: 1,
		// unitDrawDist: 300,
		// weatherDensity: 3,
		// movieSubtitle: 1,
		// minimapZoom: 0,
		// minimapInsideZoom: 0,
		// SoundZoneMusicNoDelay: 1,
		// Parsed config
		...configWtf,
		// Realm list
		...realmInfo,
		// Mandatory. hwDetect is not exposed in WoW's options UI and exists only to
		// stop the client re-detecting hardware and resetting the very graphics
		// block this change is meant to preserve, so it stays launcher-owned.
		hwDetect: 0 // Skip hardware change detection
	};

	const contents =
		Object.entries(parsed)
			.filter(v => v[1] !== undefined && v[1] !== null)
			.map(l => `SET ${l[0]} "${l[1]}"`)
			.join(eol) + eol;

	// Temp file + rename, so the config is replaced atomically instead of being
	// deleted first and rewritten afterwards.
	const tempPath = `${configPath}.launcher-tmp`;
	await fs.writeFile(tempPath, contents);
	await fs.move(tempPath, configPath, { overwrite: true });

	// Only after a successful write, so a failure here does not mark the client as
	// seeded and silently skip the defaults forever.
	if (isFirstSeed) Preferences.data = { clientInitialized: true };
	Logger.log('Config.wtf successfully patched');
};

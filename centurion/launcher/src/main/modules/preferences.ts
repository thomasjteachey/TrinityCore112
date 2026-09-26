import path from 'path';

import fs from 'fs-extra';
import { app } from 'electron';

import {
	DEFAULT_AZEROTHCORE_REALMLIST,
	DEFAULT_LAUNCHER_UPDATE_URL,
	DEFAULT_REALM_ID,
	DEFAULT_REALMLIST,
	isRetiredRealm,
	PUBLIC_REALM_IDS
} from '~common/constants';
import {
	PreferencesSchema,
	type PreferencesSchema as PreferencesData
} from '~common/schemas';
import { omit } from '~common/utils';

import { getCompatibilityRuntime } from './compatibility';

// The portable target sets PORTABLE_EXECUTABLE_DIR from its extractor stub. An
// unpacked build has no stub — needed under Proton, where the self-extractor is
// an extra failure point — so derive the same directory from the executable.
const portableDir =
	process.env.PORTABLE_EXECUTABLE_DIR ??
	(app.isPackaged ? path.dirname(app.getPath('exe')) : undefined);

const migrateLegacyDefaults = (
	json: Record<string, unknown>
): Record<string, unknown> => ({
	...json,
	realmList:
		json.realmList === '136.56.187.218' ? DEFAULT_REALMLIST : json.realmList,
	azerothcoreRealmList:
		json.azerothcoreRealmList === '136.56.187.218:3726'
			? DEFAULT_AZEROTHCORE_REALMLIST
			: json.azerothcoreRealmList,
	launcherUpdateUrl:
		json.launcherUpdateUrl === 'http://centurionpvp.com/downloads/' ||
		json.launcherUpdateUrl === 'http://136.56.187.218/downloads/'
			? DEFAULT_LAUNCHER_UPDATE_URL
			: json.launcherUpdateUrl
});

// A retired realm is gone from the realmlist, so it is replaced even in dev mode.
const enforceRealmVisibility = (prefs: PreferencesData): PreferencesData =>
	!isRetiredRealm(prefs.selectedRealm) &&
	(prefs.isDev ||
		(PUBLIC_REALM_IDS as readonly string[]).includes(prefs.selectedRealm))
		? prefs
		: { ...prefs, selectedRealm: DEFAULT_REALM_ID };

abstract class Preferences {
	static #data: PreferencesData = enforceRealmVisibility(
		PreferencesSchema.parse({
			launcherUpdateUrl: DEFAULT_LAUNCHER_UPDATE_URL,
			realmList: DEFAULT_REALMLIST,
			azerothcoreRealmList: DEFAULT_AZEROTHCORE_REALMLIST
		})
	);

	static readonly userDataDir = portableDir
		? path.join(portableDir, '.launcher')
		: app.getPath('userData');

	static async load() {
		await fs.ensureDir(this.userDataDir);
		const userDataPath = path.join(this.userDataDir, 'settings.json');
		const { isWine } = await getCompatibilityRuntime();
		try {
			const json = migrateLegacyDefaults(await fs.readJSON(userDataPath));
			return enforceRealmVisibility(
				PreferencesSchema.parse({
					...json,
					reopenLauncher: json.reopenLauncher ?? isWine,
					// A settings.json that predates this flag belongs to a player who has
					// already been running the game, so treat their client as initialised.
					// Defaulting to false would re-seed the display keys one last time and
					// force maximized mode back on once more after the update.
					clientInitialized: json.clientInitialized ?? true,
					isPortable: !!portableDir,
					clientDir: portableDir ?? json.clientDir
				})
			);
		} catch (e) {
			return enforceRealmVisibility(
				PreferencesSchema.parse({
					reopenLauncher: isWine,
					isPortable: !!portableDir,
					clientDir: portableDir
				})
			);
		}
	}

	static get data(): PreferencesData {
		return this.#data;
	}

	static set data(newData: Partial<Omit<PreferencesData, 'portableDir'>>) {
		const parsed = PreferencesSchema.parse({ ...this.#data, ...newData });
		this.#data = enforceRealmVisibility(parsed);
		void fs.writeJSON(
			path.join(this.userDataDir, 'settings.json'),
			omit(
				this.#data,
				portableDir ? ['isPortable', 'clientDir'] : ['isPortable']
			),
			{ spaces: 2 }
		);
	}

	static async isValidClientDir(clientDir?: string) {
		return !!clientDir && (await fs.exists(path.join(clientDir, 'WoW.exe')));
	}
}

export default Preferences;

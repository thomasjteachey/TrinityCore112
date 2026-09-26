import { execFile } from 'node:child_process';
import { existsSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export type CompatibilityLayer = 'none' | 'proton' | 'wine';

export type CompatibilityRuntime = {
	compatibilityLayer: CompatibilityLayer;
	isProton: boolean;
	isWine: boolean;
	launchedBySteam: boolean;
	steamAppId?: string;
	hasSteamCompatDataPath: boolean;
	hasWinePrefix: boolean;
};

const readEnvironment = (...names: string[]) => {
	for (const name of names) {
		const key = Object.keys(process.env).find(
			candidate => candidate.toLowerCase() === name.toLowerCase()
		);
		const value = key ? process.env[key] : undefined;
		if (value) return value;
	}
	return undefined;
};

const hasAnyEnvironment = (...names: string[]) =>
	names.some(name => readEnvironment(name) !== undefined);

const hasWineRegistryKey = () => {
	if (process.platform !== 'win32') return Promise.resolve(false);

	const systemRoot = readEnvironment('SystemRoot') ?? 'C:\\Windows';
	const regPath = path.win32.join(systemRoot, 'System32', 'reg.exe');

	return new Promise<boolean>(resolve => {
		execFile(
			regPath,
			['query', 'HKCU\\Software\\Wine'],
			{ timeout: 2500, windowsHide: true },
			error => resolve(!error)
		);
	});
};

// Wine ships these into every prefix and Windows never has them. Unlike the
// registry query this is synchronous, so it can run before app.whenReady()
// where the Chromium command line still has to be built.
const WINE_FILE_MARKERS = [
	'winemenubuilder.exe',
	'wineboot.exe',
	'winedbg.exe'
];

const hasWineFileMarker = () => {
	if (process.platform !== 'win32') return false;

	const systemRoot = readEnvironment('SystemRoot') ?? 'C:\\Windows';
	return WINE_FILE_MARKERS.some(marker => {
		try {
			return existsSync(path.win32.join(systemRoot, 'System32', marker));
		} catch (e) {
			return false;
		}
	});
};

// Wine maps the Unix root to Z:. Requiring a directory that only exists on a
// Unix filesystem keeps a mapped Z: network drive on real Windows from matching.
const UNIX_ROOT_MARKERS = ['Z:\\proc', 'Z:\\usr', 'Z:\\etc'];

const hasUnixRootDrive = () => {
	if (process.platform !== 'win32') return false;

	return UNIX_ROOT_MARKERS.some(marker => {
		try {
			return existsSync(marker);
		} catch (e) {
			return false;
		}
	});
};

const collectHints = () => {
	const steamAppId = readEnvironment('SteamAppId', 'SteamGameId');
	const hasSteamCompatDataPath = hasAnyEnvironment(
		'STEAM_COMPAT_DATA_PATH',
		'STEAM_COMPAT_CLIENT_INSTALL_PATH'
	);
	const hasProtonHint = hasAnyEnvironment(
		'STEAM_COMPAT_DATA_PATH',
		'STEAM_COMPAT_CLIENT_INSTALL_PATH',
		'PROTON_LOG',
		'PROTON_LOG_DIR'
	);
	const hasWinePrefix = hasAnyEnvironment(
		'WINEPREFIX',
		'WINESERVER',
		'WINELOADER',
		'WINEDLLPATH',
		'WINELOADERNOEXEC'
	);

	return { steamAppId, hasSteamCompatDataPath, hasProtonHint, hasWinePrefix };
};

const buildRuntime = (
	{
		steamAppId,
		hasSteamCompatDataPath,
		hasProtonHint,
		hasWinePrefix
	}: ReturnType<typeof collectHints>,
	wineDetected: boolean
): CompatibilityRuntime => {
	const isWine =
		process.platform === 'win32' &&
		(hasWinePrefix || hasProtonHint || wineDetected);
	// Proton hides most of its own env from the win32 process, so a Steam app id
	// inside a Wine prefix is treated as Proton even without the compat paths.
	const isProton =
		isWine && (hasProtonHint || hasSteamCompatDataPath || !!steamAppId);

	return {
		compatibilityLayer: isProton ? 'proton' : isWine ? 'wine' : 'none',
		isProton,
		isWine,
		launchedBySteam: !!steamAppId,
		steamAppId,
		hasSteamCompatDataPath,
		hasWinePrefix
	};
};

let runtimePromise: Promise<CompatibilityRuntime> | undefined;

/**
 * Synchronous detection for the pre-ready startup path. Misses only the
 * registry fallback, which the async variant fills in later.
 */
export const getCompatibilityRuntimeSync = () =>
	buildRuntime(collectHints(), hasWineFileMarker() || hasUnixRootDrive());

export const getCompatibilityRuntime = () => {
	runtimePromise ??= (async () => {
		const hints = collectHints();
		const runtime = buildRuntime(
			hints,
			hasWineFileMarker() || hasUnixRootDrive()
		);
		if (runtime.isWine) return runtime;
		return buildRuntime(hints, await hasWineRegistryKey());
	})();

	return runtimePromise;
};

export const getCompatibilityDiagnostics = async () => {
	const runtime = await getCompatibilityRuntime();
	return {
		platform: process.platform,
		architecture: process.arch,
		osRelease: os.release(),
		electronVersion: process.versions.electron,
		nodeVersion: process.versions.node,
		compatibilityLayer: runtime.compatibilityLayer,
		launchedBySteam: runtime.launchedBySteam,
		steamAppId: runtime.steamAppId,
		steamCompatDataPathDetected: runtime.hasSteamCompatDataPath,
		winePrefixDetected: runtime.hasWinePrefix,
		wineFileMarkerDetected: hasWineFileMarker(),
		unixRootDriveDetected: hasUnixRootDrive(),
		steamCompatToolPaths: readEnvironment('STEAM_COMPAT_TOOL_PATHS'),
		commandLine: process.argv.slice(1)
	};
};

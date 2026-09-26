import path from 'node:path';
import { exec } from 'node:child_process';
import type { ChildProcess } from 'node:child_process';
import nativeFs from 'node:fs';
import os from 'node:os';

import fs from 'fs-extra';
import yauzl from 'yauzl-promise';

import { getMainWindow } from '~main/index';
import { formatDuration, formatFileSize } from '~common/utils';
import {
	DEFAULT_LAUNCHER_UPDATE_URL,
	DEFAULT_REALM_ID,
	FileMap,
	type RealmId
} from '~common/constants';

import Logger from './logger';
import Preferences from './preferences';
import Observable from './observable';
import resumableFetch, { type FetchProgress } from './resumableFetch';
import { getCompatibilityRuntime } from './compatibility';
import updateFetch from './updateFetch';
import { migrateRenamedPatch, removePatchFiles } from './patchFiles';

const resolveBaseUrl = () => {
        const { launcherUpdateUrl } = Preferences.data;
        return launcherUpdateUrl ?? DEFAULT_LAUNCHER_UPDATE_URL;
};

const resolvePatchUrl = (filePath: string) =>
        new URL(`patches/${filePath.replace(/^\/+/, '')}`, resolveBaseUrl()).toString();

/**
 * Remote basename for a patch: the live one (its remoteFile, else its key), or
 * its dev counterpart when the launcher is in dev mode and the patch has one.
 *
 * The .version file follows the same name, so the update cache holds a
 * different value in each mode - toggling dev on or off therefore re-downloads
 * that patch by itself, instead of leaving the other build's archive in place.
 */
const remoteName = (name: string) => {
        const meta = FileMap[name];
        return Preferences.data.isDev && meta?.devFile
                ? meta.devFile
                : meta?.remoteFile ?? name;
};

// const isReadOnly = async (filePath: string) => {
// 	try {
// 		const { mode } = await fs.stat(filePath);
// 		return !(mode & fs.constants.S_IWUSR);
// 	} catch (e) {
// 		return false;
// 	}
// };

const execAsync = (commands: Partial<Record<NodeJS.Platform, string>>) => {
	const command = commands[os.platform()];
	if (!command) return Promise.resolve(undefined);
	return new Promise<string | undefined>(resolve => {
		exec(command, (error, stdout) => {
			if (error) resolve(undefined);
			else resolve(stdout);
		});
	});
};

type StatFsResult = {
	bavail: number | bigint;
	bsize: number | bigint;
};

type StatFs = (
	filePath: string,
	callback: (error: NodeJS.ErrnoException | null, result: StatFsResult) => void
) => void;

const statFs = (nativeFs as unknown as { statfs?: StatFs }).statfs;

const getAvailableDiskSpace = async (clientPath?: string) => {
	if (clientPath && statFs) {
		const available = await new Promise<number | undefined>(resolve => {
			statFs(clientPath, (error, result) => {
				if (error) {
					resolve(undefined);
					return;
				}

				resolve(Number(result.bavail) * Number(result.bsize));
			});
		});
		if (available !== undefined && Number.isFinite(available)) return available;
	}

	const response = await execAsync({
		win32:
			'%SYSTEMROOT%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe -command "Get-PSDrive -PSProvider FileSystem | Select-Object Name, @{n=\'Free\';e={($_.Free / 1GB)}}"'
	});
	if (!response) return Infinity;
	const drive = clientPath?.split(':')[0] ?? 'C';
	const space = parseFloat(
		response
			.split('\n')
			.find(l => l.trim().startsWith(drive))
			?.split(/\s+/)?.[1] ?? '0'
	);
	return space * 1024 ** 3;
};

let trackedGameProcess: ChildProcess | undefined;

export const trackGameProcess = (gameProcess: ChildProcess) => {
	trackedGameProcess = gameProcess;
	const clearTrackedProcess = () => {
		if (trackedGameProcess === gameProcess) trackedGameProcess = undefined;
	};
	gameProcess.once('exit', clearTrackedProcess);
	gameProcess.once('error', clearTrackedProcess);
};

// Wine ships no tasklist.exe, so the process list is unavailable and the guard
// below would report "not running" for a client the launcher did not start
// itself - after a launcher restart, for instance. A running client keeps its
// executable open, so a refused write-open is a reliable stand-in.
const isClientExecutableLocked = async (clientDir?: string) => {
	if (!clientDir) return false;

	let handle: number | undefined;
	try {
		handle = await fs.open(path.join(clientDir, 'WoW.exe'), 'r+');
		return false;
	} catch (error) {
		// Only a sharing violation counts. EPERM/EACCES mean a permissions or
		// read-only-attribute problem, and a missing file means no client at all -
		// none of those are evidence the game is running, and treating them as such
		// would block launching outright.
		return (error as NodeJS.ErrnoException).code === 'EBUSY';
	} finally {
		if (handle !== undefined) await fs.close(handle).catch(() => undefined);
	}
};

export const isGameRunning = async () => {
	if (
		trackedGameProcess &&
		trackedGameProcess.exitCode === null &&
		trackedGameProcess.signalCode === null
	)
		return true;

	const response = await execAsync({
		win32: '%SYSTEMROOT%\\System32\\tasklist.exe'
	});
	if (!response) {
		const { isWine } = await getCompatibilityRuntime();
		if (isWine) {
			void Logger.log(
				'Unable to query the Proton/Wine process list; falling back to a lock check on WoW.exe.',
				'warning'
			);
			return isClientExecutableLocked(Preferences.data.clientDir);
		}
		return false;
	}
	return response.toLowerCase().includes('wow.exe');
};

const fetchFile = async (
	filePath: string,
	progressCb?: (p: FetchProgress) => void,
	force: boolean = false
) => {
	try {
		const downloadPath = path.join(
			Preferences.userDataDir,
			'downloads',
			filePath
		);
		// The parent of the FILE, not just the downloads folder: a remote path
		// can contain a subdirectory (dev patches live under itemforge/), and
		// resumableFetch opens a write stream without creating anything.
		await fs.ensureDir(path.dirname(downloadPath));
                await resumableFetch(
                        resolvePatchUrl(filePath),
                        downloadPath,
                        progressCb,
                        {
				throttle: 500,
				force
			}
		);
		return downloadPath;
	} catch (e) {
		Logger.log(`Failed to download ${filePath}`, 'error', e);
		throw Error(`Failed to download ${filePath}`);
	}
};

/**
 * Metadata requests are a few bytes, so an absolute deadline is safe here in a
 * way it is not for a patch download (see updateFetch). Generous enough to
 * survive a cold DNS lookup on a machine that just woke its Wi-Fi up.
 */
const METADATA_TIMEOUT = 15000;

/**
 * A short, player-readable reason. The UI shows the thrown message in a single
 * narrow line, and "Failed to download X" on its own sent people hunting for
 * missing files on the server when the connection was simply dropped.
 */
const describeFetchFailure = (e: unknown) => {
	if (!(e instanceof Error)) return 'unknown error';
	const { code } = e as NodeJS.ErrnoException;
	if (code === 'ENOTFOUND' || code === 'EAI_AGAIN')
		return 'cannot reach the update server';
	if (code === 'ECONNREFUSED') return 'connection refused';
	if (code === 'ECONNRESET' || code === 'EPIPE') return 'connection dropped';
	if (code === 'ETIMEDOUT' || /timeout/i.test(e.message)) return 'timed out';
	if (code?.startsWith('CERT_') || code?.startsWith('UNABLE_TO_'))
		return 'TLS certificate rejected';
	return e.message;
};

const fetchSize = async (filePath: string) => {
	try {
		const response = await updateFetch(resolvePatchUrl(filePath), {
			method: 'HEAD',
			timeout: METADATA_TIMEOUT
		});

		if (!response.ok) {
			throw new Error(
				`Failed to fetch metadata for ${filePath} (${response.status} ${response.statusText})`
			);
		}

		return parseInt(response.headers.get('content-length') ?? '0');
	} catch (e) {
		Logger.log(`Failed to download ${filePath}`, 'error', e);
		throw Error(`Failed to download ${filePath} - ${describeFetchFailure(e)}`);
	}
};

const fetchVersion = async (filePath: string) => {
	try {
		const response = await updateFetch(resolvePatchUrl(filePath), {
			timeout: METADATA_TIMEOUT
		});

		if (!response.ok) {
			throw new Error(
				`Failed to fetch metadata for ${filePath} (${response.status} ${response.statusText})`
			);
		}

		const version = (await response.text()).trim();

		// An intercepting proxy, captive portal or AV web shield answers with an
		// HTML page instead of the version string; every such page trips this.
		if (/[/\\<>:"|?*]/.test(version)) {
			throw new Error(`Invalid version received for ${filePath}`);
		}

		return version;
	} catch (e) {
		Logger.log(`Failed to download ${filePath}`, 'error', e);
		throw Error(`Failed to download ${filePath} - ${describeFetchFailure(e)}`);
	}
};

/**
 * Returns true if the error is the yauzl-promise assertion for a corrupt
 * Local File Header:
 *   "Invalid Local File Header signature"
 */
const isCorruptZipLocalHeaderError = (err: unknown): boolean => {
        if (!(err instanceof Error)) return false;
        return /Invalid Local File Header signature/i.test(err.message);
};

type ExtractProgressCallback = (progress: number | null, percent?: number) => void;

const extractArchive = async (
        name: string,
        file: string,
        filePath: string,
        progressCb?: ExtractProgressCallback,
        extractAs?: Record<string, string>
) => {
        let finished = false;
        const archive = await yauzl.open(file);
        const extractedFiles: string[] = [];
        try {
                for await (const entry of archive) {
                        // Written straight to the new name, never to the archive's own:
                        // the art base's entry is patch-Y.MPQ, and extracting it under
                        // that name would overwrite World Terrain's archive.
                        const localName = extractAs?.[entry.filename] ?? entry.filename;
                        Logger.log(
                                localName === entry.filename
                                        ? `Extracting "${entry.filename}"...`
                                        : `Extracting "${entry.filename}" as "${localName}"...`
                        );
                        if (entry.filename.endsWith('/')) {
                                await fs.ensureDir(path.join(filePath, entry.filename));
                        } else {
                                const dest = path.join(filePath, localName);
                                await fs.ensureDir(path.dirname(dest));
                                const readStream = await entry.openReadStream();
                                const writeStream = fs.createWriteStream(dest);
                                const totalBytes = entry.uncompressedSize;
                                const hasKnownSize = Number.isFinite(totalBytes) && totalBytes > 0;

                                if (!hasKnownSize) {
                                        progressCb?.(null);
                                } else {
                                        progressCb?.(0, 0);
                                }

                                await new Promise((resolve, reject) => {
                                        let extractedBytes = 0;

                                        readStream.on('data', (chunk: Buffer) => {
                                                if (!hasKnownSize) return;

                                                extractedBytes += chunk.length;
                                                const progress = Math.min(
                                                        extractedBytes / (totalBytes ?? 1),
                                                        1
                                                );
                                                const percent = Math.round(progress * 100);
                                                progressCb?.(progress, percent);
                                        });

                                        readStream.on('error', reject);
                                        writeStream.on('error', reject);
                                        writeStream.on('finish', () => {
                                                if (hasKnownSize) {
                                                        progressCb?.(1, 100);
                                                }
                                                resolve(undefined);
                                        });

                                        readStream.pipe(writeStream);
                                });

                                extractedFiles.push(localName);
                        }
                }
                finished = true;
        } finally {
                await archive.close();
                if (finished) {
                        Logger.log(`Removing "${file}"...`);
                        await fs.remove(file);
                }
        }
        return extractedFiles;
};
/**
 * Wrapper around extractArchive that retries once when the ZIP file
 * is clearly corrupt (Invalid Local File Header signature).
 *
 * @param name      Patch name (e.g. "HDTexturesLegionnaire")
 * @param file      Full path to the downloaded .zip on disk
 * @param filePath  Destination root on disk
 * @param progressCb Extraction progress callback (same shape as extractArchive)
 * @param extractAs Archive entries to write under a different local name
 */
const extractArchiveWithRetry = async (
        name: string,
        file: string,
        filePath: string,
        progressCb?: ExtractProgressCallback,
        extractAs?: Record<string, string>
): Promise<string[]> => {
        try {
                return await extractArchive(name, file, filePath, progressCb, extractAs);
        } catch (err) {
                if (!isCorruptZipLocalHeaderError(err)) {
                        throw err;
                }

                Logger.log(
                        `Corrupt patch archive detected for "${name}" at "${file}". ` +
                                `Forcing re-download and retrying once.`,
                        'error',
                        err
                );

                const patchFileName = path.basename(file);
                const freshFile = await fetchFile(patchFileName, undefined, true);

                return await extractArchive(name, freshFile, filePath, progressCb, extractAs);
        }
};


type UpdaterState =
        | 'needsValidation'
        | 'verifying'
        | 'serverUnreachable'
        | 'noClient'
        | 'updateAvailable'
        | 'updating'
        | 'upToDate'
        | 'failed';

export type UpdaterStatus = {
	state: UpdaterState;
	progress?: number;
	message?: string;
};

class UpdaterClass extends Observable<UpdaterStatus> {
        #versionCache: Record<string, string> = {};
        #fileCache: Record<string, string[]> = {};
        #pendingInvalidations = new Set<string>();

        #cachePatchFiles = async (
                clientDir: string,
                name: string,
                version: string,
                extractPath: string,
                files: string[]
        ) => {
                if (files.length === 0) return;

                const cacheRoot = path.join(clientDir, '.launcher', 'cached', name);
                const cachePath = path.join(cacheRoot, version);

                try {
                        await fs.ensureDir(path.join(clientDir, '.launcher', 'cached'));
                        await fs.remove(cacheRoot);
                        await fs.ensureDir(cachePath);

                        for (const file of files) {
                                const source = path.join(clientDir, extractPath, file);
                                if (!(await fs.pathExists(source))) continue;

                                const destination = path.join(cachePath, file);
                                await fs.ensureDir(path.dirname(destination));
                                await fs.copy(source, destination, { overwrite: true });
                        }
                } catch (e) {
                        Logger.log(`Failed to populate cache for ${name}@${version}`, 'error', e);
                }
        };

	#loadCache = async (clientDir: string) => {
		await fs.ensureDir(path.join(clientDir, '.launcher'));

                const versionCache = path.join(clientDir, '.launcher', 'update-cache.json');
                this.#versionCache = (await fs.exists(versionCache))
                        ? await fs.readJSON(versionCache)
                        : {};
                this.#pendingInvalidations.clear();

		const fileCache = path.join(clientDir, '.launcher', 'file-cache.json');
		this.#fileCache = (await fs.exists(fileCache))
			? await fs.readJSON(fileCache)
			: {};
	};

	// Runs before anything is verified, so the renamed files are found in place
	// under the new key. The cache is saved straight away: if verification
	// failed later, a stale cache would still point at the old name, and the
	// next run would download the whole archive again.
	#migrateRenamedPatches = async (clientDir: string) => {
		for (const [name, meta] of Object.entries(FileMap)) {
			if (!meta.migrateFrom) continue;
			try {
				const renamed = await migrateRenamedPatch(clientDir, name, meta, {
					versions: this.#versionCache,
					files: this.#fileCache
				});
				if (renamed.length === 0) continue;
				Logger.log(
					`Moved ${meta.migrateFrom} to ${name} in place: ${renamed.join(', ')}`
				);
				await this.#saveCache(clientDir);
			} catch (e) {
				Logger.log(`Failed to move ${meta.migrateFrom} to ${name}`, 'error', e);
			}
		}
	};

	#saveCache = async (clientDir: string) => {
		await fs.ensureDir(path.join(clientDir, '.launcher'));

		const versionCache = path.join(clientDir, '.launcher', 'update-cache.json');
		await fs.writeJSON(versionCache, this.#versionCache, { spaces: 2 });

		const fileCache = path.join(clientDir, '.launcher', 'file-cache.json');
		await fs.writeJSON(fileCache, this.#fileCache, { spaces: 2 });
	};

	protected _value: UpdaterStatus = { state: 'needsValidation' };

	get status() {
		return this._value;
	}

	private set status(v: UpdaterStatus) {
		this._value = v;
		this._notifyObservers(v);
		const window = getMainWindow();
		if (!window) return;

		if (this.status.state === 'failed') {
			window.setProgressBar(1, { mode: 'error' });
		} else if (this.status.progress === 1) {
			window.setProgressBar(0);
		} else {
			window.setProgressBar(this.status.progress ?? 0, {
				mode: this.status.progress === -1 ? 'indeterminate' : 'normal'
			});
		}
	}

	invalidate() {
		this.status = { state: 'needsValidation' };
	}

	async verify() {
		const { clientDir, optionalPatches, selectedRealm, isPortable } =
		Preferences.data;
		const realmKey = selectedRealm ?? DEFAULT_REALM_ID;
		try {
			if (
				this.status?.state === 'verifying' ||
				this.status?.state === 'updating'
			)
				return;

			if (!clientDir || !(await Preferences.isValidClientDir(clientDir))) {
				this.status = { state: 'noClient' };
				return;
			}

                        if (isPortable) {
                                await fs.remove(path.join(clientDir, 'update-script.bat'));
                        }

			if (os.platform() === 'win32' && clientDir.length > 220) {
				this.status = {
					state: 'failed',
					message:
						'Path to current install location is too long and may cause issues.'
				};
				return;
			}

			if (await isGameRunning()) {
				this.status = {
					state: 'failed',
					message: 'Please close WoW first, before updating.'
				};
				return;
			}

			Logger.log(`Verifying client files at ${path.join(clientDir)}...`);
                        this.status = {
                                state: 'verifying',
                                progress: 0,
                                message: 'Looking for updates...'
                        };

                        await this.#loadCache(clientDir);
                        await this.#migrateRenamedPatches(clientDir);
                        let toDownload = 0;

                        const verificationEntries = Object.entries(FileMap);
                        const totalVerificationEntries = verificationEntries.length;
                        let processedVerificationEntries = 0;

                        for (const [name, meta] of verificationEntries) {
                                processedVerificationEntries += 1;
                                const verificationProgress =
                                        totalVerificationEntries === 0
                                                ? -1
                                                : processedVerificationEntries / totalVerificationEntries;
                                const displayName = meta.label ?? name;
                                this.status = {
                                        state: 'verifying',
                                        progress: verificationProgress,
                                        message: `Looking for updates... (${displayName})`
                                };

                                const shouldCache = !!meta.optional || !!meta.realms;
                                const cachedVersion = this.#versionCache[name];

                                let version = cachedVersion;
                                const getCachePath = () =>
                                        shouldCache && version
                                                ? path.join(
                                                          clientDir,
                                                          '.launcher',
                                                          'cached',
                                                          name,
                                                          version
                                                  )
                                                : undefined;
                                let cachePath = getCachePath();

                                const isOptionalEnabled =
                                        !meta.optional || optionalPatches.includes(name);
                                const isRealmEnabled =
                                        !meta.realms || meta.realms.includes(realmKey);
                                const shouldUse = isOptionalEnabled && isRealmEnabled;

                                if (!shouldUse) {
                                        if (shouldCache && cachePath) {
                                                // Move files to cache without nuking an existing copy when there's
                                                // nothing new to move. This prevents losing cached data for realms
                                                // that remain disabled across multiple verification runs.
                                                const filesToMove = await Promise.all(
                                                        (this.#fileCache[name] ?? []).map(async file => {
                                                                const source = path.join(
                                                                        clientDir,
                                                                        meta.extractPath,
                                                                        file
                                                                );
                                                                return {
                                                                        file,
                                                                        source,
                                                                        exists: await fs.pathExists(source)
                                                                };
                                                        })
                                                );

                                                const existingSources = filesToMove.filter(({ exists }) => exists);

                                                        if (existingSources.length !== 0) {
                                                                await fs.ensureDir(path.join(clientDir, '.launcher', 'cached'));
                                                                await fs.remove(cachePath);
                                                                await fs.ensureDir(cachePath);

                                                                for (const { file, source } of existingSources) {
                                                                        try {
                                                                                const destination = path.join(cachePath, file);
                                                                                await fs.ensureDir(path.dirname(destination));
                                                                                await fs.move(source, destination, {
                                                                                        overwrite: true
                                                                        });
                                                                } catch (_error) {
                                                                        console.error(_error);
                                                                }
                                                        }
                                                }
                                        }
                                        if (meta.removeWhenUnused) {
                                                try {
                                                        const removed = await removePatchFiles(
                                                                clientDir,
                                                                meta.extractPath,
                                                                meta.removeWhenUnused
                                                        );
                                                        if (removed.length !== 0) {
                                                                Logger.log(
                                                                        `Removed ${removed.join(', ')} because ${name} is not in use.`
                                                                );
                                                        }
                                                } catch (e) {
                                                        Logger.log(`Failed to remove unused ${name} files`, 'error', e);
                                                }
                                        }
                                        this.#pendingInvalidations.delete(name);
                                        continue;
                                }

                                version = await fetchVersion(`${remoteName(name)}.version`);
                                cachePath = getCachePath();

                                let expectedFiles = this.#fileCache[name];
                                if (!expectedFiles) {
                                        expectedFiles = [];
                                        this.#fileCache[name] = expectedFiles;
                                }

                                const hasAllFiles = async () => {
                                        if (expectedFiles.length === 0) {
                                                try {
                                                        const entries = await fs.readdir(
                                                                path.join(clientDir, meta.extractPath)
                                                        );
                                                        const inferredFiles = entries.filter(entry =>
                                                                entry
                                                                        .toLowerCase()
                                                                        .startsWith(name.toLowerCase())
                                                        );

                                                        if (inferredFiles.length !== 0) {
                                                                expectedFiles.splice(
                                                                        0,
                                                                        expectedFiles.length,
                                                                        ...inferredFiles
                                                                );
                                                                this.#fileCache[name] = expectedFiles;
                                                        }
                                                } catch (_error) {
                                                        // If the directory doesn't exist yet we fall back to the
                                                        // standard download path below.
                                                }

                                                if (expectedFiles.length === 0) {
                                                        return false;
                                                }
                                        }
                                        for (const file of expectedFiles) {
                                                const destination = path.join(
                                                        clientDir,
                                                        meta.extractPath,
                                                        file
                                                );
                                                if (!(await fs.pathExists(destination))) {
                                                        return false;
                                                }
                                        }
                                        return true;
                                };

                                const restoreFromCache = async () => {
                                        if (!shouldCache || !cachePath || !(await fs.exists(cachePath))) {
                                                return false;
                                        }

                                        const collectCacheFiles = async (dir: string) => {
                                                const entries = await fs.readdir(dir);
                                                const files: string[] = [];
                                                for (const entry of entries) {
                                                        const entryPath = path.join(dir, entry);
                                                        const stats = await fs.stat(entryPath);
                                                        if (stats.isDirectory()) {
                                                                files.push(
                                                                        ...(await collectCacheFiles(entryPath))
                                                                );
                                                        } else {
                                                                files.push(
                                                                        path
                                                                                .relative(cachePath, entryPath)
                                                                                .replace(/\\/g, '/')
                                                                );
                                                        }
                                                }
                                                return files;
                                        };

                                        const filesToRestore =
                                                expectedFiles.length !== 0
                                                        ? expectedFiles
                                                        : await collectCacheFiles(cachePath);

                                        if (filesToRestore.length === 0) return false;

                                        let movedAny = false;
                                        for (const file of filesToRestore) {
                                                const source = path.join(cachePath, file);
                                                if (!(await fs.pathExists(source))) continue;
                                                const destination = path.join(
                                                        clientDir,
                                                        meta.extractPath,
                                                        file
                                                );
                                                try {
                                                        await fs.ensureDir(path.dirname(destination));
                                                        await fs.copy(source, destination, { overwrite: true });
                                                        movedAny = true;
                                                } catch (e) {
                                                        console.error(e);
                                                }
                                        }

                                        if (!movedAny) return false;

                                        const uniqueFiles = Array.from(
                                                new Set([...expectedFiles, ...filesToRestore])
                                        );
                                        expectedFiles.splice(0, expectedFiles.length, ...uniqueFiles);
                                        this.#fileCache[name] = expectedFiles;

                                        if (!(await hasAllFiles())) {
                                                return false;
                                        }

                                        this.#versionCache[name] = version;
                                        this.#pendingInvalidations.delete(name);
                                        return true;
                                };

                                let needsDownload = false;
                                let downloadReason: 'missing' | 'update' | 'metadata' = 'update';

                                if (cachedVersion === version) {
                                        if (await hasAllFiles()) {
                                                this.#pendingInvalidations.delete(name);
                                                continue;
                                        }

                                        if (await restoreFromCache()) {
                                                this.#pendingInvalidations.delete(name);
                                                continue;
                                        }

                                        Logger.log(
                                                `Missing files detected for ${name}. Scheduling download to restore them.`
                                        );
                                        this.#pendingInvalidations.add(name);
                                        needsDownload = true;
                                        downloadReason = 'missing';
                                } else if (!cachedVersion) {
                                        Logger.log(
                                                `No version metadata found for ${name}. Scheduling download to rebuild the cache.`
                                        );
                                        this.#pendingInvalidations.add(name);
                                        needsDownload = true;
                                        downloadReason = 'metadata';
                                } else {
                                        if (await restoreFromCache()) {
                                                this.#pendingInvalidations.delete(name);
                                                continue;
                                        }
                                        this.#pendingInvalidations.add(name);
                                        needsDownload = true;
                                        downloadReason = 'update';
                                }

                                if (!needsDownload) continue;

                                if (shouldCache && cachePath) {
                                        await fs.remove(path.dirname(cachePath));
                                }

                                if (downloadReason === 'update') {
                                        Logger.log(`New ${name} version available: ${version}`);
                                } else if (downloadReason === 'missing') {
                                        Logger.log(`Re-downloading ${name} files to replace missing data.`);
                                } else {
                                        Logger.log(
                                                `Re-downloading ${name} to restore missing version metadata in the cache.`
                                        );
                                }

                                toDownload += await fetchSize(`${remoteName(name)}.zip`);
                        }

			await this.#saveCache(clientDir);

			if (toDownload !== 0) {
				const availableSpace = await getAvailableDiskSpace(clientDir);
				if (toDownload > availableSpace) {
					this.status = {
						state: 'failed',
						message: `Not enough disk space. Required: ${formatFileSize(
							toDownload
						)}, Available: ${formatFileSize(availableSpace)}`
					};
					return;
				}
			}

			this.status =
				toDownload !== 0
					? { state: 'updateAvailable', message: formatFileSize(toDownload) }
					: { state: 'upToDate', progress: 1 };
		} catch (e) {
			const message =
				e instanceof Error ? e.message : 'Unexpected error occurred';
			Logger.log(`Verification failed: ${message}`, 'error', e);
			this.status = { state: 'failed', message };
		}
	}

        async update(force?: boolean) {
                const { clientDir, optionalPatches, selectedRealm } = Preferences.data;
		const realmKey = selectedRealm ?? DEFAULT_REALM_ID;
		try {
			if (
				this.status?.state === 'verifying' ||
				this.status?.state === 'updating'
			)
				return;

			if (!clientDir || !(await Preferences.isValidClientDir(clientDir))) {
				this.status = { state: 'noClient' };
				return;
			}

			if (await isGameRunning()) {
				this.status = {
					state: 'failed',
					message: 'Please close WoW first, before updating.'
				};
				return;
			}

			const downloadsDir = path.join(Preferences.userDataDir, 'downloads');
			Logger.log(`Removing stale downloads from "${downloadsDir}"...`);
			await fs.remove(downloadsDir);

			Logger.log(`Updating client files at ${path.join(clientDir)}...`);
			this.status = {
				state: 'updating',
				progress: -1,
				message: 'Preparing files...'
			};

                        for (const [name, meta] of Object.entries(FileMap)) {
                                const isOptionalEnabled =
                                        !meta.optional || optionalPatches.includes(name);
                                const isRealmEnabled =
                                        !meta.realms || meta.realms.includes(realmKey);
                                const shouldUse = isOptionalEnabled && isRealmEnabled;
                                const shouldCache = !!meta.optional || !!meta.realms;

                                if (!shouldUse) {
                                        if (force) {
                                                delete this.#versionCache[name];
                                        }
                                        this.#pendingInvalidations.delete(name);
                                        continue;
                                }

                                if (this.#versionCache[name] && !force && !this.#pendingInvalidations.has(name))
                                        continue;

				Logger.log(`Downloading ${name} files...`);
				const file = await fetchFile(`${remoteName(name)}.zip`, (p: FetchProgress) => {
					const progress =
						(p.done + p.initialPartial) / (p.total + p.initialPartial);
					const percent = Math.round(progress * 100);
					const elapsed = (Date.now() - p.startedAt) / 1000;
					const rate = p.done / elapsed;
					const eta = formatDuration(p.total / rate - elapsed);
					this.status = {
						state: 'updating',
						progress,
						message: `Downloading ${name}... ${percent}% (${eta} remaining)`
					};
				});

				this.status = {
					state: 'updating',
					progress: -1,
					message: `Extracting ${name}...`
				};
                                const extractedFiles = await extractArchiveWithRetry(
                                        name,
                                        file,
                                        path.join(clientDir, meta.extractPath),
                                        (progress, percent) => {
                                                if (progress === null) {
                                                        this.status = {
                                                                state: 'updating',
                                                                progress: -1,
                                                                message: `Extracting ${name}...`
                                                        };
                                                        return;
                                                }

                                                this.status = {
                                                        state: 'updating',
                                                        progress,
                                                        message: `Extracting ${name}... ${percent}%`
                                                };
                                        },
                                        meta.extractAs
                                );

                                this.#fileCache[name] = extractedFiles;

                                const version = await fetchVersion(`${remoteName(name)}.version`);
                                this.#versionCache[name] = version;
                                this.#pendingInvalidations.delete(name);

                                if (shouldCache) {
                                        await this.#cachePatchFiles(
                                                clientDir,
                                                name,
                                                version,
                                                meta.extractPath,
                                                extractedFiles
                                        );
                                }

                                await this.#saveCache(clientDir);
			}

			this.status = { state: 'upToDate', progress: 1 };
		} catch (e) {
			const message =
				e instanceof Error ? e.message : 'Unexpected error occurred';
			Logger.log(`Update failed: ${message}`, 'error', e);
                        this.status = { state: 'failed', message };
                }
        }

        async ensureRealmPatchesFor(realmKey: RealmId) {
                const { clientDir } = Preferences.data;
                if (!clientDir) return;

                await this.#loadCache(clientDir);

                let cacheModified = false;

                for (const [name, meta] of Object.entries(FileMap)) {
                        if (!meta.realms) continue;

                        const shouldUse = meta.realms.includes(realmKey);
                        if (!shouldUse) continue;

                        const shouldCache = !!meta.optional || !!meta.realms;
                        const cacheRoot = path.join(clientDir, '.launcher', 'cached', name);
                        const cachedVersion = this.#versionCache[name];
                        const cachePath =
                                shouldCache && cachedVersion
                                        ? path.join(cacheRoot, cachedVersion)
                                        : undefined;

                        let expectedFiles = this.#fileCache[name];
                        if (!expectedFiles) {
                                expectedFiles = [];
                                this.#fileCache[name] = expectedFiles;
                                cacheModified = true;
                        }

                        const hasAllFiles = async () => {
                                if (expectedFiles.length === 0) {
                                        try {
                                                const entries = await fs.readdir(
                                                        path.join(clientDir, meta.extractPath)
                                                );
                                                const inferredFiles = entries.filter(entry =>
                                                        entry.toLowerCase().startsWith(name.toLowerCase())
                                                );

                                                if (inferredFiles.length !== 0) {
                                                        expectedFiles.splice(
                                                                0,
                                                                expectedFiles.length,
                                                                ...inferredFiles
                                                        );
                                                        this.#fileCache[name] = expectedFiles;
                                                        cacheModified = true;
                                                }
                                        } catch (_error) {
                                                // ignore
                                        }

                                        if (expectedFiles.length === 0) {
                                                return false;
                                        }
                                }

                                for (const file of expectedFiles) {
                                        const destination = path.join(
                                                clientDir,
                                                meta.extractPath,
                                                file
                                        );
                                        if (!(await fs.pathExists(destination))) {
                                                return false;
                                        }
                                }
                                return true;
                        };

                        const restoreFromCache = async () => {
                                if (!shouldCache || !cachePath || !(await fs.exists(cachePath))) {
                                        return false;
                                }

                                const collectCacheFiles = async (dir: string) => {
                                        const entries = await fs.readdir(dir);
                                        const files: string[] = [];
                                        for (const entry of entries) {
                                                const entryPath = path.join(dir, entry);
                                                const stats = await fs.stat(entryPath);
                                                if (stats.isDirectory()) {
                                                        files.push(...(await collectCacheFiles(entryPath)));
                                                } else {
                                                        files.push(
                                                                path
                                                                        .relative(cachePath, entryPath)
                                                                        .replace(/\\/g, '/')
                                                        );
                                                }
                                        }
                                        return files;
                                };

                                const filesToRestore =
                                        expectedFiles.length !== 0
                                                ? expectedFiles
                                                : await collectCacheFiles(cachePath);

                                if (filesToRestore.length === 0) return false;

                                let movedAny = false;
                                for (const file of filesToRestore) {
                                        const source = path.join(cachePath, file);
                                        if (!(await fs.pathExists(source))) continue;

                                        const destination = path.join(
                                                clientDir,
                                                meta.extractPath,
                                                file
                                        );

                                        try {
                                                await fs.ensureDir(path.dirname(destination));
                                                await fs.copy(source, destination, { overwrite: true });
                                                movedAny = true;
                                        } catch (error) {
                                                Logger.log(
                                                        `Failed to restore ${file} from cache for ${name}`,
                                                        'error',
                                                        error
                                                );
                                        }
                                }

                                if (!movedAny) return false;

                                const uniqueFiles = Array.from(
                                        new Set([...expectedFiles, ...filesToRestore])
                                );
                                expectedFiles.splice(0, expectedFiles.length, ...uniqueFiles);
                                this.#fileCache[name] = expectedFiles;
                                cacheModified = true;

                                if (!(await hasAllFiles())) {
                                        return false;
                                }

                                if (!this.#versionCache[name] && cachedVersion) {
                                        this.#versionCache[name] = cachedVersion;
                                        cacheModified = true;
                                }

                                return true;
                        };

                        if (await hasAllFiles()) {
                                continue;
                        }

                        if (await restoreFromCache()) {
                                continue;
                        }

                        Logger.log(
                                `Missing ${name} files for ${realmKey}. Downloading to restore realm patches...`
                        );
                        const file = await fetchFile(`${remoteName(name)}.zip`);
                        const extractedFiles = await extractArchiveWithRetry(
                                name,
                                file,
                                path.join(clientDir, meta.extractPath),
                                undefined,
                                meta.extractAs
                        );

                        this.#fileCache[name] = extractedFiles;

                        const version = await fetchVersion(`${remoteName(name)}.version`);
                        this.#versionCache[name] = version;

                        if (shouldCache) {
                                await this.#cachePatchFiles(
                                        clientDir,
                                        name,
                                        version,
                                        meta.extractPath,
                                        extractedFiles
                                );
                        }

                        cacheModified = true;
                }

                if (cacheModified) {
                        await this.#saveCache(clientDir);
                }
        }
}

const Updater = new UpdaterClass();
export default Updater;

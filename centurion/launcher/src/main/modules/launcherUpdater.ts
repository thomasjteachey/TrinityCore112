import path from 'node:path';
import { spawn } from 'node:child_process';
import { pipeline } from 'node:stream/promises';

import { app } from 'electron';
import fs from 'fs-extra';
import yauzl from 'yauzl-promise';

import Logger from './logger';
import Observable from './observable';
import Preferences from './preferences';
import resumableFetch, { type FetchProgress } from './resumableFetch';
import { getCompatibilityRuntime } from './compatibility';
import updateFetch from './updateFetch';

const LAUNCHER_VERSION_FILE = 'centurionlauncher.version';
const LAUNCHER_ARCHIVE_FILE = 'centurionlauncher.zip';
const LAUNCHER_EXECUTABLE_FILE = 'CenturionLauncher.exe';

const resolveLauncherUrl = (fileName: string) => {
	const url = new URL(fileName, Preferences.data.launcherUpdateUrl);
	if (app.isPackaged && url.protocol !== 'https:') {
		throw new Error('Launcher updates require an HTTPS update server.');
	}
	return url.toString();
};

const compareVersions = (leftVersion: string, rightVersion: string) => {
	const parse = (version: string) =>
		version
			.split(/[^0-9]+/)
			.filter(Boolean)
			.map(value => Number.parseInt(value, 10));
	const left = parse(leftVersion);
	const right = parse(rightVersion);
	const length = Math.max(left.length, right.length);

	for (let index = 0; index < length; index += 1) {
		const difference = (left[index] ?? 0) - (right[index] ?? 0);
		if (difference !== 0) return Math.sign(difference);
	}
	return 0;
};

const escapeForPowerShell = (value: string) => value.replace(/'/g, "''");

const extractLauncher = async (archivePath: string, destination: string) => {
	const archive = await yauzl.open(archivePath);
	let launcherPath: string | undefined;

	try {
		for await (const entry of archive) {
			const entryName = entry.filename.replace(/\\/g, '/');
			if (
				path.basename(entryName).toLowerCase() !==
				LAUNCHER_EXECUTABLE_FILE.toLowerCase()
			)
				continue;
			if (launcherPath) {
				throw new Error('Launcher archive contains duplicate executables.');
			}

			launcherPath = path.join(destination, LAUNCHER_EXECUTABLE_FILE);
			await fs.ensureDir(destination);
			const readStream = await entry.openReadStream();
			await pipeline(readStream, fs.createWriteStream(launcherPath));
		}
	} finally {
		await archive.close();
	}

	if (!launcherPath) {
		throw new Error(
			`Launcher archive does not contain ${LAUNCHER_EXECUTABLE_FILE}.`
		);
	}
	return launcherPath;
};

export type LauncherUpdaterStatus = {
	state:
		| 'idle'
		| 'checking'
		| 'downloading'
		| 'ready'
		| 'manualUpdate'
		| 'installing'
		| 'error';
	progress?: number;
	message?: string;
	version?: string;
	downloadUrl?: string;
};

class LauncherUpdaterClass extends Observable<LauncherUpdaterStatus> {
	protected _value: LauncherUpdaterStatus = { state: 'idle' };
	#downloadedLauncherPath?: string;
	#downloadedLauncherVersion?: string;

	async getInstalledVersion() {
		const versionPath = path.join(
			Preferences.userDataDir,
			LAUNCHER_VERSION_FILE
		);
		const storedVersion = await fs
			.readFile(versionPath, 'utf8')
			.then(value => value.trim())
			.catch(() => '');
		return /^\d+(?:\.\d+){1,3}$/.test(storedVersion)
			? storedVersion
			: app.getVersion();
	}

	#setStatus(status: LauncherUpdaterStatus) {
		this._value = status;
		this._notifyObservers(status);
	}

	#handleError(error: unknown) {
		void Logger.log('Launcher auto-update failed', 'error', error);
		this.#setStatus({
			state: 'error',
			message:
				'Launcher auto-update is unavailable. You can keep using this version.'
		});
	}

	async check() {
		if (!app.isPackaged) {
			void Logger.log('Skipping launcher auto-update in development mode.');
			return false;
		}
		const runtime = await getCompatibilityRuntime();
		if (!process.env.PORTABLE_EXECUTABLE_FILE && !runtime.isWine) {
			this.#handleError(
				new Error(
					'Launcher auto-update requires the standalone portable build.'
				)
			);
			return false;
		}
		if (
			this._value.state === 'checking' ||
			this._value.state === 'downloading' ||
			this._value.state === 'ready' ||
			this._value.state === 'installing'
		)
			return false;

		const updateDir = path.join(Preferences.userDataDir, 'launcher-update');
		try {
			this.#setStatus({
				state: 'checking',
				message: 'Checking for launcher updates...'
			});

			const versionResponse = await updateFetch(
				resolveLauncherUrl(LAUNCHER_VERSION_FILE)
			);
			if (!versionResponse.ok) {
				throw new Error(
					`Launcher version check failed (${versionResponse.status}).`
				);
			}

			const remoteVersion = (await versionResponse.text()).trim();
			if (!/^\d+(?:\.\d+){1,3}$/.test(remoteVersion)) {
				throw new Error('Launcher version file is invalid.');
			}

			const currentVersion = await this.getInstalledVersion();
			if (compareVersions(remoteVersion, currentVersion) <= 0) {
				void Logger.log(`Launcher ${currentVersion} is up to date.`);
				await fs.remove(updateDir);
				this.#downloadedLauncherVersion = undefined;
				this.#setStatus({ state: 'idle' });
				return false;
			}

			void Logger.log(
				`Launcher update ${remoteVersion} is available (current ${currentVersion}).`
			);
			if (runtime.isWine) {
				this.#downloadedLauncherPath = undefined;
				this.#downloadedLauncherVersion = undefined;
				await fs.remove(updateDir);
				this.#setStatus({
					state: 'manualUpdate',
					message: `Automatic installation is unavailable under ${
						runtime.isProton ? 'Proton' : 'Wine'
					}.`,
					version: remoteVersion,
					downloadUrl: resolveLauncherUrl(LAUNCHER_ARCHIVE_FILE)
				});
				return true;
			}

			this.#setStatus({
				state: 'downloading',
				progress: 0,
				message: `Downloading launcher ${remoteVersion}...`,
				version: remoteVersion
			});

			await fs.remove(updateDir);
			await fs.ensureDir(updateDir);
			const archivePath = path.join(updateDir, LAUNCHER_ARCHIVE_FILE);
			await resumableFetch(
				resolveLauncherUrl(LAUNCHER_ARCHIVE_FILE),
				archivePath,
				(progress: FetchProgress) => {
					const total = progress.total + progress.initialPartial;
					const downloaded = progress.done + progress.initialPartial;
					const ratio = total > 0 ? Math.min(downloaded / total, 1) : -1;
					this.#setStatus({
						state: 'downloading',
						progress: ratio,
						message:
							ratio === -1
								? `Downloading launcher ${remoteVersion}...`
								: `Downloading launcher ${remoteVersion}... ${Math.round(
										ratio * 100
								  )}%`,
						version: remoteVersion
					});
				},
				{ throttle: 500, force: true }
			);

			this.#downloadedLauncherPath = await extractLauncher(
				archivePath,
				updateDir
			);
			this.#downloadedLauncherVersion = remoteVersion;
			await fs.remove(archivePath);
			this.#setStatus({
				state: 'ready',
				progress: 1,
				message: 'Launcher update ready. Restart to install it.',
				version: remoteVersion
			});
			return true;
		} catch (error) {
			this.#downloadedLauncherPath = undefined;
			this.#downloadedLauncherVersion = undefined;
			await fs.remove(updateDir).catch(() => undefined);
			this.#handleError(error);
			return false;
		}
	}

	async restartAndInstall() {
		const targetPath = process.env.PORTABLE_EXECUTABLE_FILE;
		if (
			!targetPath ||
			!this.#downloadedLauncherPath ||
			!this.#downloadedLauncherVersion ||
			this._value.state !== 'ready'
		)
			return false;

		this.#setStatus({
			...this._value,
			state: 'installing',
			message: 'Restarting to install launcher update...'
		});

		const backupPath = `${targetPath}.old`;
		const candidatePath = `${targetPath}.update`;
		const updateLogPath = path.join(
			path.dirname(targetPath),
			'.launcher',
			'launcher-update.log'
		);
		const installedVersionPath = path.join(
			Preferences.userDataDir,
			LAUNCHER_VERSION_FILE
		);
		const readyPath = path.join(
			path.dirname(targetPath),
			'.launcher',
			`launcher-update-ready-${process.pid}.tmp`
		);
		const scriptPath = path.join(
			path.dirname(targetPath),
			'.launcher',
			`launcher-update-${process.pid}.ps1`
		);
		const helperErrorPath = path.join(
			path.dirname(targetPath),
			'.launcher',
			'launcher-update-helper-error.log'
		);
		const brokerPath = path.join(
			path.dirname(targetPath),
			'.launcher',
			`launcher-update-broker-${process.pid}.ps1`
		);
		await fs.remove(readyPath).catch(() => undefined);
		await fs.remove(helperErrorPath).catch(() => undefined);
		const command = `
$ErrorActionPreference = 'Stop'
$source = '${escapeForPowerShell(this.#downloadedLauncherPath)}'
$target = '${escapeForPowerShell(targetPath)}'
$backup = '${escapeForPowerShell(backupPath)}'
$candidate = '${escapeForPowerShell(candidatePath)}'
$log = '${escapeForPowerShell(updateLogPath)}'
$ready = '${escapeForPowerShell(readyPath)}'
$versionFile = '${escapeForPowerShell(installedVersionPath)}'
$newVersion = '${escapeForPowerShell(this.#downloadedLauncherVersion)}'
$scriptPath = '${escapeForPowerShell(scriptPath)}'
$brokerPath = '${escapeForPowerShell(brokerPath)}'
$helperErrorPath = '${escapeForPowerShell(helperErrorPath)}'
$launcherPid = ${process.pid}
$wrapperPid = ${process.ppid}
$hadPreviousVersion = Test-Path -LiteralPath $versionFile
$previousVersion = if ($hadPreviousVersion) { [System.IO.File]::ReadAllText($versionFile) } else { $null }

function Write-UpdateLog([string] $message) {
    $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'
    Add-Content -LiteralPath $log -Value "[$timestamp] $message" -ErrorAction SilentlyContinue
}

Write-UpdateLog "Waiting for launcher PID $launcherPid and portable wrapper PID $wrapperPid."
Set-Content -LiteralPath $ready -Value $PID -Force
Wait-Process -Id $launcherPid,$wrapperPid -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $ready -Force -ErrorAction SilentlyContinue
Write-UpdateLog 'Launcher processes exited; beginning replacement.'

for ($attempt = 1; $attempt -le 120; $attempt++) {
    try {
        if (-not (Test-Path -LiteralPath $source)) {
            throw 'Downloaded launcher is missing.'
        }
        if (Test-Path -LiteralPath $candidate) {
            Remove-Item -LiteralPath $candidate -Force
        }
        Copy-Item -LiteralPath $source -Destination $candidate -Force
        $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $candidateHash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
        if ($sourceHash -ne $candidateHash) {
            throw 'Staged launcher hash does not match the downloaded launcher.'
        }
        if (Test-Path -LiteralPath $backup) {
            Remove-Item -LiteralPath $backup -Force
        }
        if (Test-Path -LiteralPath $target) {
            Move-Item -LiteralPath $target -Destination $backup -Force
        }
		Move-Item -LiteralPath $candidate -Destination $target -Force
		Set-Content -LiteralPath $versionFile -Value $newVersion -Encoding ASCII -NoNewline
		$launchedProcess = Start-Process -FilePath $target -PassThru
        Write-UpdateLog "Replacement succeeded on attempt $attempt. Relaunched PID $($launchedProcess.Id)."
        if (Test-Path -LiteralPath $backup) {
            Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $source) {
            Remove-Item -LiteralPath $source -Force -ErrorAction SilentlyContinue
        }
        Write-UpdateLog 'Launcher update completed successfully.'
		Remove-Item -LiteralPath $helperErrorPath -Force -ErrorAction SilentlyContinue
		Remove-Item -LiteralPath $brokerPath -Force -ErrorAction SilentlyContinue
		Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
        exit 0
	} catch {
		Write-UpdateLog "Attempt $attempt failed: $($_.Exception.Message)"
		if ($hadPreviousVersion) {
			[System.IO.File]::WriteAllText($versionFile, $previousVersion, [System.Text.Encoding]::ASCII)
		} elseif (Test-Path -LiteralPath $versionFile) {
			Remove-Item -LiteralPath $versionFile -Force -ErrorAction SilentlyContinue
		}
        if (Test-Path -LiteralPath $backup) {
            if (Test-Path -LiteralPath $target) {
                Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
            }
            Move-Item -LiteralPath $backup -Destination $target -Force
        }
        if (Test-Path -LiteralPath $candidate) {
            Remove-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
        }
        Start-Sleep -Milliseconds 500
    }
}

Write-UpdateLog 'Launcher update failed after 120 attempts. The original launcher was preserved.'
exit 1
`;
		const utf16WithBom = Buffer.concat([
			Buffer.from([0xff, 0xfe]),
			Buffer.from(command, 'utf16le')
		]);
		await fs.writeFile(scriptPath, utf16WithBom);
		const helperCommandLine = `powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File "${scriptPath.replace(
			/"/g,
			'\\"'
		)}"`;
		const brokerCommand = `
$ErrorActionPreference = 'Stop'
$commandLine = '${escapeForPowerShell(helperCommandLine)}'
$result = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $commandLine }
if ($result.ReturnValue -ne 0) {
    throw "Win32_Process.Create failed with code $($result.ReturnValue)."
}
`;
		const brokerUtf16WithBom = Buffer.concat([
			Buffer.from([0xff, 0xfe]),
			Buffer.from(brokerCommand, 'utf16le')
		]);
		await fs.writeFile(brokerPath, brokerUtf16WithBom);
		const helperErrorFd = fs.openSync(helperErrorPath, 'a');

		const updater = spawn(
			'powershell.exe',
			[
				'-NoLogo',
				'-NoProfile',
				'-NonInteractive',
				'-ExecutionPolicy',
				'Bypass',
				'-WindowStyle',
				'Hidden',
				'-File',
				brokerPath
			],
			{
				detached: false,
				stdio: ['ignore', 'ignore', helperErrorFd],
				windowsHide: true
			}
		);
		fs.closeSync(helperErrorFd);

		const helperReady = await new Promise<boolean>(resolve => {
			let settled = false;
			const settle = (ready: boolean) => {
				if (settled) return;
				settled = true;
				resolve(ready);
			};

			updater.once('error', error => {
				this.#handleError(error);
				settle(false);
			});

			void (async () => {
				for (let attempt = 0; attempt < 100; attempt += 1) {
					if (await fs.pathExists(readyPath)) {
						settle(true);
						return;
					}
					await new Promise(resolveDelay => setTimeout(resolveDelay, 100));
				}
				settle(false);
			})();
		});

		if (!helperReady) {
			updater.kill();
			await fs.remove(readyPath).catch(() => undefined);
			const helperError = await fs
				.readFile(helperErrorPath, 'utf8')
				.then(value => value.trim())
				.catch(() => '');
			this.#handleError(
				new Error(
					helperError ||
						'Launcher update helper did not start within 10 seconds.'
				)
			);
			return false;
		}

		void Logger.log(`Launcher update helper started (PID ${updater.pid}).`);
		updater.unref();
		setImmediate(() => app.quit());
		return true;
	}
}

const LauncherUpdater = new LauncherUpdaterClass();
export default LauncherUpdater;

import { join } from 'path';

import fs from 'fs-extra';
import { app, shell, BrowserWindow, nativeImage } from 'electron';
import { electronApp, optimizer, is } from '@electron-toolkit/utils';
import { createIPCHandler } from 'electron-trpc/main';

import icon from '~build/icon.png?asset';

import Preferences from './modules/preferences';
import Updater from './modules/updater';
import LauncherUpdater from './modules/launcherUpdater';
import Logger from './modules/logger';
import {
	getCompatibilityDiagnostics,
	getCompatibilityRuntimeSync
} from './modules/compatibility';
import { appRouter } from './api/root';

export let mainWindow: BrowserWindow | null = null;
let isQuitting = false;

// Chromium's process sandbox and its out-of-process GPU are not implemented by
// Wine, so under Proton the child processes die on spawn and the launcher hangs
// with no window. These switches have to be applied before app.whenReady().
const isTruthyEnv = (value?: string) => /^(1|true|yes|on)$/i.test(value ?? '');
const isFalsyEnv = (value?: string) => /^(0|false|no|off)$/i.test(value ?? '');

const applyCompatibilitySwitches = () => {
	const runtime = getCompatibilityRuntimeSync();
	const override = process.env.CENTURION_COMPAT_MODE;

	const enabled = isFalsyEnv(override)
		? false
		: isTruthyEnv(override) ||
		  runtime.isWine ||
		  process.argv.includes('--compat-mode');

	if (!enabled) return { runtime, compatibilitySwitches: false };

	app.commandLine.appendSwitch('no-sandbox');
	app.commandLine.appendSwitch('disable-gpu-sandbox');
	app.commandLine.appendSwitch('in-process-gpu');
	app.commandLine.appendSwitch('disable-gpu-compositing');
	app.commandLine.appendSwitch(
		'disable-features',
		'HardwareMediaKeyHandling,MediaSessionService'
	);
	app.disableHardwareAcceleration();

	return { runtime, compatibilitySwitches: true };
};

// Written synchronously so that even an exit before app.whenReady() leaves a
// trace. Everything else in this file logs too late to explain a silent quit.
// Under Proton the working directory is not obvious and a single location is
// easy to miss, so every candidate is written and stderr always gets a copy —
// stderr is what Proton captures in its own log.
const breadcrumbPaths = () => {
	const candidates: (() => string)[] = [
		() => join(Preferences.userDataDir, 'startup.log'),
		() => join(app.getPath('userData'), 'startup.log'),
		() => join(app.getPath('temp'), 'centurion-startup.log')
	];

	const paths = new Set<string>();
	for (const candidate of candidates) {
		try {
			paths.add(candidate());
		} catch (e) {
			// A path that cannot be resolved is simply skipped.
		}
	}
	return [...paths];
};

const breadcrumb = (stage: string, detail?: Record<string, unknown>) => {
	const line = `[${new Date().toISOString()}] ${stage}${
		detail ? ` ${JSON.stringify(detail)}` : ''
	}\n`;

	try {
		process.stderr.write(`[centurion] ${line}`);
	} catch (e) {
		// No console attached; the files below still carry the trace.
	}

	const failures: string[] = [];
	for (const target of breadcrumbPaths()) {
		try {
			fs.ensureDirSync(join(target, '..'));
			fs.appendFileSync(target, line);
		} catch (e) {
			failures.push(`${target}: ${(e as Error).message}`);
		}
	}

	// Silence here is what made the last round undiagnosable.
	if (failures.length) {
		try {
			process.stderr.write(
				`[centurion] breadcrumb write failed -> ${failures.join(' | ')}\n`
			);
		} catch (e) {
			// Nothing left to report through.
		}
	}
};

breadcrumb('boot', {
	pid: process.pid,
	argv: process.argv.slice(1),
	exe: app.getPath('exe'),
	cwd: process.cwd(),
	userDataDir: Preferences.userDataDir
});

const compatibility = applyCompatibilitySwitches();

breadcrumb('compatibility', {
	layer: compatibility.runtime.compatibilityLayer,
	switches: compatibility.compatibilitySwitches
});

export const getMainWindow = () => {
	const window = mainWindow;
	return window && !window.isDestroyed() ? window : null;
};

// build/icon.png is 2048x2048 for the installer. Wine publishes the window icon
// as _NET_WM_ICON, and at that size the X request (16MB) exceeds the protocol
// maximum, which Xlib treats as fatal — the process dies while the window is
// being created. Window icons are never drawn above ~48px anyway.
const windowIcon = (() => {
	try {
		const image = nativeImage.createFromPath(icon);
		return image.isEmpty() ? undefined : image.resize({ width: 256, height: 256 });
	} catch (e) {
		return undefined;
	}
})();

const hasSingleInstanceLock = app.requestSingleInstanceLock();

breadcrumb('single-instance', { acquired: hasSingleInstanceLock });

if (!hasSingleInstanceLock) {
	// Wine keeps hung processes alive under wineserver, and Chromium's process
	// singleton then hands the lock to a zombie that owns no window. Exiting
	// there makes the launcher permanently unstartable, so under a
	// compatibility layer a lost lock is treated as stale. This keys off the
	// detected runtime rather than whether the switches were applied, so that
	// forcing switches on Windows does not also weaken the instance guard, and
	// disabling them under Wine does not restore the silent exit.
	if (!compatibility.runtime.isWine) {
		app.quit();
		process.exit(0);
	}

	breadcrumb('single-instance-ignored', {
		reason: 'compatibility layer, assuming stale lock'
	});
}

app.on('second-instance', () => {
	const window = getMainWindow();
	if (!window) {
		if (!isQuitting && app.isReady()) void createWindow();
		return;
	}

	if (window.isMinimized()) window.restore();
	window.show();
	window.focus();
});

async function createWindow() {
	if (isQuitting || getMainWindow()) return;

	const { rememberPosition, windowPosition } = Preferences.data;

	const position = rememberPosition
		? windowPosition
		: { width: 800, height: 600 };

	// Create the browser window.
	const window = new BrowserWindow({
		...position,
		minWidth: 800,
		minHeight: 600,
		icon: windowIcon ?? icon,
		frame: false,
		webPreferences: {
			preload: join(__dirname, '../preload/index.js'),
			contextIsolation: true,
			sandbox: false,
			devTools: import.meta.env.MODE !== 'production'
		}
	});
	mainWindow = window;

	createIPCHandler({ router: appRouter, windows: [window] });

	window.on('ready-to-show', () => {
		if (!window.isDestroyed()) window.show();
	});

	// Software rendering under Wine can miss the first paint. Never leave the
	// process alive behind an invisible window.
	const showFallback = setTimeout(() => {
		if (window.isDestroyed() || window.isVisible()) return;
		void Logger.log('Window did not paint in time, forcing show', 'warning');
		window.show();
	}, 10000);

	window.webContents.on('did-fail-load', (_event, code, description, url) => {
		void Logger.log(
			`Renderer failed to load ${url}: ${description} (${code})`,
			'error'
		);
	});

	window.webContents.on('render-process-gone', (_event, details) => {
		void Logger.log('Renderer process gone', 'error', details);
	});

	window.webContents.setWindowOpenHandler(details => {
		shell.openExternal(details.url);
		return { action: 'deny' };
	});

	window.on('close', () => {
		if (window.isDestroyed()) return;
		const [x = 0, y = 0] = window.getPosition();
		const [width = 0, height = 0] = window.getSize();
		Preferences.data = { windowPosition: { x, y, width, height } };
	});

	window.on('closed', () => {
		clearTimeout(showFallback);
		// Only safe once the window is gone; clearing while a renderer is
		// subscribed permanently silences its status updates.
		Updater.clearObservers();
		if (mainWindow === window) mainWindow = null;
	});

	// HMR for renderer base on electron-vite cli.
	// Load the remote URL for development or the local html file for production.
	if (is.dev && process.env.ELECTRON_RENDERER_URL) {
		window.loadURL(process.env.ELECTRON_RENDERER_URL);
	} else {
		window.loadFile(join(__dirname, '../renderer/index.html'));
	}
}

// This method will be called when Electron has finished
// initialization and is ready to create browser windows.
// Some APIs can only be used after this event occurs.
app.whenReady().then(async () => {
	breadcrumb('ready');

	// Initialization
	Preferences.data = await Preferences.load();
	const diagnostics = {
		...(await getCompatibilityDiagnostics()),
		compatibilitySwitches: compatibility.compatibilitySwitches,
		packaged: app.isPackaged,
		portable: Preferences.data.isPortable ?? false
	};
	void Logger.log('Runtime diagnostics', undefined, diagnostics);
	// Written eagerly so a startup hang still leaves something to look at; the
	// regular log is only flushed on exit.
	void Logger.saveDiagnostics(diagnostics);

	// Set app user model id for windows
	electronApp.setAppUserModelId('com.centurionpvp.launcher');

	// Default open or close DevTools by F12 in development
	// and ignore CommandOrControl + R in production.
	// see https://github.com/alex8088/electron-toolkit/tree/master/packages/utils
	app.on('browser-window-created', (_, window) => {
		optimizer.watchWindowShortcuts(window);
	});

	app.on('child-process-gone', (_event, details) => {
		void Logger.log(`Child process gone: ${details.type}`, 'error', details);
	});

	await createWindow();
	breadcrumb('window-created', { visible: getMainWindow()?.isVisible() });
	void LauncherUpdater.check();
	void Updater.verify();
});

// Quit when all windows are closed
app.on('before-quit', () => {
	isQuitting = true;
});

app.on('window-all-closed', async () => {
	isQuitting = true;
	await Logger.saveLog();
	app.quit();
});

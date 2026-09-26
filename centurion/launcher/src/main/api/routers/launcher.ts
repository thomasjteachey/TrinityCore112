import path from 'path';
import { spawn } from 'child_process';

import fs from 'fs-extra';

import Preferences from '~main/modules/preferences';
import { getMainWindow } from '~main/index';
import Logger from '~main/modules/logger';
import { isGameRunning, trackGameProcess } from '~main/modules/updater';
import { patchConfig } from '~main/modules/patcher';
import { getCompatibilityRuntime } from '~main/modules/compatibility';

import { createTRPCRouter, publicProcedure } from '../trpc';

export const launcherRouter = createTRPCRouter({
	start: publicProcedure.mutation(async () => {
		const { cleanWdb, reopenLauncher, clientDir } = Preferences.data;
		if (!clientDir) return false;

		const clientPath = path.join(clientDir, 'WoW.exe');
		Logger.log(`Launching ${clientPath}...`);
		if (await isGameRunning()) return false;

                if (cleanWdb) {
                        Logger.log('Cleaning up WDB...');
                        await fs.remove(path.join(clientDir, 'WDB'));
                }

                Logger.log('Cleaning up Cache...');
                await fs.remove(path.join(clientDir, 'Cache'));

		// Make sure config.wtf is correct
		Logger.log('Checking Config.wtf...');
		await patchConfig();

		Logger.log('Launching WoW...');
		const runtime = await getCompatibilityRuntime();
		let gameProcess;
		try {
			gameProcess = spawn(clientPath, {
				cwd: clientDir,
				detached: !reopenLauncher
			});
			await new Promise<void>((resolve, reject) => {
				gameProcess.once('spawn', resolve);
				gameProcess.once('error', reject);
			});
		} catch (error) {
			Logger.log('Failed to launch WoW', 'error', error);
			throw new Error(
				runtime.isWine
					? 'Failed to launch WoW through Proton/Wine. Verify the Steam compatibility tool and client location.'
					: 'Failed to launch WoW. Verify the client location and permissions.'
			);
		}

		trackGameProcess(gameProcess);

		if (!reopenLauncher) {
			getMainWindow()?.close();
			return true;
		}

		getMainWindow()?.hide();
		gameProcess.on('exit', (code, signal) => {
			Logger.log('WoW stopped');
			if (code && code !== 0) {
				void Logger.log(
					`WoW exited with code ${code}${signal ? ` (${signal})` : ''}.`,
					'warning'
				);
			}
			getMainWindow()?.show();
		});
		return true;
	})
});

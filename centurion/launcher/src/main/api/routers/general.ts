import { dialog, shell } from 'electron';
import { z } from 'zod';

import { getMainWindow } from '~main/index';
import LauncherUpdater from '~main/modules/launcherUpdater';

import { createTRPCRouter, publicProcedure } from '../trpc';

export const generalRouter = createTRPCRouter({
	version: publicProcedure.query(() => LauncherUpdater.getInstalledVersion()),
	quit: publicProcedure.mutation(() => getMainWindow()?.close()),
	minimize: publicProcedure.mutation(() => getMainWindow()?.minimize()),
	openLink: publicProcedure
		.input(z.string().url())
		.mutation(({ input }) => shell.openExternal(input)),
	dragWindow: publicProcedure
		.input(z.object({ x: z.number(), y: z.number() }))
		.mutation(({ input }) => {
			const window = getMainWindow();
			if (!window) return;
			const [x = 0, y = 0] = window.getPosition();
			window.setPosition(x + input.x, y + input.y);
		}),
	filePicker: publicProcedure
		.input(
			z.object({
				title: z.string().optional(),
				message: z.string().optional(),
				filters: z
					.array(
						z.object({
							name: z.string(),
							extensions: z.array(z.string())
						})
					)
					.optional(),
				properties: z
					.array(
						z.enum([
							'openDirectory',
							'openFile',
							'multiSelections',
							'showHiddenFiles',
							'createDirectory',
							'promptToCreate',
							'noResolveAliases',
							'treatPackageAsDirectory',
							'dontAddToRecent'
						])
					)
					.optional()
			})
		)
		.mutation(async ({ input }) => {
			const window = getMainWindow();
			if (!window) return { canceled: true } as const;
			const { canceled, filePaths } = await dialog.showOpenDialog(
				window,
				input
			);

			return canceled
				? ({ canceled: true } as const)
				: ({
						canceled: false,
						path: filePaths as [string, ...string[]]
				  } as const);
		})
});

import LauncherUpdater from '~main/modules/launcherUpdater';

import { createTRPCRouter, publicProcedure } from '../trpc';

export const launcherUpdaterRouter = createTRPCRouter({
	check: publicProcedure.mutation(() => LauncherUpdater.check()),
	restartAndInstall: publicProcedure.mutation(() =>
		LauncherUpdater.restartAndInstall()
	),
	observe: publicProcedure.subscription(() => LauncherUpdater.observe())
});

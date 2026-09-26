import { createTRPCRouter } from './trpc';
import { launcherRouter } from './routers/launcher';
import { updaterRouter } from './routers/updater';
import { generalRouter } from './routers/general';
import { preferencesRouter } from './routers/preferences';
import { launcherUpdaterRouter } from './routers/launcherUpdater';

export const appRouter = createTRPCRouter({
	general: generalRouter,
	preferences: preferencesRouter,
	launcher: launcherRouter,
	launcherUpdater: launcherUpdaterRouter,
	updater: updaterRouter
});

export type AppRouter = typeof appRouter;

import { z } from 'zod';

import Updater from '~main/modules/updater';

import { createTRPCRouter, publicProcedure } from '../trpc';

export const updaterRouter = createTRPCRouter({
	invalidate: publicProcedure.mutation(() => Updater.invalidate()),
	verify: publicProcedure.mutation(() => Updater.verify()),
	update: publicProcedure
		.input(z.boolean().optional())
		.mutation(({ input }) => Updater.update(input)),
        observe: publicProcedure.subscription(() => Updater.observe())
});
